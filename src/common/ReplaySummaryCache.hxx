#pragma once
#include "ReplaySlots.hxx"
#include "ReplayInputs.hxx"
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#ifdef _WIN32
#include "CoordinationBytes.hxx"
#endif

namespace sf4e { namespace replayfiles {
// Worker-owned and bounded. Each entry is a summary under the SHA-256 of the
// file's whole contents, never the contents themselves, so equal contents
// alone permit reuse. Listings read the archive in the same order every time,
// so the least recently used entry goes only if this listing has not used it;
// otherwise the new summary is not kept, and a larger archive still reuses
// kCapacity summaries a listing. The worker calls NextListing before each.
class ArchiveSummaryCache {
public:
 static constexpr std::size_t kCapacity = 1024;
 void NextListing() { ++listing_; }
 std::optional<replayinputs::Summary> Read(const replayslots::Bytes& bytes) {
  const std::string key = Key(bytes);
  const auto at = key.empty() ? index_.end() : index_.find(key);
  if (at != index_.end()) {
   at->second->listing = listing_;
   entries_.splice(entries_.end(), entries_, at->second);
   return at->second->summary;
  }
  ++parses_;
  replayinputs::Match match;
  std::optional<replayinputs::Summary> summary;
  if (replayinputs::Parse(bytes.data(), bytes.size(), match)) summary = replayinputs::Summarize(match);
  // A file that could not be hashed is summed every time.
  if (key.empty()) return summary;
  if (entries_.size() == kCapacity) {
   if (entries_.front().listing == listing_) return summary;
   index_.erase(entries_.front().key);
   entries_.pop_front();
  }
  entries_.push_back({key, summary, listing_});
  index_.emplace(key, std::prev(entries_.end()));
  return summary;
 }
 std::size_t Size() const { return entries_.size(); }
 std::size_t Parses() const { return parses_; }
 std::size_t KeyBytes() const { std::size_t bytes = 0; for (const auto& entry : entries_) bytes += entry.key.size(); return bytes; }
private:
 static std::string Key(const replayslots::Bytes& bytes) {
  const std::string contents(bytes.begin(), bytes.end());
#ifdef _WIN32
  return coordination::Sha256(contents);
#else
  // Off Windows only the tests use this cache; its key is the contents.
  return contents;
#endif
 }
 struct Entry { std::string key; std::optional<replayinputs::Summary> summary; std::uint64_t listing; };
 std::list<Entry> entries_;
 std::unordered_map<std::string, std::list<Entry>::iterator> index_;
 std::uint64_t listing_ = 0;
 std::size_t parses_ = 0;
};

} }
