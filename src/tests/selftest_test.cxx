// The in-game self-test's own rules: which names start a test, and that a
// move's cells are counted as the bars draw them, one side at a time.
#include "../sf4e/sf4e__SelfTest.hxx"
#include <cstdio>
#include <stdexcept>
using namespace sf4e;
using training::MeterKind;
namespace {
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
void Add(training::MeterView& meter, MeterKind own, MeterKind other, int count) {
    for (int i = 0; i < count; ++i) {
        training::MeterFrame frame;
        frame.cells[0].kind = own; frame.cells[1].kind = other;
        meter.frames.push_back(frame);
    }
}
}
int main() {
    try {
        Require(selftest::Parse(L"training") == selftest::Test::Training, "training names the training test");
        Require(selftest::Parse(L"") == selftest::Test::None, "no name, no test");
        Require(selftest::Parse(L"Training") == selftest::Test::None && selftest::Parse(L"trainingx") == selftest::Test::None,
            "only the exact name starts a test");
        // Ryu's crouching LP as the bars draw it, Player 2 standing and then blocking.
        training::MeterView meter;
        Add(meter, MeterKind::Startup, MeterKind::Neutral, 3);
        Add(meter, MeterKind::Active, MeterKind::Neutral, 2);
        Add(meter, MeterKind::Recovery, MeterKind::Guard, 6);
        Add(meter, MeterKind::Neutral, MeterKind::Neutral, 4);
        const auto own = selftest::CountCells(meter, 0), other = selftest::CountCells(meter, 1);
        Require(own.startup == 3 && own.active == 2 && own.recovery == 6, "Player 1's cells are counted by the kind drawn");
        Require(other.startup == 0 && other.active == 0 && other.recovery == 0, "the other side's cells are its own");
    } catch (const std::exception& error) {
        std::printf("SelfTest: %s\n", error.what());
        return 1;
    }
    std::printf("SelfTest: pass\n");
    return 0;
}
