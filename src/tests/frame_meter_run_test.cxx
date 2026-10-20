// The frame meter's bars as SF6's meter has them: they start on the first
// active frame of an exchange, fill left to right and go on from the left
// edge past their width, hold four quiet frames after it on its last active
// frame, and any action after that starts the next exchange.
#include "../training/FrameMeter.hxx"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
using namespace sf4e::training;
namespace {
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
// One frame of the fight: each fighter's status (0 standing, 2 jumping, 10
// walking, 14 guarding, 16 attacking, 21 hit, 22 blocking).
struct Step { unsigned own, other; };
// Plays steps on a meter, its frame counter going on from where it was.
struct Fight {
    FrameMeter meter;
    int at = 0;
    void Play(Step step, int count = 1) {
        for (int i = 0; i < count; ++i, ++at) {
            std::array<FighterSample, 2> both;
            for (int side = 0; side < 2; ++side) {
                auto& fighter = both[side];
                fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false;
                fighter.status = side ? step.other : step.own;
                fighter.action = fighter.status ? 100 + side : 0;
                fighter.actionFrame = static_cast<float>(at);
            }
            meter.Observe(at, both);
        }
    }
    const MeterView& View() const { return meter.View(); }
    // The frames index a cell of the bars shows, window cells across, back pages back.
    int Cell(std::size_t cell, std::size_t window = 60, std::size_t back = 0) const {
        return MeterFrameIndex(View().frames.size(), View().length, cell, window, back);
    }
};
const Step Quiet{0, 0}, Attack{16, 0}, Struck{0, 21};
const unsigned Quiets = MeterIdleFrames;
}
int main() {
    try {
        // Pages: the frame at position pos of an exchange is drawn in cell
        // pos % window of page pos / window; the bars show the newest frame's.
        Require(MeterFrameIndex(0, 0, 0) == -1 && MeterFrameIndex(3, 3, 0, 60) == 0 && MeterFrameIndex(3, 3, 2, 60) == 2 &&
            MeterFrameIndex(3, 3, 3, 60) == -1 && MeterFrameIndex(3, 3, 60, 60) == -1, "A short exchange was not drawn from the left edge");
        Require(MeterFrameIndex(61, 61, 0, 60) == 60 && MeterFrameIndex(61, 61, 1, 60) == -1 && MeterFrameIndex(61, 61, 0, 60, 1) == 0 &&
            MeterFrameIndex(61, 61, 59, 60, 1) == 59 && MeterFrameIndex(61, 61, 0, 60, 2) == -1, "An exchange past the bars did not go on from their left edge");
        // Kept frames are the exchange's newest: the oldest page is only partly kept.
        Require(MeterFrameIndex(240, 300, 59, 60) == 239 && MeterFrameIndex(240, 300, 0, 60, 4) == -1 && MeterFrameIndex(240, 300, 59, 60, 4) == -1 &&
            MeterFrameIndex(240, 300, 0, 60, 3) == 0 && MeterBackLimit(240, 300, 60) == 3, "The kept frames were not paged by their positions");
        Require(MeterBackLimit(240, 250, 120) == 2 && MeterFrameIndex(240, 250, 9, 120, 2) == -1 && MeterFrameIndex(240, 250, 10, 120, 2) == 0 &&
            MeterBackLimit(0, 0, 60) == 0 && MeterBackLimit(30, 30, 60) == 0, "A held meter could step back past its frames or not to them");
        // Sliding: the newest frame at the right edge once the bars are full,
        // from the left until then, and back steps frames toward the oldest.
        Require(MeterSlideIndex(3, 0, 60) == 0 && MeterSlideIndex(3, 2, 60) == 2 && MeterSlideIndex(3, 3, 60) == -1, "A short exchange did not slide in from the left");
        Require(MeterSlideIndex(100, 59, 60) == 99 && MeterSlideIndex(100, 0, 60) == 40 && MeterSlideIndex(100, 59, 60, 10) == 89 &&
            MeterSlideLimit(100, 60) == 40 && MeterSlideIndex(100, 0, 60, 40) == 0 && MeterSlideLimit(30, 60) == 0, "The bars did not slide a frame at a time");
        // Bars made by hand give no length: their frames are the whole exchange.
        Require(MeterFrameIndex(130, 0, 9, 60) == 129 && MeterBackLimit(130, 0, 60) == 2, "Bars made by hand were not paged from their first frame");

        // Start promptly: idle frames are not kept, and the first active one starts the bars.
        {
            Fight start; start.Play(Quiet, 40);
            Require(start.View().frames.empty() && !start.View().frozen, "Idle frames before an exchange were kept");
            start.Play(Attack);
            Require(start.View().frames.size() == 1 && start.View().length == 1 && start.View().frames.front().frame == 40 && start.Cell(0) == 0,
                "The first active frame did not start the bars at their left edge");
        }
        // Fill left to right: past the width the bars go on from their left edge.
        {
            Fight fill; fill.Play(Attack, 10);
            Require(fill.Cell(9) == 9 && fill.Cell(10) == -1, "The bars did not fill from the left");
            fill.Play(Attack, 120);
            Require(fill.View().length == 130 && fill.Cell(0) == 120 && fill.Cell(9) == 129 && fill.Cell(10) == -1, "The bars did not go on from their left edge");
            for (const std::size_t window : {90u, 120u})
                Require(fill.Cell(129 % window, window) == 129 && fill.Cell(129 % window + 1, window) == -1, "Another width did not page the same frames");
            const int length = static_cast<int>(MeterHistory) + 200;
            fill.Play(Attack, length - 130);
            Require(fill.View().frames.size() == MeterHistory && fill.View().length == static_cast<std::size_t>(length) &&
                fill.Cell((length - 1) % 60) == static_cast<int>(MeterHistory) - 1,
                "The newest frame left its cell once the oldest were dropped");
        }
        // Meaty frames are marked once, when the other is seen up: its last
        // frame down, and the attacker's active frames before the one that met it.
        {
            std::deque<MeterFrame> bars(6);
            const MeterKind kinds[] = {MeterKind::Startup, MeterKind::Active, MeterKind::Active, MeterKind::Active, MeterKind::Active, MeterKind::Active};
            for (std::size_t i = 0; i < bars.size(); ++i) { bars[i].cells[0].kind = kinds[i]; bars[i].cells[1].kind = MeterKind::Down; }
            MarkMeaty(bars, 1);
            Require(bars[5].cells[1].meaty && !bars[4].cells[1].meaty, "The frame the fighter could first be hit on was not marked");
            Require(!bars[0].cells[0].meaty && bars[1].cells[0].meaty && bars[4].cells[0].meaty && !bars[5].cells[0].meaty,
                "The attack's active frames before the one that met the fighter were not the meaty ones");
            bars.back().cells[0].kind = MeterKind::Recovery; for (auto& frame : bars) frame.cells[0].meaty = false;
            MarkMeaty(bars, 1);
            Require(!bars[3].cells[0].meaty, "An attack no longer active as the fighter got up was marked meaty");
        }
        // After a pause the next exchange starts on clear bars, or, continuing,
        // goes on after the last one, the quiet frames between left out.
        for (const bool continuous : {false, true}) {
            Fight pause; pause.meter.SetContinuous(continuous);
            pause.Play(Attack, 10); pause.Play(Quiet, Quiets);
            Require(pause.View().frozen && pause.View().length == 10, "The bars did not hold after an exchange");
            pause.Play(Attack, 5);
            Require(!pause.View().frozen && pause.View().length == (continuous ? 15u : 5u) && pause.View().frames.size() == pause.View().length,
                continuous ? "Continuing started the next exchange over" : "The next exchange did not start on clear bars");
        }
        // A shorter history keeps only the newest frames, at once and from then on.
        {
            Fight kept; kept.Play(Attack, 600);
            kept.meter.SetHistory(240);
            Require(kept.View().frames.size() == 240 && kept.View().length == 600, "A shorter history kept the oldest frames");
            kept.Play(Attack, 10);
            Require(kept.View().frames.size() == 240 && kept.View().length == 610, "The history grew past its length");
        }
        // Stop promptly: four quiet frames after a whiff hold the bars on its last active frame.
        {
            Fight whiff; whiff.Play(Quiet, 3); whiff.Play(Attack, 10); whiff.Play(Quiet, Quiets - 1);
            Require(!whiff.View().frozen && whiff.View().frames.size() == 13, "The bars were held before four quiet frames");
            whiff.Play(Quiet);
            Require(whiff.View().frozen && whiff.View().frames.size() == 10 && whiff.View().length == 10 && whiff.View().frames.back().frame == 12,
                "A whiff was not held on its last active frame four quiet frames after it");
            whiff.Play(Quiet, 100);
            Require(whiff.View().frozen && whiff.View().frames.size() == 10, "Idle frames changed the held bars");
            // Held just before and just after a page boundary: the quiet
            // frames that crossed it leave with the rest.
            Fight before; before.Play(Attack, 60); before.Play(Quiet, Quiets - 1);
            Require(before.Cell(0) == 60, "The quiet frames did not go on to the next page as they came");
            before.Play(Quiet);
            Require(before.View().frozen && before.Cell(59) == 59 && before.Cell(0) == 0 && MeterBackLimit(before.View().frames.size(), before.View().length, 60) == 0,
                "An exchange that filled the bars was held on the next, empty page");
            Fight after; after.Play(Attack, 61); after.Play(Quiet, Quiets);
            Require(after.View().frozen && after.Cell(0) == 60 && after.Cell(1) == -1 && after.Cell(59, 60, 1) == 59,
                "An exchange one frame past the bars was not held on its last page");
        }
        // A frame trap's gap, shorter than four frames, stays in the exchange.
        {
            Fight trap; trap.Play(Attack, 10); trap.Play(Struck, 5); trap.Play(Quiet, Quiets - 1); trap.Play(Attack, 10); trap.Play(Struck, 6);
            trap.Play(Quiet, Quiets);
            const auto& frames = trap.View().frames;
            Require(trap.View().frozen && frames.size() == 34, "A frame trap's gap ended the exchange");
            for (int i = 15; i < 18; ++i) Require(frames[i].cells[0].kind == MeterKind::Neutral && frames[i].cells[1].kind == MeterKind::Neutral,
                "The gap's frames were not kept");
        }
        // Restart: any action after a hold starts the next exchange on clear bars; a jump is no action.
        {
            for (const Step action : {Attack, Step{10, 0}, Step{0, 10}}) {
                Fight again; again.Play(Attack, 30); again.Play(Struck, 10); again.Play(Quiet, Quiets + 20);
                again.Play({2, 0}, 30);
                Require(again.View().frozen && again.View().frames.size() == 40, "A jump started the next exchange");
                const int at = again.at;
                again.Play(action);
                Require(!again.View().frozen && again.View().frames.size() == 1 && again.View().length == 1 && again.View().frames.front().frame == at &&
                    !again.View().frames.front().cells[0].newAction && again.Cell(0) == 0 && again.Cell(1) == -1,
                    "An action after the hold did not start the next exchange on clear bars");
            }
        }
        // A projectile: its thrower's frames are held once it recovers, and
        // the other's guard, then its block, show the exchange whole from the
        // throw, its flight among it.
        {
            Fight fireball; fireball.Play(Quiet, 5);
            const int thrown = fireball.at;
            fireball.Play(Attack, 12); fireball.Play(Quiet, 30);
            Require(fireball.View().frozen && fireball.View().frames.size() == 12, "The thrower's frames were not held once it recovered");
            fireball.Play({0, 14}, 2); fireball.Play({0, 22}, 10);
            Require(!fireball.View().frozen && fireball.View().frames.front().frame == thrown && fireball.View().frames.size() == 54 &&
                fireball.View().frames.front().cells[0].phase == Phase::Attack, "The projectile's guard and block did not show the exchange whole");
            fireball.Play(Quiet, Quiets);
            Require(fireball.View().frozen && fireball.View().frames.size() == 54, "The blocked projectile's exchange was not held");
        }
        // Each hit of a combo marks the attacker's cell with its advantage, from
        // the game's counts of frames until each fighter can act; a knockdown
        // (the game's 1000) marks nothing.
        {
            FrameMeter meter;
            const auto play = [&](int frame, unsigned own, int ownAction, int ownLeft, unsigned other, float combo, int otherLeft) {
                std::array<FighterSample, 2> both;
                for (auto& fighter : both) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false; }
                both[0].status = own; both[0].action = ownAction; both[0].actionFrame = static_cast<float>(frame); both[0].untilActionable = ownLeft;
                both[1].status = other; both[1].action = other ? 101 : 0; both[1].actionFrame = static_cast<float>(frame);
                both[1].comboDamage = combo; both[1].untilActionable = otherLeft;
                meter.Observe(frame, both);
                return meter.View().frames.empty() ? MeterCell{} : meter.View().frames.back().cells[0];
            };
            play(0, 0, 0, 0, 0, 0, 0);
            Require(play(1, 16, 100, 20, 0, 0, 0).hitAdvantage == NoHitAdvantage, "A whiffing frame was marked");
            const auto first = play(2, 16, 100, 20, 21, 10, 23);
            Require(first.hitAdvantage == 3, "A hit did not carry its advantage");
            Require(play(3, 16, 100, 19, 21, 10, 22).hitAdvantage == NoHitAdvantage, "A frame after the hit was marked");
            const auto second = play(4, 16, 102, 30, 21, 20, 26);
            Require(second.hitAdvantage == -4, "A combo's next hit did not carry its own advantage");
            Require(play(5, 16, 102, 29, 21, 30, 1000).hitAdvantage == NoHitAdvantage, "A knockdown was marked");
        }
        // Hitstop: frames where neither fighter's animation moves leave the bars.
        {
            Fight stop; stop.Play(Attack, 3);
            std::array<FighterSample, 2> both = stop.View().current;
            for (int i = 0; i < 8; ++i) stop.meter.Observe(stop.at++, both);
            Require(stop.View().frames.size() == 3 && stop.View().length == 3, "Hitstop frames were drawn");
            stop.Play(Attack);
            Require(stop.View().frames.size() == 4, "The frame after hitstop was not drawn");
        }
        // As the game plays a hit: the attacker's frame repeats on the frame
        // it lands and the one hit starts its reaction at 0, so hitstop takes
        // that frame too. The hit's mark goes on the frame it landed on, the
        // reaction's start and the hitstop line on the next frame drawn.
        {
            FrameMeter meter;
            const auto play = [&](int frame, float own, int otherAction, float other, int ownLeft, int otherLeft) {
                std::array<FighterSample, 2> both;
                for (auto& fighter : both) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false; }
                both[0].status = own > 0 ? 16 : 0; both[0].action = own > 0 ? 268 : 0; both[0].actionFrame = own; both[0].untilActionable = ownLeft;
                both[1].status = otherAction ? 21 : 0; both[1].action = otherAction; both[1].actionFrame = other; both[1].untilActionable = otherLeft;
                meter.Observe(frame, both);
            };
            play(0, 0, 0, 40, 0, 0);
            for (int f = 1; f <= 3; ++f) play(f, static_cast<float>(f), 0, 40.f + f, 0, 0);
            const auto drawn = meter.View().frames.size();
            play(4, 3, 131, 0, 15, 20);
            for (int f = 5; f < 12; ++f) play(f, 3, 131, 0, 19 - f, 24 - f);
            Require(meter.View().frames.size() == drawn, "The hit's own frame or its hitstop was drawn");
            play(12, 4, 131, 1.25f, 7, 12);
            const auto& landed = meter.View().frames[drawn - 1].cells[0];
            const auto& after = meter.View().frames.back().cells;
            Require(meter.View().frames.size() == drawn + 1 && landed.hitAdvantage == 5 && !landed.afterStop,
                "The hit's mark was not on the frame it landed on");
            Require(after[0].hitAdvantage == NoHitAdvantage && after[1].newAction && after[0].afterStop && after[1].afterStop,
                "The reaction's start and the hitstop line did not show on the frame after hitstop");
        }
        // The game's counts said -14 as Ryu's crouching HP landed, but the
        // other's stun stalled a frame: it was free 13 frames before Ryu, and
        // the exchange's last hit says what was measured.
        {
            FrameMeter meter;
            const auto play = [&](int frame, unsigned own, float ownFrame, unsigned other, int otherAction, float otherFrame, int ownLeft, int otherLeft) {
                std::array<FighterSample, 2> both;
                for (auto& fighter : both) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false; }
                both[0].status = own; both[0].action = own ? 270 : 0; both[0].actionFrame = ownFrame; both[0].untilActionable = ownLeft;
                both[1].status = other; both[1].action = otherAction; both[1].actionFrame = otherFrame; both[1].untilActionable = otherLeft;
                meter.Observe(frame, both);
            };
            int at = 0;
            play(at++, 0, 0, 0, 0, 0, 0, 0);
            for (int f = 1; f <= 4; ++f) play(at++, 16, static_cast<float>(f), 0, 0, static_cast<float>(f), 0, 0);
            play(at++, 16, 4, 21, 144, 0, 45, 31);
            const int landed = at - 1;
            for (int f = 1; f <= 20; ++f) play(at++, 16, 4.f + f, 21, 144, static_cast<float>(f), 0, 0);
            for (int f = 1; f <= 13; ++f) play(at++, 16, 24.f + f, 0, 0, static_cast<float>(f), 0, 0);
            for (int f = 1; f <= 3; ++f) play(at++, 0, static_cast<float>(f), 0, 0, 13.f + f, 0, 0);
            const auto& advantage = meter.View().lastAdvantage;
            Require(advantage.valid && advantage.frames[0] == -13, "The crouching HP's exchange was not measured at -13");
            const auto& frames = meter.View().frames;
            const auto marked = std::find_if(frames.begin(), frames.end(), [](const MeterFrame& frame) { return frame.cells[0].hitAdvantage != NoHitAdvantage; });
            Require(marked != frames.end() && marked->frame < landed && marked->cells[0].hitAdvantage == -13, "The last hit did not take the measured advantage");
        }
        std::puts("Frame meter run tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
