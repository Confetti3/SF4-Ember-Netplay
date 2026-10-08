#include "replay_slots_support.hxx"
#include "../common/ReplayRecordingWorker.hxx"
#include <future>

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
 event.kind = RecordingBoundary::Kind::Start; event.time = time;
 event.names = {{name, "Opponent"}, false}; event.fighters[0] = 2; event.fighters[1] = 3;
 return event;
}
static RecordingSnapshot Slots(const Bytes& body) { return {"account", "archive", {{body, Sidecar(body)}}}; }
static void TestDelayedSaveAndRapidRematch() {
 RecordingAttribution owner;
 const auto before = std::make_shared<const RecordingSnapshot>(Slots(Saved(1)));
 owner.Begin(Start(10, "First"), before); owner.End(20);
 std::vector<std::string> names;
 const auto publish = [&](const std::filesystem::path&, const Bytes&, const ReplayNames& note) { names.push_back(note.players[0]); return true; };
 owner.Retry(*before, publish);
 CHECK(owner.Pending() && names.empty()); // save has not arrived
 owner.Begin(Start(21, "Second"), before); // keep the first recording's owner
 auto first = Slots(Saved(20));
 owner.Retry(first, publish);
 CHECK(!owner.Pending() && names == std::vector<std::string>{"First"});
 owner.End(30);
 owner.Retry(Slots(Saved(30)), publish);
 CHECK(!owner.Pending() && names.size() == 2 && names[1] == "Second");
 // A publisher refusal retains the already identified exact body even when
 // another recording overwrites its slot before publication can be retried.
 owner.Begin(Start(40, "Third"), std::make_shared<const RecordingSnapshot>(first)); owner.End(50);
 owner.Retry(Slots(Saved(50)), [](const std::filesystem::path&, const Bytes&, const ReplayNames&) { return false; });
 owner.Begin(Start(51, "Fourth"), std::make_shared<const RecordingSnapshot>(Slots(Saved(50))));
 owner.Retry(Slots(Saved(60)), [&](const std::filesystem::path&, const Bytes& body, const ReplayNames& note) {
  CHECK(body == Saved(50) && note.players[0] == "Third"); return true;
 });
 owner.End(60); owner.Retry(Slots(Saved(60)), publish);
 CHECK(names.back() == "Fourth");
 // Coarse native timestamps cannot distinguish an unobserved save in the
 // same second as the next match starts. Never bind that body to the old one.
 owner.Begin(Start(70, "Old"), before); owner.End(80);
 owner.Begin(Start(80, "New"), before);
 const auto count = names.size(); owner.Retry(Slots(Saved(80)), publish);
 CHECK(names.size() == count);
 owner.End(85); owner.Retry(Slots(Saved(80)), publish);
 CHECK(names.size() == count); // do not give that old save the new names either
 owner.Reset(); owner.Begin(Start(90, "Missed"), {}); owner.End(100);
 owner.Retry(Slots(Saved(100)), publish); CHECK(names.size() == count);
 owner.Begin(Start(110, "Account"), before); owner.End(120);
 auto other = Slots(Saved(120)); other.saves = "different-account";
 owner.Retry(other, publish); CHECK(!owner.Pending());
 // Retained baselines are bounded even if saves never arrive. An evicted
 // recording cannot claim an arbitrarily delayed body after many rematches.
 for (unsigned match = 0; match < RecordingAttribution::kPendingLimit + 1; ++match) {
  owner.Begin(Start(200 + match * 20, "Bounded"), before); owner.End(210 + match * 20);
 }
 const auto boundedCount = names.size(); owner.Retry(Slots(Saved(210)), publish);
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
  publishing.set_value(); gate.wait(); return true;
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
  [&](const std::filesystem::path&, const Bytes&, const ReplayNames&) { ++publications; return true; },
  [&](const RecordingBoundary& event) { if (event.kind == RecordingBoundary::Kind::End) ended.set_value(); });
 worker.Post(Start(10, "Missed"));
 CHECK(reading.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 RecordingBoundary end; end.time = 20; worker.Post(end);
 release.set_value();
 CHECK(ended.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
 worker.Stop();
 CHECK(publications == 0);
}
int main() {
 TestDelayedSaveAndRapidRematch(); TestBlockedPublisherDoesNotBlockHooks(); TestBoundaryDuringBlockedBaselineIsRefused();
 std::printf("replay_provenance_test: all tests passed\n");
}
