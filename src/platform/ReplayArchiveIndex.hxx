#pragma once
#include "../common/ReplayFileSafety.hxx"
#include "../common/ReplaySlots.hxx"
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <vector>

namespace sf4e { namespace replayfiles {
// Lightweight immutable CRC buckets. No replay bytes ever enter this index.
// Each refresh, and each copy added, publishes a new generation (Snapshot),
// which is never changed once published: whoever takes one sees its
// candidates and whether its refresh listed the archive whole together.
class ArchiveCandidates {
public:
 using Paths = std::vector<std::filesystem::path>;
 using Bucket = std::shared_ptr<const Paths>;
 using Map = std::map<std::uint32_t, Bucket>;
 struct Snapshot {
  Map files;
  bool complete = false;
  Paths Candidates(std::uint32_t crc) const {
   const auto at = files.find(crc);
   return at != files.end() && at->second ? *at->second : Paths{};
  }
 };
 using Generation = std::shared_ptr<const Snapshot>;
 Generation Current() const { std::lock_guard<std::mutex> lock(mutex_); return current_; }
 Paths Candidates(std::uint32_t crc) const { return Current()->Candidates(crc); }
 void Add(std::uint32_t crc, const std::filesystem::path& path) {
  // Build the next generation outside the lock; retry if another publisher won.
  for (;;) {
   const Generation before = Current();
   auto after = std::make_shared<Snapshot>(*before);
   Paths paths = before->Candidates(crc);
   if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
   const Bucket bucket = std::make_shared<const Paths>(std::move(paths));
   after->files[crc] = bucket;
   std::lock_guard<std::mutex> lock(mutex_);
   if (current_ != before) continue;
   if (refreshing_) added_[crc] = bucket;
   current_ = std::move(after);
   return;
  }
 }
 // scan returns the Snapshot of a whole reading of the archive; copies added
 // while it reads are kept in the generation it publishes.
 template<class Scan> void Refresh(const Scan& scan) {
  std::lock_guard<std::mutex> worker(workerMutex_);
  { std::lock_guard<std::mutex> lock(mutex_); refreshing_ = true; }
  try {
   Snapshot seen = scan();
   for (;;) {
    Map added;
    {
     std::lock_guard<std::mutex> lock(mutex_);
     if (added_.empty()) { current_ = std::make_shared<const Snapshot>(std::move(seen)); refreshing_ = false; break; }
     added.swap(added_);
    }
    for (const auto& item : added) {
     Paths paths = seen.Candidates(item.first);
     for (const auto& path : *item.second) if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
     seen.files[item.first] = std::make_shared<const Paths>(std::move(paths));
    }
   }
  } catch (...) {
   Map discarded;
   { std::lock_guard<std::mutex> lock(mutex_); refreshing_ = false; discarded.swap(added_); }
   throw;
  }
 }
private:
 mutable std::mutex mutex_;
 std::mutex workerMutex_;
 bool refreshing_ = false;
 Generation current_ = std::make_shared<const Snapshot>();
 Map added_;
};

// A pass's view of the index: one generation, taken once, so the candidates
// it finds and whether they are all of them come from the same refresh,
// whatever refresh lands meanwhile; and the copies the pass makes, which also
// go into the index for later passes and listings.
class ArchiveView {
public:
 explicit ArchiveView(ArchiveCandidates& index) : index_(index), taken_(index.Current()) {}
 bool Complete() const { return taken_->complete; }
 ArchiveCandidates::Paths Candidates(std::uint32_t crc) const {
  ArchiveCandidates::Paths paths = taken_->Candidates(crc);
  const auto at = added_.find(crc);
  if (at != added_.end()) for (const auto& path : at->second) if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
  return paths;
 }
 void Add(std::uint32_t crc, const std::filesystem::path& path) { added_[crc].push_back(path); index_.Add(crc, path); }
private:
 ArchiveCandidates& index_;
 const ArchiveCandidates::Generation taken_;
 std::map<std::uint32_t, ArchiveCandidates::Paths> added_;
};

// Every archive file a replay may be in, by CRC, for
// ArchiveCandidates::Refresh: each file that reads whole, which `read` gets
// too, and each of Ember's files whose name gives a CRC (ReplaySlots.hxx:
// ParseArchiveName) but that cannot be read now, which may hold a replay with
// that CRC. complete says nothing was missed: false when a folder could not
// be listed or entered, or a replay file that could not be read gives no CRC
// to keep it under. No archive yet is an empty one, and complete.
template<class Read> ArchiveCandidates::Snapshot ScanArchive(const std::filesystem::path& archive, const Read& read) {
 std::map<std::uint32_t, ArchiveCandidates::Paths> paths;
 ArchiveCandidates::Snapshot seen;
 bool& complete = seen.complete;
 complete = true;
 std::error_code error;
 std::filesystem::recursive_directory_iterator at(archive, error);
 if (error) {
  std::error_code probe;
  complete = !std::filesystem::exists(archive, probe) && !probe;
  return seen;
 }
 for (const std::filesystem::recursive_directory_iterator end; at != end;) {
  const std::filesystem::path path = at->path();
  if (path.extension() == L".emberreplay" || path.extension() == L".usf4replay") {
   ArchiveFile file;
   const bool inRoot = path.parent_path() == archive;
   const ArchiveState state = ReadArchive(path, inRoot, file);
   std::uint64_t time = 0;
   std::uint32_t crc = 0;
   if (state == ArchiveState::Valid) { read(path, file); paths[file.crc].push_back(path); }
   else if (state == ArchiveState::Unreadable) {
    if (inRoot && replayslots::ParseArchiveName(path.filename().wstring(), time, crc)) paths[crc].push_back(path);
    else complete = false;
   }
  }
  at.increment(error);
  if (error) { complete = false; break; }
 }
 for (auto& item : paths) seen.files.emplace(item.first, std::make_shared<const ArchiveCandidates::Paths>(std::move(item.second)));
 return seen;
}

// A game slot's replay kept by its path, size and write time, so an unchanged
// slot's 50 KB are read and summed once. Only a successful read is kept: a
// slot the game holds open is read again on the next scan. The lock is held
// only to look and to store, never while a file is read.
class SlotFileCache {
public:
 template<class Reader> std::optional<replayslots::Bytes> Read(const std::filesystem::path& file, const Reader& read) {
  std::error_code failed;
  const std::uintmax_t size = std::filesystem::file_size(file, failed);
  if (failed) return std::nullopt;
  const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, failed);
  if (failed) return std::nullopt;
  {
   std::lock_guard<std::mutex> lock(mutex_);
   const auto at = kept_.find(file);
   if (at != kept_.end() && at->second.size == size && at->second.written == written) return at->second.replay;
  }
  std::optional<replayslots::Bytes> replay = read(file);
  if (replay) { std::lock_guard<std::mutex> lock(mutex_); kept_[file] = Kept{size, written, *replay}; }
  return replay;
 }
private:
 struct Kept { std::uintmax_t size; std::filesystem::file_time_type written; replayslots::Bytes replay; };
 std::mutex mutex_;
 std::map<std::filesystem::path, Kept> kept_;
};
} }
