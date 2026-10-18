// Same real-GGPO driver as the native diagnostic, but with a small fake engine.
// Passing this test does not claim that native USFIV reset is rollback safe.
#include "../common/ContinuousGgpoProbe.hxx"
#include "test_support.hxx"
#include <array>
#include <set>

namespace probe = sf4e::continuousprobe;
struct FakeEngine : probe::Engine {
    struct State { int frame = 0, epoch = 0, epochFrame = 0; std::uint64_t digest = 17; } state;
    struct Token { State state; };
    std::set<Token*> owned;
    std::array<State, probe::FrameLimit + 1> expected{};
    std::array<bool, probe::FrameLimit + 1> seen{};
    probe::Driver* driver = nullptr;
    bool failLoad = false, throwLoad = false, failSave = false, failAdvance = false, invalid = false, testReentrantClose = false;
    unsigned saves = 0, loads = 0, frees = 0, advances = 0, replay = 0, resetCalls = 0, deniedCloses = 0;
    bool failed = false;
    static bool Equal(const State& a, const State& b) {
        return a.frame == b.frame && a.epoch == b.epoch && a.epochFrame == b.epochFrame && a.digest == b.digest;
    }
    bool Save(void*& token) override {
        CHECK(!failed && !invalid);
        Token* value = new Token{state};
        CHECK(owned.insert(value).second); token = value; ++saves;
        if (testReentrantClose && !deniedCloses) { CHECK(driver && !driver->Close()); ++deniedCloses; }
        if (failSave && saves == 2) { failed = true; return false; }
        return true;
    }
    bool Load(void* token) override {
        CHECK(!failed && !invalid);
        auto* value = static_cast<Token*>(token); CHECK(owned.count(value) == 1); ++loads;
        if (throwLoad) { failed = true; throw 1; }
        if (failLoad) { failed = true; return false; }
        state = value->state;
        return true;
    }
    void Free(void* token) override {
        auto* value = static_cast<Token*>(token); CHECK(owned.erase(value) == 1); ++frees;
        delete value;
    }
    bool Advance(int frame, bool resimulation) override {
        CHECK(!failed && !invalid); CHECK(frame == state.frame); ++advances;
        if (resimulation) ++replay;
        if (failAdvance && frame == 19) {
            // Models a native close reentering the harness while the original
            // update is on the stack. Only its deferred cleanup is permitted.
            invalid = failed = true;
            driver->Fail("fake-native-close");
            CHECK(!driver->Close()); ++deniedCloses;
            return false;
        }
        if (frame == probe::RestartFrame) { ++state.epoch; state.epochFrame = 0; state.digest = 17; ++resetCalls; }
        state.digest = (state.digest ^ static_cast<unsigned>(frame + 31 * state.epochFrame)) * 1099511628211ULL;
        ++state.epochFrame; ++state.frame;
        if (seen[state.frame]) CHECK(Equal(state, expected[state.frame]));
        else { CHECK(!resimulation); expected[state.frame] = state; seen[state.frame] = true; }
        return true;
    }
};

void Run(FakeEngine& engine, bool success, const char* reason) {
    probe::Driver driver(engine); engine.driver = &driver;
    CHECK(driver.Start()); CHECK(!driver.Start());
    const auto deadline = GetTickCount64() + probe::TotalTimeoutMs + 1000;
    while (!driver.Finished() && GetTickCount64() < deadline) { driver.Tick(); Sleep(1); }
    CHECK(driver.Finished()); CHECK(!driver.Active()); CHECK(driver.Succeeded() == success);
    CHECK(driver.Close()); CHECK(driver.Close());
    const auto& stats = driver.GetStats();
    CHECK(stats.opens == 2 && stats.closes == 2);
    CHECK(engine.owned.empty() && engine.saves == engine.frees && stats.saves == stats.frees);
    if (success) {
        CHECK(driver.Failure().empty());
        CHECK(stats.frames[0] == probe::FrameLimit && stats.frames[1] == probe::FrameLimit);
        CHECK(stats.confirmed[0] >= probe::FrameLimit - 1 && stats.confirmed[1] >= probe::FrameLimit - 1);
        CHECK(stats.runningEvents[0] == 1 && stats.runningEvents[1] == 1);
        CHECK(stats.loads > 20 && stats.replayFrames > 100 && stats.resetCrossingLoads > 0 && stats.maxDepth >= 4);
        CHECK(engine.state.frame == probe::FrameLimit && engine.state.epoch == 1 && engine.resetCalls > 1);
        CHECK(engine.replay == stats.replayFrames && engine.loads == stats.loads);
        CHECK(engine.deniedCloses == 1 && stats.deferredCloses == 1);
    } else CHECK(driver.Failure() == reason);
    std::printf("continuous-driver success=%d failure=%s frames=%d/%d loads=%u replay=%u reset_crossings=%u depth=%u tokens=%u/%u\n",
        success ? 1 : 0, driver.Failure().c_str(), stats.frames[0], stats.frames[1], stats.loads,
        stats.replayFrames, stats.resetCrossingLoads, stats.maxDepth, engine.saves, engine.frees);
}

int main() {
    { FakeEngine engine; engine.testReentrantClose = true; Run(engine, true, ""); }
    { FakeEngine engine; engine.failLoad = true; Run(engine, false, "native-load-failed"); }
    { FakeEngine engine; engine.throwLoad = true; Run(engine, false, "native-load-exception"); }
    { FakeEngine engine; engine.failSave = true; Run(engine, false, "native-save-failed"); }
    { FakeEngine engine; engine.failAdvance = true; Run(engine, false, "fake-native-close"); }
    {
        FakeEngine engine, rejectedEngine;
        probe::Driver active(engine), rejected(rejectedEngine);
        CHECK(active.Start()); CHECK(!rejected.Start()); CHECK(rejected.Finished());
        CHECK(rejected.Failure() == "another-private-driver-active"); CHECK(!rejected.Active());
        CHECK(active.Close()); CHECK(active.Failure() == "private-probe-closed-before-completion");
        CHECK(active.GetStats().opens == active.GetStats().closes);
        CHECK(engine.owned.empty() && rejectedEngine.owned.empty());
    }
    std::puts("Continuous private GGPO driver tests passed (synthetic engine only)");
}
