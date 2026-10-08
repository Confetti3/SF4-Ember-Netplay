#include "../common/ReplayArchiveIndex.hxx"
#include "test_support.hxx"
#include <future>

using namespace sf4e::replayfiles;
static void TestBoundedPayloads() {
 ArchiveSummaryCache cache;
 sf4e::replayslots::Bytes bytes(sf4e::replayslots::kLargestReplay, 0);
 for (unsigned replay = 0; replay < 10000; ++replay) {
  sf4e::replayslots::WriteU32(bytes.data(), replay);
  cache.Read(bytes);
  CHECK(cache.Size() <= ArchiveSummaryCache::kCapacity);
  CHECK(cache.PayloadBytes() <= ArchiveSummaryCache::kCapacity * bytes.size());
 }
 CHECK(cache.Size() == ArchiveSummaryCache::kCapacity);
 CHECK(cache.PayloadBytes() == ArchiveSummaryCache::kCapacity * bytes.size());
 cache.Read(bytes); // exact content reuse adds no second representation
 CHECK(cache.Size() == ArchiveSummaryCache::kCapacity);
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
int main() {
 TestBoundedPayloads(); TestCrcBucketsAndConcurrentPublication();
 std::printf("replay_archive_index_test: all tests passed\n");
}
