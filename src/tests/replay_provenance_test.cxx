#include "replay_slots_support.hxx"
#include "../common/ReplayRecordingWorker.hxx"
#include <future>
#include <chrono>

using namespace sf4e::replayfiles;
static Bytes Saved(unsigned time) {
 Bytes body = Replay(2, 800);
 const auto native = 116444736000000000ull + std::uint64_t{time} * 10000000ull;
 WriteU32(body.data() + 0x10, static_cast<std::uint32_t>(native));
 WriteU32(body.data() + 0x14, static_cast<std::uint32_t>(native >> 32));
 return body;
}
static RecordingBoundary Start(unsigned time, const char* name) {
 RecordingBoundary event;
 event.kind = RecordingBoundary::Kind::Start; event.recordingEnabled = true; event.time = time;
 event.names = {{name, "Opponent"}, false}; event.fighters[0] = 2; event.fighters[1] = 3;
 return event;
}
static RecordingSnapshot Slots(const Bytes& body) { return {"account", "archive", {{body, Sidecar(body)}}}; }
static void TestDelayedSaveAndRapidRematch() {
 RecordingAttribution owner;
 const auto before = std::make_shared<const RecordingSnapshot>(Slots(Saved(1)));
 owner.Begin(Start(10, "First"), before); owner.End(20);
 std::vector<std::string> names;
 const auto publish = [&](const std::filesystem::path&, const Bytes&, const ReplayNames& note) { names.push_back(note.players[0]); return NamePublication::Published; };
 owner.ObserveSave(*before); owner.PublishIdentified(publish);
 CHECK(owner.Pending() && names.empty()); // save has not arrived
 owner.Begin(Start(21, "Second"), before); // keep the first recording's owner
 auto first = Slots(Saved(20));
 owner.ObserveSave(first); owner.PublishIdentified(publish);
 CHECK(!owner.Pending() && names == std::vector<std::string>{"First"});
 owner.End(30);
 owner.ObserveSave(Slots(Saved(30))); owner.PublishIdentified(publish);
 CHECK(!owner.Pending() && names.size() == 2 && names[1] == "Second");
 // A retryable publisher failure retains the already identified exact body even when
 // another recording overwrites its slot before publication can be retried.
 owner.Begin(Start(40, "Third"), std::make_shared<const RecordingSnapshot>(first)); owner.End(50);
 owner.ObserveSave(Slots(Saved(50))); owner.PublishIdentified([](const std::filesystem::path&, const Bytes&, const ReplayNames&) { return NamePublication::Retryable; });
 owner.Begin(Start(51, "Fourth"), std::make_shared<const RecordingSnapshot>(Slots(Saved(50))));
 owner.ObserveSave(Slots(Saved(60))); owner.PublishIdentified([&](const std::filesystem::path&, const Bytes& body, const ReplayNames& note) {
  CHECK(body == Saved(50) && note.players[0] == "Third"); return NamePublication::Published;
 });
 owner.End(60); owner.ObserveSave(Slots(Saved(60))); owner.PublishIdentified(publish);
 CHECK(names.back() == "Fourth");
 // Coarse native timestamps cannot distinguish an unobserved save in the
 // same second as the next match starts. Never bind that body to the old one.
 owner.Begin(Start(70, "Old"), before); owner.End(80);
 owner.Begin(Start(80, "New"), before);
 const auto count = names.size(); owner.ObserveSave(Slots(Saved(80))); owner.PublishIdentified(publish);
 CHECK(names.size() == count);
 owner.End(85); owner.ObserveSave(Slots(Saved(80))); owner.PublishIdentified(publish);
 CHECK(names.size() == count); // do not give that old save the new names either
 owner.Reset(); owner.Begin(Start(90, "Missed"), {}); owner.End(100);
 owner.ObserveSave(Slots(Saved(100))); owner.PublishIdentified(publish); CHECK(names.size() == count);
 owner.Begin(Start(110, "Account"), before); owner.End(120);
 auto other = Slots(Saved(120)); other.saves = "different-account";
 owner.ObserveSave(other); owner.PublishIdentified(publish); CHECK(!owner.Pending());
 // Retained baselines are bounded even if saves never arrive. An evicted
 // recording cannot claim an arbitrarily delayed body after many rematches.
 for (unsigned match = 0; match < RecordingAttribution::kPendingLimit + 1; ++match) {
  owner.Begin(Start(200 + match * 20, "Bounded"), before); owner.End(210 + match * 20);
 }
 const auto boundedCount = names.size(); owner.ObserveSave(Slots(Saved(210))); owner.PublishIdentified(publish);
 CHECK(names.size() == boundedCount); owner.Reset();
}
static void TestBlockedPublisherDoesNotBlockHooks() {
 std::mutex diskMutex;
 auto disk = Slots(Saved(1));
 std::promise<void> baselineRead, publishing, release;
 auto gate = release.get_future();
 RecordingWorker worker([&] {
  std::lock_guard<std::mutex> lock(diskMutex);
  return disk;
 }, [&](const std::filesystem::path&, const Bytes& body, const ReplayNames& names) {
  CHECK(body == Saved(20) && names.players[0] == "First");
  publishing.set_value(); gate.wait(); return NamePublication::Published;
 }, [&](const RecordingBoundary& event) {
  if (event.kind == RecordingBoundary::Kind::Start && event.time == 10) baselineRead.set_value();
 });
 worker.Post(Start(10, "First"));
 CHECK(baselineRead.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 // The observer confirms Begin owns the baseline. A delayed save arrives
 // only after End, on the retry scan.
 RecordingBoundary end; end.time = 20;
 worker.Post(end);
 { std::lock_guard<std::mutex> lock(diskMutex); disk = Slots(Saved(20)); }
 worker.FilesChanged();
 CHECK(publishing.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 auto hooks = std::async(std::launch::async, [&] {
  worker.Post(Start(21, "Second")); end.time = 30; worker.Post(end);
 });
 CHECK(hooks.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 hooks.get(); release.set_value(); worker.Stop();
}
static void TestBoundaryDuringBlockedBaselineIsRefused() {
 std::promise<void> reading, release, ended;
 auto gate = release.get_future();
 std::atomic<unsigned> publications{0};
 RecordingWorker worker([&] { reading.set_value(); gate.wait(); return Slots(Saved(1)); },
  [&](const std::filesystem::path&, const Bytes&, const ReplayNames&) { ++publications; return NamePublication::Published; },
  [&](const RecordingBoundary& event) { if (event.kind == RecordingBoundary::Kind::End) ended.set_value(); });
 worker.Post(Start(10, "Missed"));
 CHECK(reading.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 RecordingBoundary end; end.time = 20; worker.Post(end);
 release.set_value();
 CHECK(ended.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 worker.Stop();
 CHECK(publications == 0);
}
static void TestDisabledAndDormantRecordings() {
 std::atomic<unsigned> reads{0}, writes{0}, logs{0};
 std::mutex observedMutex;
 std::condition_variable observed;
 unsigned lastTime = 0;
 std::mutex readMutex;
 std::condition_variable readObserved;
 RecordingWorker worker([&] {
  { std::lock_guard<std::mutex> lock(readMutex); ++reads; readObserved.notify_all(); }
  return Slots(Saved(1));
 },
  [&](const std::filesystem::path&, const Bytes&, const ReplayNames&) { ++writes; return NamePublication::Published; },
  [&](const RecordingBoundary& event) { std::lock_guard<std::mutex> lock(observedMutex); lastTime = static_cast<unsigned>(event.time); observed.notify_all(); },
  [&](const char*) { ++logs; });
 const auto wait = [&](unsigned time) { std::unique_lock<std::mutex> lock(observedMutex); CHECK(observed.wait_for(lock, std::chrono::seconds(2), [&] { return lastTime == time; })); };
 auto disabled = Start(10, "Spectator"); disabled.recordingEnabled = false; disabled.names.spectated = true;
 worker.Post(disabled); wait(10);
 RecordingBoundary end; end.time = 20; worker.Post(end); wait(20);
 CHECK(reads == 0 && writes == 0);
 worker.Post(Start(30, "No save")); wait(30);
 end.time = 40; worker.Post(end); wait(40);
 {
  std::unique_lock<std::mutex> lock(readMutex);
  CHECK(readObserved.wait_for(lock, std::chrono::seconds(2), [&] { return reads >= 2; }));
  CHECK(!readObserved.wait_for(lock, std::chrono::milliseconds(800), [&] { return reads > 2; }));
 }
 worker.Stop();
 CHECK(reads == 2 && writes == 0 && logs == 1);
}
static void TestPermanentRefusalAndBoundedRetries() {
 unsigned attempts = 0, logs = 0;
 RecordingAttribution owner([&](const char*) { ++logs; });
 const auto before = std::make_shared<const RecordingSnapshot>(Slots(Saved(1)));
 owner.Begin(Start(10, "Refused"), before); owner.End(20);
 owner.ObserveSave(Slots(Saved(20)));
 CHECK(owner.Pending() && !owner.NeedsSnapshot());
 const auto refuse = [&](const std::filesystem::path&, const Bytes&, const ReplayNames&) { ++attempts; return NamePublication::Refused; };
 owner.PublishIdentified(refuse);
 for (int i = 0; i < 10; ++i) owner.PublishIdentified(refuse);
 CHECK(!owner.Pending() && attempts == 1 && logs == 1);
 owner.Begin(Start(30, "Retryable"), before); owner.End(40); owner.ObserveSave(Slots(Saved(40)));
 const auto retry = [&](const std::filesystem::path&, const Bytes& body, const ReplayNames&) { CHECK(body == Saved(40)); ++attempts; return NamePublication::Retryable; };
 for (int i = 0; i < 10; ++i) owner.PublishIdentified(retry);
 CHECK(!owner.Pending() && attempts == 4 && logs == 2);
 // A save that never arrives remains dormant, then is explicitly given up.
 owner.Begin(Start(50, "Missing"), before); owner.End(60);
 owner.ObserveSave(*before); CHECK(owner.NeedsSnapshot());
 owner.GiveUp(); CHECK(!owner.Pending() && logs == 3);
}
static void TestIdentifiedBodyRetriesWithoutScanning() {
 std::mutex diskMutex;
 auto disk = Slots(Saved(1));
 std::atomic<unsigned> reads{0}, attempts{0};
 std::promise<void> baseline, identified, refused;
 RecordingWorker worker([&] { ++reads; std::lock_guard<std::mutex> lock(diskMutex); return disk; },
  [&](const std::filesystem::path&, const Bytes& body, const ReplayNames&) {
   CHECK(body == Saved(20));
   if (++attempts == 1) { identified.set_value(); return NamePublication::Retryable; }
   refused.set_value(); return NamePublication::Refused;
  }, [&](const RecordingBoundary& event) { if (event.kind == RecordingBoundary::Kind::Start) baseline.set_value(); });
 worker.Post(Start(10, "Retry"));
 CHECK(baseline.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 { std::lock_guard<std::mutex> lock(diskMutex); disk = Slots(Saved(20)); }
 RecordingBoundary end; end.time = 20; worker.Post(end);
 CHECK(identified.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 worker.FilesChanged();
 CHECK(refused.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 worker.Stop(); CHECK(reads == 2 && attempts == 2);
}
static void TestInvalidExistingNamesLifecycle() {
 namespace fs = std::filesystem;
 const auto folder = fs::temp_directory_path() / ("ember-provenance-refused-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 const auto path = NamesPath(folder, Saved(20));
 CHECK(fs::create_directories(path.parent_path()));
 { std::ofstream file(path, std::ios::binary); file << "damaged original"; CHECK(file.good()); }
 std::promise<void> baseline, refused;
 std::atomic<unsigned> reads{0}, attempts{0}, writes{0}, logs{0};
 std::mutex diskMutex;
 auto disk = Slots(Saved(1)); disk.archive = folder;
 RecordingWorker worker([&] { ++reads; std::lock_guard<std::mutex> lock(diskMutex); return disk; },
  [&](const fs::path& archive, const Bytes& body, const ReplayNames& names) {
   ++attempts;
   const auto result = PublishBodyNames(NamesPath(archive, body), body, names, [&](const Bytes&) { ++writes; return false; });
   CHECK(result == NamePublication::Refused); refused.set_value(); return result;
  }, [&](const RecordingBoundary& event) { if (event.kind == RecordingBoundary::Kind::Start && event.time == 10) baseline.set_value(); },
  [&](const char*) { ++logs; });
 worker.Post(Start(10, "Refused"));
 CHECK(baseline.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 { std::lock_guard<std::mutex> lock(diskMutex); disk = Slots(Saved(20)); disk.archive = folder; }
 RecordingBoundary end; end.time = 20; worker.Post(end);
 CHECK(refused.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 for (int i = 0; i < 10; ++i) worker.FilesChanged();
 worker.Stop();
 CHECK(reads == 2 && attempts == 1 && writes == 0 && logs == 1);
 const auto original = ReadFile(path); CHECK(original && std::string(original->begin(), original->end()) == "damaged original");
 fs::remove_all(folder);
}
static void TestWorkerFencesSameSecondRematchBaseline() {
 std::mutex diskMutex, observedMutex;
 std::condition_variable observed;
 auto disk = Slots(Saved(1));
 unsigned lastTime = 0;
 std::atomic<unsigned> publications{0}, reads{0};
 std::promise<void> endScan;
 RecordingWorker worker([&] {
  RecordingSnapshot snapshot;
  { std::lock_guard<std::mutex> lock(diskMutex); snapshot = disk; }
  if (++reads == 2) endScan.set_value();
  return snapshot;
 },
  [&](const std::filesystem::path&, const Bytes&, const ReplayNames&) { ++publications; return NamePublication::Published; },
  [&](const RecordingBoundary& event) { std::lock_guard<std::mutex> lock(observedMutex); lastTime = static_cast<unsigned>(event.time); observed.notify_all(); });
 const auto wait = [&](unsigned time) { std::unique_lock<std::mutex> lock(observedMutex); CHECK(observed.wait_for(lock, std::chrono::seconds(2), [&] { return lastTime == time; })); };
 worker.Post(Start(70, "Old")); wait(70);
 RecordingBoundary end; end.time = 80; worker.Post(end); wait(80);
 CHECK(endScan.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 { std::lock_guard<std::mutex> lock(diskMutex); disk = Slots(Saved(80)); }
 { std::unique_lock<std::mutex> lock(observedMutex); lastTime = 0; }
 worker.Post(Start(80, "New"));
 // Use a distinct marker for the second start: waiting on time=80 alone
 // would also accept the preceding End observer.
 end.time = 85;
 // The start must own its baseline before End is posted. Signal that using
 // the observation state and a condition-variable wait.
 wait(80); worker.Post(end); wait(85);
 worker.Stop(); CHECK(publications == 0);
}
int main() {
 TestWorkerFencesSameSecondRematchBaseline();
 TestIdentifiedBodyRetriesWithoutScanning(); TestInvalidExistingNamesLifecycle();
 TestDisabledAndDormantRecordings(); TestPermanentRefusalAndBoundedRetries();
 TestDelayedSaveAndRapidRematch(); TestBlockedPublisherDoesNotBlockHooks(); TestBoundaryDuringBlockedBaselineIsRefused();
 std::printf("replay_provenance_test: all tests passed\n");
}
