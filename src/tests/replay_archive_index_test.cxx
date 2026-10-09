#include "../common/ReplaySummaryCache.hxx"
#include "../common/ReplayFileSafety.hxx"
#include "../platform/ReplayArchiveIndex.hxx"
#include "test_support.hxx"
#include <chrono>
#include <fstream>
#include <future>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace sf4e::replayfiles;
static void TestBoundedPayloads() {
 ArchiveSummaryCache cache;
 sf4e::replayslots::Bytes bytes(sf4e::replayslots::kLargestReplay, 0);
 for (unsigned replay = 0; replay < 10000; ++replay) {
  sf4e::replayslots::WriteU32(bytes.data(), replay);
  cache.NextListing(); // one file a listing: the oldest entry is always free to go
  cache.Read(bytes);
  CHECK(cache.Size() <= ArchiveSummaryCache::kCapacity);
 }
 CHECK(cache.Size() == ArchiveSummaryCache::kCapacity);
#ifdef _WIN32
 // Only the digests are kept, whatever the replays' size.
 CHECK(cache.KeyBytes() == ArchiveSummaryCache::kCapacity * 64);
#endif
 const auto parses = cache.Parses();
 cache.Read(bytes); // exact content reuse adds no second representation
 CHECK(cache.Size() == ArchiveSummaryCache::kCapacity && cache.Parses() == parses);
}
static void TestCrcBucketsAndConcurrentPublication() {
 ArchiveCandidates index;
 index.Refresh([] {
  ArchiveCandidates::Map seen;
  for (unsigned crc = 0; crc < 10000; ++crc)
   seen[crc] = std::make_shared<const ArchiveCandidates::Paths>(ArchiveCandidates::Paths{std::to_string(crc)});
  return seen;
 });
 CHECK(index.Candidates(123) == ArchiveCandidates::Paths{"123"});
 CHECK(index.Candidates(10000).empty());
 auto held = index.Candidates(123);
 std::promise<void> scanning, release;
 auto gate = release.get_future();
 auto refresh = std::async(std::launch::async, [&] {
  index.Refresh([&] { scanning.set_value(); gate.wait(); return ArchiveCandidates::Map{}; });
 });
 CHECK(scanning.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 // Consumers and imports must finish while the scanning worker is blocked.
 auto consumer = std::async(std::launch::async, [&] {
  CHECK(index.Candidates(123) == held);
  index.Add(123, "new"); index.Add(123, "new"); index.Add(99999, "other");
 });
 CHECK(consumer.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 consumer.get(); release.set_value(); refresh.get();
 CHECK(held == ArchiveCandidates::Paths{"123"});
 const auto found = index.Candidates(123);
 CHECK(std::count(found.begin(), found.end(), std::filesystem::path("new")) == 1);
 CHECK(index.Candidates(99999) == ArchiveCandidates::Paths{"other"});
 CHECK(index.Candidates(124).empty());
 // A failed refresh leaves the published index usable.
 try { index.Refresh([]() -> ArchiveCandidates::Map { throw 1; }); } catch (int) {}
 CHECK(index.Candidates(99999) == ArchiveCandidates::Paths{"other"});
}
// One round of `frames` frames: same size for every count, different summaries.
static sf4e::replayslots::Bytes Parseable(std::uint32_t frames) {
 sf4e::replayslots::Bytes replay(0x320 + 0x88, 0);
 std::memcpy(replay.data(), "#BRP", 4);
 replay[8] = 1; replay[10] = 8;
 sf4e::replayslots::WriteU32(replay.data() + 0x18, 1);
 sf4e::replayslots::WriteU32(replay.data() + 0x320 + 0x7C, 6);
 const std::uint32_t stream[2] = {0, 0x400000 | (frames - 1)};
 for (auto value : stream) for (int i = 0; i < 3; i++) replay.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
 return replay;
}
// Rewrites four header bytes Parse does not read so that replay has the
// CRC of target: a real collision, which only the contents can tell apart.
static void SameCrc(sf4e::replayslots::Bytes& replay, const sf4e::replayslots::Bytes& target) {
 using sf4e::replayslots::Crc32; using sf4e::replayslots::WriteU32;
 std::uint8_t* const patch = replay.data() + 0x300;
 WriteU32(patch, 0);
 const auto base = Crc32(replay.data(), replay.size());
 std::uint32_t basis[32] = {}, masks[32] = {};
 for (int bit = 0; bit < 32; ++bit) {
  WriteU32(patch, std::uint32_t{1} << bit);
  auto column = Crc32(replay.data(), replay.size()) ^ base;
  auto mask = std::uint32_t{1} << bit;
  for (int row = 31; row >= 0; --row) if (column & (std::uint32_t{1} << row)) {
   if (basis[row]) { column ^= basis[row]; mask ^= masks[row]; }
   else { basis[row] = column; masks[row] = mask; break; }
  }
 }
 auto wanted = Crc32(target.data(), target.size()) ^ base;
 std::uint32_t value = 0;
 for (int row = 31; row >= 0; --row) if (wanted & (std::uint32_t{1} << row)) { CHECK(basis[row]); wanted ^= basis[row]; value ^= masks[row]; }
 CHECK(wanted == 0);
 WriteU32(patch, value);
 CHECK(Crc32(replay.data(), replay.size()) == Crc32(target.data(), target.size()));
}
static void TestRepeatedListingsReuseSummaries() {
 // More distinct replays than the cache used to hold, listed in the same order.
 std::vector<sf4e::replayslots::Bytes> archive;
 for (std::uint32_t replay = 0; replay < 40; ++replay) archive.push_back(Parseable(100 + replay));
 ArchiveSummaryCache cache;
 for (int listing = 0; listing < 3; ++listing) {
  cache.NextListing();
  for (std::uint32_t at = 0; at < archive.size(); ++at) {
   const auto summary = cache.Read(archive[at]);
   CHECK(summary && summary->frames == 100 + at);
  }
  CHECK(cache.Parses() == archive.size());
 }
 // Same size and CRC, different bytes: never another replay's summary.
 cache.NextListing();
 auto other = Parseable(7);
 SameCrc(other, archive[0]);
 CHECK(other != archive[0] && other.size() == archive[0].size());
 CHECK(cache.Read(other)->frames == 7 && cache.Parses() == archive.size() + 1);
 CHECK(cache.Read(archive[0])->frames == 100 && cache.Parses() == archive.size() + 1);
}
static void TestLargerArchiveKeepsSomeSummaries() {
 // An archive larger than the cache still reuses kCapacity summaries a listing.
 std::vector<sf4e::replayslots::Bytes> archive;
 for (std::uint32_t replay = 0; replay < ArchiveSummaryCache::kCapacity + 50; ++replay) archive.push_back(Parseable(100 + replay));
 ArchiveSummaryCache cache;
 for (int listing = 0; listing < 3; ++listing) {
  cache.NextListing();
  const auto before = cache.Parses();
  for (const auto& replay : archive) CHECK(cache.Read(replay));
  CHECK(cache.Size() == ArchiveSummaryCache::kCapacity);
  if (listing) CHECK(cache.Parses() - before == archive.size() - ArchiveSummaryCache::kCapacity);
 }
}
static void TestSlotReadFailuresAreRetried() {
 namespace fs = std::filesystem;
 using sf4e::replayslots::Bytes;
 const fs::path folder = fs::temp_directory_path() / ("ember-slot-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 CHECK(fs::create_directory(folder));
 const Bytes replay{'#', 'B', 'R', 'P', 1, 2, 3};
 const auto save = [&](const fs::path& path) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(replay.data()), static_cast<std::streamsize>(replay.size()));
  out.close(); CHECK(!out.fail());
 };
 const fs::path slot = folder / "280";
 save(slot);
 SlotFileCache cache;
 unsigned reads = 0;
 bool locked = true;
 const auto read = [&](const fs::path& path) -> std::optional<Bytes> { ++reads; if (locked) return std::nullopt; return sf4e::replayfiles::ReadFile(path); };
 CHECK(!cache.Read(slot, read));
 // Unchanged size and time: a failed read is not kept as "no replay".
 locked = false;
 CHECK(cache.Read(slot, read) == replay && reads == 2);
 CHECK(cache.Read(slot, read) == replay && reads == 2);
#ifdef _WIN32
 // The game holding the slot open with no sharing, through the real reader.
 const fs::path held = folder / "281";
 save(held);
 const auto real = [](const fs::path& path) { return sf4e::replayfiles::ReadFile(path); };
 const HANDLE game = CreateFileW(held.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
 CHECK(game != INVALID_HANDLE_VALUE);
 CHECK(!cache.Read(held, real));
 CHECK(CloseHandle(game));
 CHECK(cache.Read(held, real) == replay);
#endif
 fs::remove_all(folder);
}
int main() {
 TestBoundedPayloads(); TestCrcBucketsAndConcurrentPublication();
 TestRepeatedListingsReuseSummaries(); TestLargerArchiveKeepsSomeSummaries(); TestSlotReadFailuresAreRetried();
 std::printf("replay_archive_index_test: all tests passed\n");
}
