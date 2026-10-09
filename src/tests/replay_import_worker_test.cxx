#include "replay_slots_support.hxx"
#include "../common/ReplayPreparationWorker.hxx"
#include "../platform/ReplayFiles.hxx"
#include <future>
#include <chrono>
#include <fstream>
#include <atomic>
#include <stdexcept>
#ifdef _WIN32
#include "../platform/ReplayImportFreshness.hxx"
#endif

using namespace sf4e::replayfiles;
using namespace sf4e::platform::replays;
using Worker = PreparationWorker<PreparedImport>;

static void TestBlockedPreparation(bool archivePublication) {
 std::promise<void> blocked, release;
 auto gate = release.get_future();
 const auto gameThread = std::this_thread::get_id();
 std::atomic<unsigned> handled{0};
 Worker worker([&](const std::filesystem::path&) {
  CHECK(std::this_thread::get_id() != gameThread);
  // Both slot preparation and create-only durable backup publication are
  // callbacks inside this production mailbox's worker, never Request/Take.
  PreparedImport value;
  if (!archivePublication) { blocked.set_value(); gate.wait(); }
  value.imported.slot = 280;
  value.changes = {{"280", {1, 2}, {3}, true}};
  if (archivePublication) {
   const Bytes body = Replay(2, 800), exported{1, 2, 3};
   CHECK(PublishArchive(body, exported, [](ArchiveFile&) { return ArchiveState::Missing; },
    [&](const Bytes& bytes) { CHECK(bytes == exported); blocked.set_value(); gate.wait(); return true; }));
  }
  value.result = ImportResult::Done; value.notInvalidated = [] { return true; };
  return std::make_shared<const PreparedImport>(std::move(value));
 }, [](const Worker::Value&) {});
 CHECK(worker.Request("selected.emberreplay"));
 CHECK(blocked.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 // Model outer-tick dispatch: unrelated room requests and a duplicate import
 // continue while preparation/publication is deliberately blocked.
 auto dispatch = std::async(std::launch::async, [&] {
  Worker::Value value;
  for (int tick = 0; tick < 100; ++tick) {
   CHECK(!worker.Take(value)); CHECK(!worker.Request("duplicate.emberreplay")); ++handled;
  }
 });
 CHECK(dispatch.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 dispatch.get(); CHECK(handled == 100); release.set_value();
 worker.Stop(); // joins the preparation already in progress
 Worker::Value value;
 CHECK(worker.Take(value) && value && value->imported.slot == 280);
 CHECK(!worker.Take(value)); // one immutable handoff
 unsigned writes = 0, publications = 0;
 CHECK(Apply(value->changes, value->notInvalidated, [&](const std::string&, const Bytes&) { ++writes; return true; },
  [](const std::string&) { return true; }, [&] { ++publications; return true; }) == ApplyOutcome::Done);
 CHECK(writes == 1 && publications == 1);
}
static void TestFreshnessRefusesBeforeWriting() {
 std::uint64_t revision = 7;
 bool fileChanged = false, accountChanged = false;
 PreparedImport value;
 value.changes = {{"280", {1}, {2}, true}};
 value.notInvalidated = [&] { return revision == 7 && !fileChanged && !accountChanged; };
 unsigned writes = 0, removes = 0, publications = 0;
 const auto commit = [&] {
  return Apply(value.changes, value.notInvalidated, [&](const std::string&, const Bytes&) { ++writes; return true; },
   [&](const std::string&) { ++removes; return true; }, [&] { ++publications; return true; });
 };
 revision = 8; CHECK(commit() == ApplyOutcome::RejectedBeforeWrite);
 revision = 7; fileChanged = true; CHECK(commit() == ApplyOutcome::RejectedBeforeWrite);
 fileChanged = false; accountChanged = true; CHECK(commit() == ApplyOutcome::RejectedBeforeWrite);
 CHECK(writes == 0 && removes == 0 && publications == 0);
 value.notInvalidated = []() -> bool { throw std::runtime_error("freshness unavailable"); };
 CHECK(commit() == ApplyOutcome::RejectedBeforeWrite && writes == 0);
}
static void TestRecoveryPublicationIsOffThreadAndBounded() {
 std::promise<void> persisting, release;
 auto gate = release.get_future();
 std::atomic<unsigned> attempts{0};
 const auto gameThread = std::this_thread::get_id();
 Worker worker([](const std::filesystem::path&) { return std::make_shared<const PreparedImport>(); },
  [&](const Worker::Value& value) {
   CHECK(value->changes[0].before == Bytes({1, 2}));
   CHECK(std::this_thread::get_id() != gameThread);
   ++attempts; persisting.set_value(); gate.wait();
   throw std::runtime_error("permanent durable publication failure");
  });
 auto value = std::make_shared<PreparedImport>();
 value->changes = {{"280", {1, 2}, {3}, true}};
 worker.RetainRecovery(value);
 CHECK(persisting.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 auto dispatch = std::async(std::launch::async, [&] {
  Worker::Value out;
  CHECK(worker.Failed() && !worker.Request("next") && !worker.Take(out));
 });
 CHECK(dispatch.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 dispatch.get(); release.set_value(); worker.Stop();
 CHECK(attempts == 1 && worker.Failed());
}
static void TestFailedPreparationCompletes() {
 std::promise<void> started, release;
 auto gate = release.get_future();
 Worker failing([&](const std::filesystem::path&) -> Worker::Value { started.set_value(); gate.wait(); throw std::bad_alloc(); }, [](const Worker::Value&) {});
 CHECK(failing.Request("selected"));
 CHECK(started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 release.set_value(); failing.Stop();
 Worker::Value out; CHECK(failing.Take(out) && !out);
}
#ifdef _WIN32
static void TestProductionFolderFreshness() {
 namespace fs = std::filesystem;
 const auto folder = fs::temp_directory_path() / ("ember-import-freshness-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 CHECK(fs::create_directory(folder));
 {
  SaveFolderFreshness guard;
  CHECK(guard.Arm(folder) && guard.NotInvalidated());
  PreparedImport value; value.changes = {{"280", {}, {1}, false}}; value.notInvalidated = [&] { return guard.NotInvalidated(); };
  { std::ofstream changed(folder / "280", std::ios::binary); changed << "new native save"; CHECK(changed.good()); }
  // This fixture covers notifications as invalidation signals. The handoff
  // suite separately covers exact bytes when this signal is still pending.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (guard.NotInvalidated() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  CHECK(!guard.NotInvalidated());
  unsigned writes = 0;
  CHECK(Apply(value.changes, value.notInvalidated, [&](const std::string&, const Bytes&) { ++writes; return true; },
   [](const std::string&) { return true; }, [] { return true; }) == ApplyOutcome::RejectedBeforeWrite);
  CHECK(writes == 0);
 }
 fs::remove_all(folder);
}
#endif
int main() {
#ifdef _WIN32
 TestProductionFolderFreshness();
#endif
 TestBlockedPreparation(false); TestBlockedPreparation(true);
 TestFreshnessRefusesBeforeWriting(); TestRecoveryPublicationIsOffThreadAndBounded(); TestFailedPreparationCompletes();
 std::printf("replay_import_worker_test: all tests passed\n");
}
