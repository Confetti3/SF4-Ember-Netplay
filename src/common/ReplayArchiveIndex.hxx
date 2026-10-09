#pragma once
#include "ReplayFileSafety.hxx"
#include "ReplayInputs.hxx"
#include <map>
#include <memory>
#include <mutex>
#include <list>

namespace sf4e { namespace replayfiles {
// Lightweight immutable CRC buckets. Consumers copy only the selected bucket,
// outside the publication lock. No replay bytes ever enter this index.
class ArchiveCandidates {
public:
 using Paths = std::vector<std::filesystem::path>;
 using Bucket = std::shared_ptr<const Paths>;
 using Map = std::map<std::uint32_t, Bucket>;
 Paths Candidates(std::uint32_t crc) const {
  Bucket bucket;
  { std::lock_guard<std::mutex> lock(mutex_); const auto at = files_.find(crc); if (at != files_.end()) bucket = at->second; }
  return bucket ? *bucket : Paths{};
 }
 void Add(std::uint32_t crc, const std::filesystem::path& path) {
  // Build a bucket outside the consumer lock; retry if another publisher won.
  for (;;) {
   Bucket before;
   { std::lock_guard<std::mutex> lock(mutex_); before = files_[crc]; }
   Paths paths = before ? *before : Paths{};
   if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
   auto after = std::make_shared<const Paths>(std::move(paths));
   std::lock_guard<std::mutex> lock(mutex_);
   if (files_[crc] != before) continue;
   if (refreshing_) added_[crc] = after;
   files_[crc] = std::move(after);
   return;
  }
 }
 template<class Scan> void Refresh(const Scan& scan) {
  std::lock_guard<std::mutex> worker(workerMutex_);
  { std::lock_guard<std::mutex> lock(mutex_); refreshing_ = true; }
  Map seen;
  try {
   seen = scan();
   for (;;) {
    Map added;
    {
     std::lock_guard<std::mutex> lock(mutex_);
     if (added_.empty()) { files_.swap(seen); refreshing_ = false; break; }
     added.swap(added_);
    }
    for (const auto& item : added) {
     Paths paths = seen[item.first] ? *seen[item.first] : Paths{};
     for (const auto& path : *item.second) if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
     seen[item.first] = std::make_shared<const Paths>(std::move(paths));
    }
   }
  } catch (...) {
   Map discarded;
   { std::lock_guard<std::mutex> lock(mutex_); refreshing_ = false; discarded.swap(added_); }
   throw;
  }
  // Destruction of the previous archive-sized map is also outside the lock.
 }
private:
 mutable std::mutex mutex_;
 std::mutex workerMutex_;
 bool refreshing_ = false;
 Map files_, added_;
};

// Worker-owned FIFO, deliberately independent of archive size. Each cached
// replay has one byte representation; exact contents alone permit reuse.
class ArchiveSummaryCache {
public:
 static constexpr std::size_t kCapacity = 16;
 std::optional<replayinputs::Summary> Read(const replayslots::Bytes& bytes) {
  for (const auto& entry : entries_) if (entry.bytes == bytes) return entry.summary;
  replayinputs::Match match;
  std::optional<replayinputs::Summary> summary;
  if (replayinputs::Parse(bytes.data(), bytes.size(), match)) summary = replayinputs::Summarize(match);
  if (entries_.size() == kCapacity) entries_.pop_front();
  entries_.push_back({bytes, summary});
  return summary;
 }
 std::size_t Size() const { return entries_.size(); }
 std::size_t PayloadBytes() const { std::size_t bytes = 0; for (const auto& entry : entries_) bytes += entry.bytes.size(); return bytes; }
private:
 struct Entry { replayslots::Bytes bytes; std::optional<replayinputs::Summary> summary; };
 std::list<Entry> entries_;
};
} }
