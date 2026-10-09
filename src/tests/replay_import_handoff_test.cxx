#include "replay_slots_support.hxx"
#include "../platform/ReplayImportWorker.hxx"
#include <chrono>
#include <fstream>
#include <future>
#include <thread>

using namespace sf4e::replayfiles;
using namespace sf4e::platform::replays;
namespace fs = std::filesystem;
using Worker = ImportWorker;

static void Save(const fs::path& path, const Bytes& bytes) {
 std::ofstream out(path, std::ios::binary | std::ios::trunc);
 out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
 out.close(); CHECK(!out.fail());
}

struct Handoff {
 fs::path root = fs::temp_directory_path() / ("ember-import-handoff-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 fs::path saves = root / "saves", archive = root / "archive", backup = archive / "held.usf4replay";
 const std::thread::id gameThread = std::this_thread::get_id();
 const Bytes body = Replay(2, 800);
 std::vector<Change> originals;
 unsigned writes = 0, removes = 0, publications = 0;
 Handoff() {
  CHECK(fs::create_directories(saves) && fs::create_directory(archive));
  Bytes list = EmptyList(), swan = EmptySwan();
  Fill(list, swan, 280, body, 0x44);
  originals = {{"280", body}, {"280.0", Sidecar(body)}, {"replays-swan.dat", swan},
   {"replays-swan.dat.0", Sidecar(swan)}, {"LIST", list}, {"LIST.0", Sidecar(list)}};
  for (auto& file : originals) {
   file.path = saves / file.name; file.existed = true;
   Save(file.path, file.before);
   file.name = "capcom/superstreetfighteriv/ssf4_savedata/" + file.name;
  }
  Save(backup, body);
 }
 ~Handoff() { std::error_code error; fs::remove_all(root, error); }
 ImportTransaction Prepare() {
  std::promise<void> prepared;
  Worker worker([&](const fs::path&) {
   CHECK(std::this_thread::get_id() != gameThread);
   PreparedImport value;
   value.result = ImportResult::Done;
   for (const auto& file : originals) {
    Change change; change.name = file.name; change.path = file.path; change.after = {9, 8, 7};
    CHECK(Snapshot(change.path, 1 << 20, change));
    value.changes.push_back(std::move(change));
   }
   CHECK(VerifyBackup({backup}, archive, body, value.backup));
   // Deterministically model an unsignaled notification through the handoff.
   // The commit must reject stale real files even when this returns true.
   value.notInvalidated = [] { return true; };
   auto out = std::make_shared<const PreparedImport>(std::move(value));
   prepared.set_value(); return out;
  }, [](const ImportTransaction&) {});
  CHECK(worker.Request(backup));
  CHECK(prepared.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  worker.Stop();
  ImportTransaction value;
  CHECK(worker.Take(value) && value && !worker.Take(value));
  return value;
 }
 ApplyOutcome Commit(const ImportTransaction& value, bool accept = true) {
  return Apply(value->changes, [&] { CHECK(std::this_thread::get_id() == gameThread); return value->Fresh(); },
   [&](const std::string& name, const Bytes& bytes) {
    CHECK(std::this_thread::get_id() == gameThread); ++writes;
    for (const auto& file : originals) if (file.name == name) { Save(file.path, bytes); return true; }
    return false;
   }, [&](const std::string&) { ++removes; return false; }, [&] { ++publications; return accept; });
 }
 void RejectWithoutChangingFiles(const ImportTransaction& value) {
  std::vector<Change> current;
  for (const auto& file : originals) {
   Change read;
   CHECK(Snapshot(file.path, 1 << 20, read)); current.push_back(std::move(read));
  }
  CHECK(value->notInvalidated());
  CHECK(Commit(value, false) == ApplyOutcome::RejectedBeforeWrite);
  CHECK(writes == 0 && removes == 0 && publications == 0);
  for (std::size_t i = 0; i < originals.size(); ++i) {
   Change after;
   CHECK(Snapshot(originals[i].path, 1 << 20, after));
   CHECK(after.existed == current[i].existed && after.before == current[i].before);
  }
 }
};

static void TestExistingFileChangedWithNotificationPending() {
 Handoff h;
 // Every overwrite target participates, including both indexes and all .0s.
 for (const auto& file : h.originals) {
  auto value = h.Prepare();
  const auto timestamp = fs::last_write_time(file.path);
  Bytes newer = file.before; newer.back() ^= 1;
  Save(file.path, newer); fs::last_write_time(file.path, timestamp);
  CHECK(fs::file_size(file.path) == file.before.size());
  h.RejectWithoutChangingFiles(value);
  CHECK(ReadFile(file.path, 1 << 20) == std::optional<Bytes>(newer));
  Save(file.path, file.before);
 }
 // No stale bytes: the same game-owner gate permits one complete execution.
 CHECK(h.Commit(h.Prepare()) == ApplyOutcome::Done);
 CHECK(h.writes == h.originals.size() && h.removes == 0 && h.publications == 1);
}

static void TestBackupDeletedAfterHandoff() {
 Handoff h;
 const auto value = h.Prepare();
 CHECK(fs::remove(h.backup));
 CHECK(value->backup.replay.SameBody(h.body)); // memory evidence survives
 h.RejectWithoutChangingFiles(value);
 CHECK(!fs::exists(h.backup));
}

static void TestBackupReplacedAfterHandoff() {
 Handoff h;
 const auto value = h.Prepare();
 const auto timestamp = fs::last_write_time(h.backup);
 CHECK(fs::remove(h.backup));
 const Bytes other = Replay(3, h.body.size());
 Save(h.backup, other); fs::last_write_time(h.backup, timestamp);
 ArchiveFile valid;
 CHECK(ReadArchive(h.backup, true, valid) == ArchiveState::Valid);
 CHECK(fs::file_size(h.backup) == value->backup.replay.contents.size());
 h.RejectWithoutChangingFiles(value);
 CHECK(ReadFile(h.backup) == std::optional<Bytes>(other));
}

static void TestMissingEmptyAndUnreadableTargets() {
 Handoff h;
 const auto file = h.originals.front();
 PreparedImport value; value.notInvalidated = [] { return true; };
 Change change; change.path = file.path; change.name = file.name;
 CHECK(fs::remove(file.path));
 CHECK(Snapshot(file.path, 100, change) && !change.existed);
 value.changes = {change}; CHECK(value.Fresh());
 Save(file.path, {}); CHECK(!value.Fresh()); // missing became existing empty
 CHECK(Snapshot(file.path, 100, change) && change.existed && change.before.empty());
 value.changes = {change}; CHECK(value.Fresh());
 CHECK(fs::remove(file.path)); CHECK(!value.Fresh()); // empty became missing
 CHECK(fs::create_directory(file.path)); CHECK(!value.Fresh()); // unreadable as a file
}

int main() {
 TestExistingFileChangedWithNotificationPending();
 TestBackupDeletedAfterHandoff(); TestBackupReplacedAfterHandoff(); TestMissingEmptyAndUnreadableTargets();
 std::printf("replay_import_handoff_test: all tests passed\n");
}
