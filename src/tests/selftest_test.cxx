// The in-game self-test's own rule: a move's cells are counted as the bars
// draw them, one side at a time.
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
        // Ryu's crouching LP as the bars draw it, Player 2 standing and then blocking.
        training::MeterView meter;
        Add(meter, MeterKind::Startup, MeterKind::Neutral, 3);
        Add(meter, MeterKind::Active, MeterKind::Neutral, 2);
        Add(meter, MeterKind::Recovery, MeterKind::Guard, 6);
        Add(meter, MeterKind::Neutral, MeterKind::Neutral, 4);
        const auto own = selftest::CountCells(meter, 0), other = selftest::CountCells(meter, 1);
        Require(own.startup == 3 && own.active == 2 && own.recovery == 6, "Player 1's cells are counted by the kind drawn");
        Require(other.startup == 0 && other.active == 0 && other.recovery == 0, "the other side's cells are its own");

        // The tournament host's fighter: forward always, from either side,
        // heavy punch for two frames of every twenty, and nothing else.
        int presses = 0;
        for (unsigned frame = 0; frame < 10 * selftest::kAttackPeriod; ++frame) {
            const unsigned right = selftest::AttackInput(frame, true), left = selftest::AttackInput(frame, false);
            Require((right & training::RightBit) && !(right & training::LeftBit), "Player 1 walks right");
            Require((left & training::LeftBit) && !(left & training::RightBit), "Player 2 walks left");
            Require((right & ~(training::RightBit | combo::HP)) == 0 && (left & ~(training::LeftBit | combo::HP)) == 0,
                "only forward and heavy punch are held");
            Require((right & combo::HP) == (left & combo::HP), "both sides press on the same frames");
            const bool held = frame > 0 && (selftest::AttackInput(frame - 1, true) & combo::HP);
            presses += (right & combo::HP) && !held ? 1 : 0;
        }
        Require(presses == 10, "heavy punch is pressed once a period");
        Require((selftest::AttackInput(0, true) & combo::HP) && (selftest::AttackInput(1, true) & combo::HP) &&
            !(selftest::AttackInput(2, true) & combo::HP), "each press is held for two frames");
    } catch (const std::exception& error) {
        std::printf("SelfTest: %s\n", error.what());
        return 1;
    }
    std::printf("SelfTest: pass\n");
    return 0;
}
