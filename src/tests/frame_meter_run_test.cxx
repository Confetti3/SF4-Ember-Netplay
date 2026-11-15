// The frame meter's bars as SF6's meter has them: they start on the first
// active frame of an exchange, fill left to right and go on from the left
// edge past their width, hold four quiet frames after it on its last active
// frame, and any action after that starts the next exchange.
#include "../training/FrameMeter.hxx"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace sf4e::training;
namespace bac = sf4e::bac;
namespace {
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
// Both fighters valid and still in `status`, seen once: the meter a move's frames are played on.
std::array<FighterSample, 2> Still(FrameMeter& meter, unsigned status = 0) {
    std::array<FighterSample, 2> pair;
    for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = status; fighter.action = 0; }
    meter.Observe(0, pair);
    return pair;
}
// A script's hitboxes, as the tests below play them.
constexpr auto Before = bac::HitPhase::Before, Out = bac::HitPhase::Out, Done = bac::HitPhase::Done;
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
        // Hitstop: frames the game holds both fighters in leave the bars.
        {
            Fight stop; stop.Play(Attack, 3);
            std::array<FighterSample, 2> both = stop.View().current;
            for (auto& fighter : both) fighter.hitStop = true;
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
                both[0].hitStop = both[1].hitStop = frame >= 4 && frame < 12;
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
        // The game's counts said -14 as Ryu's crouching HP landed, and still
        // through its hitstop, but the other's stun stalled a frame: it was
        // free 13 frames before Ryu, and the hit settles as its count runs
        // out, with Ryu's 13 left.
        {
            FrameMeter meter;
            const auto play = [&](int frame, unsigned own, float ownFrame, unsigned other, int otherAction, float otherFrame, int ownLeft, int otherLeft) {
                std::array<FighterSample, 2> both;
                for (auto& fighter : both) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false; }
                both[0].status = own; both[0].action = own ? 270 : 0; both[0].actionFrame = ownFrame; both[0].untilActionable = ownLeft;
                both[1].status = other; both[1].action = otherAction; both[1].actionFrame = otherFrame; both[1].untilActionable = otherLeft;
                both[0].hitStop = both[1].hitStop = other && ownFrame == 4;
                meter.Observe(frame, both);
            };
            int at = 0;
            play(at++, 0, 0, 0, 0, 0, 0, 0);
            for (int f = 1; f <= 4; ++f) play(at++, 16, static_cast<float>(f), 0, 0, static_cast<float>(f), 0, 0);
            play(at++, 16, 4, 21, 144, 0, 46, 32);
            const int landed = at - 1;
            // The other's count holds at 5 for a frame.
            constexpr int stalled = 15;
            for (int f = 1; f <= 12; ++f) play(at++, 16, 4, 21, 144, 0, 46 - f, 32 - f);
            for (int f = 1; f <= 20; ++f) play(at++, 16, 4.f + f, 21, 144, static_cast<float>(f), 34 - f, f <= stalled ? 20 - f : 21 - f);
            for (int f = 1; f <= 13; ++f) play(at++, 16, 24.f + f, 0, 0, static_cast<float>(f), 14 - f, 0);
            for (int f = 1; f <= 3; ++f) play(at++, 0, static_cast<float>(f), 0, 0, 13.f + f, 0, 0);
            const auto& advantage = meter.View().lastAdvantage;
            Require(advantage.valid && advantage.frames[0] == -13, "The crouching HP's exchange was not measured at -13");
            const auto& frames = meter.View().frames;
            const auto marked = std::find_if(frames.begin(), frames.end(), [](const MeterFrame& frame) { return frame.cells[0].hitAdvantage != NoHitAdvantage; });
            Require(marked != frames.end() && marked->frame < landed && marked->cells[0].hitAdvantage == -13, "The hit did not settle once the other's count ran out");
        }
        // A link: the attacker is free first, after the other's count stalled
        // a frame, so the first hit reads +6 from what the other has left,
        // not the +5 its counts said, and the next hit keeps its own mark.
        {
            FrameMeter meter;
            const auto play = [&](int frame, unsigned own, int ownAction, int ownLeft, float combo, int otherLeft) {
                std::array<FighterSample, 2> both;
                for (auto& fighter : both) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false; }
                both[0].status = own; both[0].action = ownAction; both[0].actionFrame = static_cast<float>(frame); both[0].untilActionable = ownLeft;
                both[1].status = combo > 0 ? 21 : 0; both[1].action = combo > 0 ? 144 : 0; both[1].actionFrame = static_cast<float>(frame);
                both[1].comboDamage = combo; both[1].untilActionable = otherLeft;
                meter.Observe(frame, both);
            };
            play(0, 0, 0, 0, 0, 0);
            play(1, 16, 100, 10, 0, 0);
            play(2, 16, 100, 10, 10, 15);
            constexpr int stalled = 3;
            for (int f = 1; f <= 10; ++f) play(2 + f, f < 10 ? 16 : 0, f < 10 ? 100 : 0, 10 - f, 10, f <= stalled ? 15 - f : 16 - f);
            play(13, 16, 102, 20, 10, 4);
            play(14, 16, 102, 20, 20, 23);
            std::vector<int> marks;
            for (const auto& frame : meter.View().frames)
                if (frame.cells[0].hitAdvantage != NoHitAdvantage) marks.push_back(frame.cells[0].hitAdvantage);
            Require(marks == std::vector<int>{6, 3}, "The link's first hit was not read again as the attacker became free");
        }
        for (unsigned from : {ActorStatus::Stand, ActorStatus::Jump}) {
            // Akuma's hyakki leaves the ground and lands in the common landing: its 4 frames are
            // the move's recovery. A jump's attack lands the same way, and that is no recovery.
            FrameMeter counted; auto pair = Still(counted, from);
            int tick = 1;
            pair[0].status = 16; pair[0].action = 425;
            for (auto phase : {Before, Out, Done}) { pair[0].hits = phase; pair[0].actionFrame = static_cast<float>(tick); counted.Observe(tick++, pair); }
            pair[0].status = ActorStatus::JumpToStand; pair[0].action = 9; pair[0].hits = bac::HitPhase::Untimed;
            for (int frame = 0; frame < 4; ++frame) { pair[0].actionFrame = static_cast<float>(frame); pair[1].actionFrame = static_cast<float>(tick); counted.Observe(tick++, pair); }
            pair[0].status = ActorStatus::Forward; pair[0].action = 16; pair[0].actionFrame = 0; counted.Observe(tick++, pair);
            Require(counted.View().moves[0].recovery == (from == ActorStatus::Stand ? 5 : 1), "A ground move's landing was not its recovery, or a jump attack's was");
        }
        {
            // Evil Ryu's 421LP: the fireball comes out the step after its spawn (frame 47, boundary 48)
            // and hits 17 game frames later, after he has recovered.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 462; pair[0].firstActiveFrame = 48; pair[0].lastActiveFrame = 49;
            pair[0].boundaryProvenance = BoundaryProvenance::BacEffectSpawn; pair[0].projectileDelay = 17;
            int tick = 1;
            for (; tick <= 55; ++tick) { pair[0].actionFrame = static_cast<float>(tick); counted.Observe(tick, pair); }
            pair[0] = pair[1];
            for (; tick <= 64; ++tick) { pair[0].actionFrame = pair[1].actionFrame = static_cast<float>(tick); counted.Observe(tick, pair); }
            Require(counted.View().startupFrames[0] < 0, "A falling fireball hit before its delay");
            pair[0].actionFrame = pair[1].actionFrame = 65; counted.Observe(tick++, pair);
            Require(counted.View().startupFrames[0] == 65, "A falling fireball's startup missed its delay");
        }
        for (const bool parry : {false, true}) {
            // El Fuerte's 214K lands in a script with no hit whose header says 0 to 2: no active
            // frames there. A parry opens with such a script, and keeps its header's.
            FrameMeter counted; auto pair = Still(counted);
            int tick = 1;
            pair[0].status = 16; pair[0].commandMove = 0x3000;
            if (!parry) {
                pair[0].action = 465;
                for (auto phase : {Before, Out, Out, Done}) { pair[0].hits = phase; pair[0].actionFrame = static_cast<float>(tick); counted.Observe(tick++, pair); }
            }
            pair[0].action = 464; pair[0].hits = bac::HitPhase::NoHit; pair[0].firstActiveFrame = 0; pair[0].lastActiveFrame = 2;
            pair[0].boundaryProvenance = BoundaryProvenance::BacActionHeader;
            for (int frame = 0; frame < 4; ++frame) { pair[0].actionFrame = static_cast<float>(frame); pair[1].actionFrame = static_cast<float>(tick); counted.Observe(tick++, pair); }
            Require(counted.View().moves[0].active == 2, parry ? "A parry lost its header's active frames" : "A hitless landing's header counted as active");
        }
        {
            // Abel's EX 236P grabs on its first active frame, drops Ryu and hits him, grabs him
            // again and lets go: active 1, and recovery from the last release.
            FrameMeter counted; auto pair = Still(counted);
            int tick = 1;
            pair[0].status = 16; pair[0].commandMove = 0x4000;
            const auto step = [&](int action, bac::HitPhase phase, unsigned other) {
                pair[0].action = action; pair[0].hits = phase; pair[0].actionFrame = static_cast<float>(tick);
                pair[1].status = other; pair[1].action = other ? 500 + static_cast<int>(other) : 0; pair[1].actionFrame = static_cast<float>(tick);
                counted.Observe(tick++, pair);
            };
            step(515, Before, 0); step(515, Out, 0);
            for (int i = 0; i < 3; ++i) step(516, Before, ActorStatus::Sequence);
            step(518, Before, ActorStatus::DamageBlow); step(518, Out, ActorStatus::DamageBlow); step(518, Done, ActorStatus::DamageBlow);
            for (int i = 0; i < 3; ++i) step(521, Before, ActorStatus::Sequence);
            for (int i = 0; i < 3; ++i) step(521, Done, ActorStatus::Down);
            Require(counted.View().startupFrames[0] == 2 && counted.View().moves[0].active == 1 && counted.View().moves[0].recovery == 3,
                "A grab's follow-up hits counted as active, or its recovery not from the last release");
        }
        {
            // Balrog's headbutt: six startup frames in one script, then a second script that
            // opens active. Its startup is seven, not one counted again from the second script.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 346; pair[0].hits = Before;
            for (int frame = 1; frame <= 6; ++frame) { pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[0].action = 369; pair[0].hits = Out; pair[0].actionFrame = 0; counted.Observe(7, pair);
            pair[0].actionFrame = 1; counted.Observe(8, pair);
            Require(counted.View().startupFrames[0] == 7 && counted.View().moves[0].active == 2, "A move's active second script restarted its startup");
        }
        {
            // Vega's roll: the first script is active 3 frames and ends with one more, the second
            // (same command move) winds up and hits 1, then 4 frames recover. Its frame 0 is a
            // frame too, so 3(3)1: active 7.
            FrameMeter counted; auto pair = Still(counted);
            int tick = 1;
            pair[0].status = 16; pair[0].action = 416; pair[0].commandMove = 0x1000;
            for (auto phase : {Before, Out, Out, Out, Done}) { pair[0].hits = phase; pair[0].actionFrame = static_cast<float>(tick); counted.Observe(tick++, pair); }
            Require(counted.View().moves[0].active == 3 && counted.View().moves[0].recovery == 1, "The first script's hits or its tail miscounted");
            pair[0].action = 417;
            int frame = 0;
            for (auto phase : {Before, Before, Out, Done, Done, Done, Done}) { pair[0].hits = phase; pair[0].actionFrame = static_cast<float>(frame++); counted.Observe(tick++, pair); }
            Require(counted.View().startupFrames[0] == 2 && counted.View().moves[0].active == 7 && counted.View().moves[0].recovery == 4,
                "A two-script move's gap or recovery miscounted");
        }
        {
            // Poison's 236LP: the fireball spawns at script frame 33 (boundary 34) while the
            // script plays 2 frames a step; the step from 32.99 to 34.99 spawns it, the next hits.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 443; pair[0].firstActiveFrame = 34; pair[0].lastActiveFrame = 35;
            pair[0].boundaryProvenance = BoundaryProvenance::BacEffectSpawn;
            int tick = 1;
            for (float frame : {28.99f, 30.99f, 32.99f, 34.99f}) { pair[0].actionFrame = frame; counted.Observe(tick++, pair); }
            Require(counted.View().startupFrames[0] < 0, "A fireball hit the step it spawned");
            pair[0].actionFrame = 36.99f; counted.Observe(tick++, pair);
            Require(counted.View().startupFrames[0] == 5, "A fireball's startup missed the step after its spawn");
            pair[0].actionFrame = 38.99f; counted.Observe(tick++, pair);
            Require(counted.View().moves[0].active == 1 && counted.View().moves[0].recovery == 1, "A fireball's step after its spawn was not its active one");
        }
        {
            // Akuma's red fireball LP: spawn frame 47 (boundary 48) at 1.5 frames a step. The step
            // from 46 to 47.5 spawns it and the one to 49 hits, though 49 lies past the boundary.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 388; pair[0].firstActiveFrame = 48; pair[0].lastActiveFrame = 49;
            pair[0].boundaryProvenance = BoundaryProvenance::BacEffectSpawn;
            int tick = 1;
            for (float frame : {43.f, 44.5f, 46.f, 47.5f}) { pair[0].actionFrame = frame; counted.Observe(tick++, pair); }
            Require(counted.View().startupFrames[0] < 0, "A fireball hit the step it spawned (1.5 a step)");
            pair[0].actionFrame = 49.f; counted.Observe(tick++, pair);
            Require(counted.View().startupFrames[0] == 5, "A fireball's step after its spawn was missed at 1.5 a step");
            pair[0].actionFrame = 50.5f; counted.Observe(tick++, pair);
            Require(counted.View().moves[0].active == 1 && counted.View().moves[0].recovery == 1, "A fireball had no active frame at 1.5 a step");
        }
        for (const bool input : {false, true}) {
            // Seth's 214LK plays two scripts, the second opening in startup, under one command
            // move: one move, startup counted from the first. A target combo is a new input
            // (another command move) and so a new move with its own startup.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 450; pair[0].hits = Before; pair[0].commandMove = 0x1000;
            // The target combo's first hit is out on frame 5; Seth's first script has none.
            for (int frame = 1; frame <= 5; ++frame) { pair[0].hits = input && frame == 5 ? Out : Before; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[0].action = 464; pair[0].commandMove = input ? 0x2000 : 0x1000;
            for (int frame = 0; frame < 7; ++frame) { pair[0].hits = frame < 6 ? Before : Out; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(6 + frame, pair); }
            Require(counted.View().startupFrames[0] == (input ? 7 : 12), input ? "A target combo's own startup was not counted" : "A move's own next script restarted its startup");
        }
        {
            // A plinked EX input: the first button makes a normal for a frame, then the special
            // takes over. The special's startup does not include the normal's frame.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 100; pair[0].hits = Before; pair[0].commandMove = 0x1000; pair[0].actionFrame = 1;
            counted.Observe(1, pair);
            pair[0].action = 300; pair[0].commandMove = 0x2000;
            for (int frame = 0; frame < 5; ++frame) { pair[0].hits = frame < 4 ? Before : Out; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(2 + frame, pair); }
            Require(counted.View().startupFrames[0] == 5, "A plinked special counted the normal's frame in its startup");
        }
        {
            // Akuma's Demon Flip plays a stance that attacks nothing; a follow-up pressed 10 frames
            // in is a new input whose startup counts from its own first frame.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 445; pair[0].commandMove = 0x1000; pair[0].hits = bac::HitPhase::Untimed;
            for (int frame = 1; frame <= 10; ++frame) { pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[0].action = 447; pair[0].commandMove = 0x2000;
            for (int frame = 0; frame < 9; ++frame) { pair[0].hits = frame < 8 ? Before : Out; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(11 + frame, pair); }
            Require(counted.View().startupFrames[0] == 9, "A follow-up from a stance counted the stance's frames in its startup");
        }
        {
            // Adon's far HK hits twice: Ryu's reaction starts again from its first frame as the
            // second hitstop begins. That frame is hitstop, not one the reaction moved on.
            FighterSample before, after;
            before.valid = after.valid = true; before.action = after.action = 133;
            before.actionFrame = 2.375f; after.actionFrame = 0;
            Require(!AnimationMoved(before, after), "A reaction started again by a second hit counted as moving");
            after.actionFrame = 1.2f;
            Require(AnimationMoved(before, after), "A restarted reaction past its first frame did not count as moving");
            before.actionFrame = 1; after.actionFrame = 1;
            Require(!AnimationMoved(before, after), "A held frame counted as moving");
            after.action = 134;
            Require(AnimationMoved(before, after), "A new action past its first frame did not count as moving");
        }
        {
            // Blanka stands in a negative posture, as the game has it in the air or thrown: an
            // attack from it is still measured.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            pair[0].posture = -1; counted.Observe(0, pair);
            pair[0].status = 16; pair[0].action = 100; pair[0].actionFrame = 1; counted.Observe(1, pair);
            pair[1].status = 21; pair[1].action = 200; pair[0].actionFrame = 2; counted.Observe(2, pair);
            Require(counted.View().advantage.pending && counted.View().advantage.attacker == 0, "An attack from a negative posture was taken for a bad read");
        }
        {
            // Akuma's demon flip punch sends Ryu from standing straight into the bound;
            // the hit is still contact, with his combo damage changing on it.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            pair[1].comboDamage = 320;
            counted.Observe(0, pair);
            pair[0].status = 16; pair[0].action = 425; pair[0].posture = -1;
            for (int frame = 1; frame <= 3; ++frame) { pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[1].status = 18; pair[1].action = 28; pair[1].comboDamage = 130; pair[0].actionFrame = 4; counted.Observe(4, pair);
            Require(counted.View().advantage.pending && counted.View().advantage.attacker == 0 && !counted.View().advantage.blocked,
                "A hit straight into the bound was not contact");
        }
        {
            // Adon's throw: the grab is active on frame 3, then Adon plays the throw in Skill
            // while Ryu is held in a sequence. The throw is no new move: startup stays 3 and
            // its frames are drawn as the sequence; once Ryu is let go the rest is recovery.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 288;
            for (int frame = 1; frame <= 3; ++frame) { pair[0].hits = frame < 3 ? Before : Out; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[0].action = 292; pair[0].hits = Before; pair[1].status = 24; pair[1].action = 293;
            for (int frame = 0; frame < 5; ++frame) { pair[0].actionFrame = static_cast<float>(frame); pair[1].actionFrame = static_cast<float>(frame); counted.Observe(4 + frame, pair); }
            Require(counted.View().startupFrames[0] == 3 && counted.View().frames.back().cells[0].kind == MeterKind::Sequence,
                "A throw played in Skill restarted the thrower's startup, or was not drawn as its sequence");
            pair[0].hits = Done; pair[1].status = 20; pair[1].action = 34;
            for (int frame = 5; frame < 8; ++frame) { pair[0].actionFrame = static_cast<float>(frame); pair[1].actionFrame = static_cast<float>(frame); counted.Observe(4 + frame, pair); }
            Require(counted.View().startupFrames[0] == 3 && counted.View().moves[0].recovery == 3, "A thrower's frames after the throw were not its recovery");
        }
        {
            // Ken's close MK (Omega) hits twice from one script, boxes 8 to 10 and 14 to 17. Frame
            // data counts its active frames from the first hit to the last, the gap included: 2(4)3, 9.
            FrameMeter counted; auto pair = Still(counted);
            pair[0].status = 16; pair[0].action = 100;
            for (int frame = 1; frame <= 20; ++frame) {
                pair[0].hits = frame < 8 ? Before : frame < 10 ? Out : frame < 14 ? bac::HitPhase::Between : frame < 17 ? Out : Done;
                pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair);
            }
            Require(counted.View().startupFrames[0] == 8 && counted.View().moves[0].active == 9 && counted.View().moves[0].recovery == 4,
                "A two-hit move's active frames were not counted from its first hit to its last");
        }
        {
            // A crouching jab of 3, 2 and 7 (first=3, last=5, ending on frame 11), cells as played,
            // as Street Fighter 6 draws it: 2, 2 and 7. The numbers are frame data's.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
            pair[0].status = 16; pair[0].action = 100; pair[0].firstActiveFrame = 3; pair[0].lastActiveFrame = 5;
            for (int frame = 1; frame <= 11; ++frame) { pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            std::array<int, 3> cells{};
            for (const auto& frame : counted.View().frames) {
                const auto kind = frame.cells[0].kind;
                if (kind == MeterKind::Startup || kind == MeterKind::Active || kind == MeterKind::Recovery)
                    ++cells[kind == MeterKind::Startup ? 0 : kind == MeterKind::Active ? 1 : 2];
            }
            Require(cells == (std::array<int, 3>{2, 2, 7}), "An attack's cells were not drawn as they play");
            Require(counted.View().startupFrames[0] == 3 && counted.View().moves[0].active == 2 && counted.View().moves[0].recovery == 7,
                "The cells changed a move's numbers");
        }
        std::puts("Frame meter run tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
