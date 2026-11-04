// Frame meter cases that read samples through the per-frame observer: action
// chains, advantage, startup, recovery and the fighters' hitstop. How the bars
// start, fill, hold and page: frame_meter_run_test.cxx.
#include "../training/FrameMeter.hxx"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace sf4e::training;
namespace bac = sf4e::bac;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
int main() {
    try {
        // How the bars start, fill, hold and page: frame_meter_run_test.cxx.
        // Reproduce action chains through the same per-frame observer used by
        // native training. An internal move change must not discard contact.
        for (int variant = 0; variant < 4; ++variant) {
            FrameMeter chain;
            std::array<FighterSample, 2> samples;
            for (int frame = 0; frame <= 20; ++frame) {
                for (auto& sample : samples) {
                    sample.valid = true; sample.posture = 0; sample.timeScale = 1;
                    sample.basicActionInhibited = false; sample.status = 0;
                    sample.action = 0; sample.actionFrame = static_cast<float>(frame);
                }
                samples[0].status = frame >= 1 && frame < 15 ? 16 : 0;
                samples[0].action = samples[0].status ? (frame < 6 ? 100 : 101) : 0;
                // Target combo / special cancel, airborne special phase, and
                // projectile contact after the owner's recovery, respectively.
                if (variant == 1 && frame >= 6 && frame < 12) samples[0].posture = 2;
                if (variant == 1 && frame >= 6 && frame < 12) samples[0].timeScale = .5f;
                if (variant == 2 && frame >= 8) { samples[0].status = 0; samples[0].action = 0; }
                const int contact = variant == 2 ? 12 : 4;
                samples[1].status = frame >= contact && frame < 18 ? 22 : 0;
                samples[1].action = samples[1].status ? (frame < 9 ? 200 : 201) : 0;
                if (variant == 3 && frame >= contact && frame < 18) {
                    samples[1].status = frame < 7 ? 23 : frame < 14 ? 19 : 20;
                    samples[1].posture = 3;
                }
                chain.Observe(frame, samples);
            }
            Require(chain.View().advantage.valid, variant == 0 ? "Target combo/special action chain lost frame advantage" :
                variant == 1 ? "Airborne special phase lost frame advantage" : "Delayed special contact lost frame advantage");
            Require(chain.View().advantage.frames[0] == (variant == 2 ? 10 : 3), "Action chain recovery boundary wrong");
            Require(chain.View().advantage.knockdown == (variant == 3), "Wakeup comparison label wrong");
        }
        // The runtime passes the signed 16-bit integral of native simulation
        // time. Reproduce a late-session exchange, including both boundaries.
        for (const int start : {32760, 33000, 65530}) {
            FrameMeter longSession;
            std::array<FighterSample, 2> samples;
            for (int step = 0; step <= 20; ++step) {
                for (auto& sample : samples) {
                    sample = FighterSample{};
                    sample.valid = true; sample.posture = 0; sample.timeScale = 1;
                    sample.basicActionInhibited = false; sample.status = 0;
                    sample.action = 0; sample.actionFrame = static_cast<float>(step);
                }
                samples[0].status = step >= 1 && step < 15 ? 16 : 0;
                samples[0].action = samples[0].status ? 100 : 0;
                samples[1].status = step >= 4 && step < 18 ? 22 : 0;
                samples[1].action = samples[1].status ? 200 : 0;
                const int raw = (start + step) & 0xffff;
                longSession.Observe(raw >= 32768 ? raw - 65536 : raw, samples);
            }
            Require(longSession.View().advantage.valid,
                "Native signed-frame rollover stopped training advantage");
            Require(longSession.View().advantage.frames[0] == 3 &&
                longSession.View().advantage.frames[1] == -3,
                "Native frame boundary changed the recovery interval");
        }
        // More than two full counter periods without reloading training.
        FrameMeter soak;
        std::array<FighterSample, 2> soakSamples;
        for (int tick = 0; tick < 131100; ++tick) {
            const int step = tick % 30;
            for (auto& sample : soakSamples) {
                sample = FighterSample{};
                sample.valid = true; sample.posture = 0; sample.timeScale = 1;
                sample.basicActionInhibited = false; sample.status = 0;
                sample.action = 0; sample.actionFrame = static_cast<float>(step);
            }
            soakSamples[0].status = step >= 1 && step < 15 ? 16 : 0;
            soakSamples[0].action = soakSamples[0].status ? 100 : 0;
            soakSamples[1].status = step >= 4 && step < 18 ? 22 : 0;
            soakSamples[1].action = soakSamples[1].status ? 200 : 0;
            const int raw = tick & 0xffff;
            soak.Observe(raw >= 32768 ? raw - 65536 : raw, soakSamples);
            if (step >= 18) Require(soak.View().advantage.valid && soak.View().advantage.frames[0] == 3,
                "Repeated long-session exchanges lost frame advantage");
        }
        FrameMeter startup;
        std::array<FighterSample, 2> startupSamples;
        for (auto& sample : startupSamples) {
            sample.valid = true; sample.status = 0; sample.action = 0;
            sample.posture = 0; sample.timeScale = 1; sample.basicActionInhibited = false;
        }
        startup.Observe(0, startupSamples);
        auto& move = startupSamples[0];
        move.status = 16; move.action = 100; move.firstActiveFrame = 9; move.lastActiveFrame = 10;
        move.actionFrame = 3; startup.Observe(1, startupSamples);
        move.actionFrame = 6; startup.Observe(2, startupSamples);
        move.timeScale = 0; startup.Observe(3, startupSamples); // A frame frozen by a super's freeze.
        move.actionFrame = 9; startup.Observe(4, startupSamples);
        Require(startup.View().startupFrames[0] == 3, "Startup counted BAC ticks or freeze instead of advancing frames");
        move.action = 101; move.actionFrame = 1; move.firstActiveFrame = 2; move.lastActiveFrame = 3; move.timeScale = 1;
        startup.Observe(5, startupSamples); move.actionFrame = 2; startup.Observe(6, startupSamples);
        Require(startup.View().startupFrames[0] == 2, "Target combo follow-up did not update startup");
        move.action = 102; move.actionFrame = 0; move.firstActiveFrame = -1;
        startup.Observe(7, startupSamples);
        Require(startup.View().startupFrames[0] == 2, "Recovery-only special action discarded startup");
        move.status = 0; startup.Observe(8, startupSamples);
        move.status = 16; move.action = 103; move.firstActiveFrame = -1; move.actionFrame = 1;
        startup.Observe(9, startupSamples); move.actionFrame = 2; startup.Observe(10, startupSamples);
        Require(startup.View().startupFrames[0] == -1, "Missing attack boundary fabricated startup");
        Require(startup.View().startupUnavailable[0] == MeasurementUnavailable::NoAttackBoundary,
            "Missing attack boundary had no explanation");
        move.action = 104; move.firstActiveFrame = 1; move.lastActiveFrame = 2; move.actionFrame = 1;
        move.boundaryProvenance = BoundaryProvenance::BacActionHeader; startup.Observe(11, startupSamples);
        Require(startup.View().startupFrames[0] == 3, "Multi-phase startup discarded its initial phase");
        Require(startup.View().startupBoundaryProvenance[0] == BoundaryProvenance::BacActionHeader,
            "Multi-phase startup retained provenance from the earlier boundary-less action");
        startup.Reset();
        Require(startup.View().startupFrames[0] == -1, "Reset retained startup");

        FrameMeter meter;
        std::array<FighterSample, 2> fighters;
        fighters[0].valid = fighters[1].valid = true;
        fighters[0].posture = fighters[1].posture = 0;
        fighters[0].status = 16; fighters[1].status = 22;
        fighters[0].action = 100; fighters[1].action = 200;
        // Animations advance, or the frames read as hitstop and leave the bars.
        const auto advance = [&](int i) { fighters[0].actionFrame = fighters[1].actionFrame = static_cast<float>(i); };
        for (int i = 0; i < 5; ++i) { advance(i); meter.Observe(i, fighters); }
        Require(meter.View().stateFrames[0] == 5, "State duration wrong");
        fighters[0].action = 101; advance(5); meter.Observe(5, fighters);
        Require(meter.View().lastAttackFrames[0] == 5 && meter.View().actionFrames[0] == 1, "Cancelled action duration wrong");
        Require(ClassifyStatus(16) == Phase::Attack && ClassifyStatus(22) == Phase::Guard && ClassifyStatus(99) == Phase::Unknown, "Native status classification wrong");
        fighters[0].status = fighters[1].status = 0;
        for (int i = 6; i < 50; ++i) { advance(i); meter.Observe(i, fighters); }
        Require(meter.View().frozen && meter.View().frames.back().frame == 5 && meter.View().length == 6, "Idle did not hold exchange on its last active frame");
        fighters[0].status = 16; fighters[0].action = 103; fighters[1].status = 22; fighters[1].action = 201;
        for (int i = 50; i < 50 + 3 * static_cast<int>(MeterHistory); ++i) { advance(i); meter.Observe(i, fighters); }
        Require(meter.View().frames.size() == MeterHistory && meter.View().length == 3 * MeterHistory, "Frame meter unbounded");
        meter.Observe(10, fighters);
        Require(meter.View().frames.size() == 1 && meter.View().stateFrames[0] == 1, "Timeline crossed reset");
        fighters[0].valid = false; meter.Observe(11, fighters);
        Require(meter.View().stateFrames[0] == 0, "Missing actor fabricated state duration");

        // Grounded block/hit exchanges with known recovery frames. Reuse the
        // sampling path, including repeated hitstop frames and mirrored sides.
        auto exchange = [&](int attacker, unsigned reaction, int attackEnd, int defenderEnd) {
            meter.Reset(); fighters = {};
            for (int frame = 0; frame <= (std::max)(attackEnd, defenderEnd); ++frame) {
                for (int side = 0; side < 2; ++side) {
                    auto& fighter = fighters[side];
                    fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1;
                    fighter.basicActionInhibited = false;
                    fighter.status = side == attacker ? (frame >= 1 && frame < attackEnd ? 16 : 0) :
                        (frame >= 3 && frame < defenderEnd ? reaction : 0);
                    fighter.action = fighter.status ? 100 + side : 0;
                    fighter.actionFrame = static_cast<float>(frame);
                }
                meter.Observe(frame, fighters);
            }
        };
        exchange(0, 22, 10, 15);
        Require(meter.View().advantage.valid && meter.View().advantage.frames[0] == 5 &&
            meter.View().advantage.frames[1] == -5, "Positive block advantage or reciprocal value wrong");
        meter.Observe(16, fighters);
        Require(meter.View().advantage.valid && meter.View().advantage.frames[0] == 5, "Idle discarded completed advantage");
        exchange(0, 21, 13, 10);
        Require(meter.View().advantage.valid && meter.View().advantage.frames[0] == -3, "Negative hit advantage wrong");
        exchange(1, 22, 10, 15);
        Require(meter.View().advantage.valid && meter.View().advantage.frames[1] == 5, "P2 attack was not mirrored");
        exchange(0, 22, 10, 10);
        Require(meter.View().advantage.valid && meter.View().advantage.frames[0] == 0 &&
            meter.View().advantage.frames[1] == 0, "Simultaneous recovery was not zero");
        fighters[0].status = 16; fighters[0].action = 100; meter.Observe(11, fighters);
        Require(!meter.View().advantage.valid, "New attack retained stale advantage");
        fighters[1].status = 14; fighters[1].action = 200; meter.Observe(12, fighters);
        Require(!meter.View().advantage.pending, "Guard posture fabricated block contact");
        fighters[1].status = 22; meter.Observe(13, fighters);
        Require(meter.View().advantage.pending, "Block contact did not start measurement");
        fighters[0].status = 0; fighters[0].action = 0; fighters[0].timeScale = 0; meter.Observe(14, fighters);
        fighters[0].timeScale = 1; meter.Observe(15, fighters);
        fighters[1].status = 0; fighters[1].action = 0; meter.Observe(16, fighters);
        Require(meter.View().advantage.valid && meter.View().advantage.frames[0] == 1, "Frozen fighter counted as recovered");
        fighters[0].status = 16; fighters[0].action = 100; meter.Observe(17, fighters);
        fighters[1].status = 22; fighters[1].action = 200; meter.Observe(18, fighters);
        fighters[0].action = 101; meter.Observe(19, fighters);
        Require(meter.View().advantage.pending && !meter.View().advantage.valid, "Cancel discarded the active exchange");
        fighters[1].actionFrame = -1; meter.Observe(20, fighters);
        Require(meter.View().advantage.pending, "Follow-up contact discarded the cancelled sequence");
        // A multi-hit action restarts the comparison at its last contact.
        exchange(0, 22, 10, 15);
        fighters[0].status = 16; fighters[0].action = 100; meter.Observe(16, fighters);
        fighters[1].status = 22; fighters[1].action = 200; meter.Observe(17, fighters);
        fighters[1].status = 14; meter.Observe(18, fighters);
        fighters[1].status = 22; meter.Observe(19, fighters);
        fighters[0].status = 0; fighters[0].action = 0; meter.Observe(20, fighters);
        fighters[1].status = 0; fighters[1].basicActionInhibited = true; meter.Observe(21, fighters);
        Require(meter.View().advantage.pending, "Native action inhibit counted as recovery");
        fighters[1].basicActionInhibited = false; meter.Observe(22, fighters);
        Require(meter.View().advantage.valid && meter.View().advantage.frames[0] == 2,
            "Repeated hit reused earlier recovery boundary");
        exchange(0, 22, 10, 15);
        fighters[0].posture = 2; meter.Observe(16, fighters);
        Require(meter.View().advantage.valid, "Movement discarded completed exchange");
        exchange(0, 22, 10, 15);
        fighters[1].valid = false; meter.Observe(16, fighters);
        Require(!meter.View().advantage.valid, "Missing actor kept advantage");
        exchange(0, 22, 10, 15); meter.Observe(18, fighters);
        Require(!meter.View().advantage.valid, "Advantage crossed a simulation gap");
        exchange(0, 22, 10, 15); meter.Reset();
        Require(!meter.View().advantage.valid, "Practice reset retained advantage");
        // The last reading is shown through a whiff and a walk, still shown
        // while the next exchange is measured, and replaced by its reading.
        exchange(0, 22, 10, 15);
        fighters[0].status = 16; fighters[0].action = 100; meter.Observe(16, fighters);
        fighters[0].status = 0; fighters[0].action = 0; meter.Observe(17, fighters);
        fighters[1].status = 10; fighters[1].action = 5; meter.Observe(18, fighters);
        Require(!meter.View().advantage.valid && ShownAdvantage(meter.View()).valid && ShownAdvantage(meter.View()).frames[0] == 5,
            "A whiff or a walk took the last reading away");
        fighters[1].status = 16; fighters[1].action = 201; meter.Observe(19, fighters);
        fighters[0].status = 21; fighters[0].action = 101; meter.Observe(20, fighters);
        Require(meter.View().advantage.pending && ShownAdvantage(meter.View()).frames[0] == 5 && ShownAdvantage(meter.View()).attacker == 0,
            "A new exchange did not show the last reading while it was measured");
        fighters[1].status = 0; fighters[1].action = 0; meter.Observe(21, fighters);
        fighters[0].status = 0; fighters[0].action = 0; meter.Observe(22, fighters);
        Require(meter.View().lastAdvantage.valid && ShownAdvantage(meter.View()).frames[0] == -1 && ShownAdvantage(meter.View()).attacker == 1,
            "The next reading did not replace the last one");
        // A reset (the reset key's checkpoint, Clear history, the battle's end)
        // and a gap in the frames clear the last reading and the bars.
        meter.Reset();
        Require(!ShownAdvantage(meter.View()).valid && meter.View().frames.empty(), "A reset kept the last reading or its bars");
        exchange(0, 22, 10, 15); meter.Observe(18, fighters);
        Require(!ShownAdvantage(meter.View()).valid && meter.View().frames.empty(), "The last reading crossed a simulation gap");
        {
            // The meter resolves each cell as its frame arrives: a guard held
            // from a jump guards nothing in the air and reads as nothing, and
            // stays so after the jump has left the kept frames; a grounded
            // guard and a blocked hit read as guarding. Runs part where a new
            // action begins (a cancel), never inside a guard, and at each hit
            // of a combo.
            FrameMeter resolved; std::array<FighterSample, 2> both;
            int observed = 0;
            const auto observe = [&](unsigned p1, int a1, unsigned p2, int a2, float damage = 0, float frame = 1) {
                for (auto& fighter : both) { fighter = FighterSample{}; fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; }
                both[0].status = p1; both[0].action = a1; both[0].actionFrame = frame;
                both[1].status = p2; both[1].action = a2; both[1].comboDamage = damage; both[1].actionFrame = frame;
                resolved.Observe(observed++, both);
                // An idle frame before any action leaves the bars empty.
                return resolved.View().frames.empty() ? std::array<MeterCell, 2>{} : resolved.View().frames.back().cells;
            };
            observe(ActorStatus::Jump, 1, ActorStatus::Skill, 9);
            for (int held = 0; held < static_cast<int>(MeterHistory) + 40; ++held)
                Require(observe(ActorStatus::GuardStand, 2, ActorStatus::Skill, 9)[0].kind == MeterKind::Neutral, "A guard held from a jump read as guarding");
            for (const auto& frame : resolved.View().frames)
                Require(frame.cells[0].kind == MeterKind::Neutral, "A guard from a jump turned to guarding once the jump left the history");
            Require(observe(ActorStatus::DamageGuard, 3, ActorStatus::Skill, 9)[0].kind == MeterKind::Guard, "A blocked hit did not read as guarding");
            resolved.Reset(); observed = 0;
            observe(ActorStatus::Stand, 1, ActorStatus::Skill, 9);
            Require(observe(ActorStatus::GuardStand, 2, ActorStatus::Skill, 9)[0].kind == MeterKind::Guard, "A grounded guard did not read as guarding");
            auto cells = observe(ActorStatus::DamageGuard, 3, ActorStatus::Skill, 9);
            Require(cells[0].kind == MeterKind::Guard && cells[0].newAction && !cells[0].split, "A blocked hit split the guard's run");
            resolved.Reset(); observed = 0;
            // A jab cancelled into a special: two counts. A hit and then the combo's next: two counts.
            observe(ActorStatus::Stand, 1, ActorStatus::Stand, 1);
            Require(resolved.View().frames.empty(), "An idle frame started the bars");
            cells = observe(ActorStatus::Skill, 100, ActorStatus::Damage, 300, 40);
            Require(!cells[0].split && !cells[0].newAction, "The first cell parted from the frame before it");
            cells = observe(ActorStatus::Skill, 100, ActorStatus::Damage, 300, 40, 2);
            Require(!cells[0].split && !cells[1].split, "A move or a hit split its own run");
            cells = observe(ActorStatus::Skill, 101, ActorStatus::Damage, 300, 90, 3);
            Require(cells[0].newAction && cells[0].split && cells[1].split && !cells[1].newAction, "A cancel or a combo's next hit did not start a count");
            // A meter cell tells an attack's startup, active and recovery frames apart by the script's boundary.
            FighterSample attack; attack.valid = true; attack.status = 16; attack.firstActiveFrame = 4; attack.lastActiveFrame = 7;
            const auto at = [&](float frame) { attack.actionFrame = frame; return ClassifyMeter(attack); };
            // The counter is read after the update: a script active from frame 4
            // has a startup of 4, three startup cells and its fourth frame active.
            Require(at(3) == MeterKind::Startup && at(4) == MeterKind::Active && at(6) == MeterKind::Active && at(7) == MeterKind::Recovery, "Attack frames misfiled");
            // A move of 4 startup, 3 active, 4 recovery frames, blocked on its first active frame with two frames of hitstop.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            const auto step = [&](int tick, unsigned status, float frame, unsigned other, bool stop = false) {
                for (auto& fighter : pair) { fighter = FighterSample{}; fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false; fighter.action = 0; }
                pair[0].status = status; pair[0].action = status == 16 ? 100 : 0; pair[0].actionFrame = frame; pair[0].firstActiveFrame = status == 16 ? 4 : -1; pair[0].lastActiveFrame = status == 16 ? 7 : -1;
                pair[1].status = other; pair[1].action = other ? 200 : 0;
                // Hitstop holds both: the one struck moves on with the attacker.
                pair[1].actionFrame = other ? frame : static_cast<float>(tick);
                pair[0].hitStop = pair[1].hitStop = stop;
                counted.Observe(tick, pair);
            };
            int tick = 0;
            step(tick++, 0, 0, 0);
            float last = 0;
            for (float frame : {1.f, 2.f, 3.f, 4.f, 4.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f}) step(tick, 16, frame, frame >= 4 ? 22 : 0, frame == last), ++tick, last = frame;
            Require(counted.View().startupFrames[0] == 4, "The startup frames before the first active one miscounted");
            Require(counted.View().moves[0].live && counted.View().moves[0].active == 3 && counted.View().moves[0].recovery == 4, "A live move's active or recovery cells miscounted, or hitstop counted");
            Require(counted.View().advantage.attacker == 0 && counted.View().advantage.blocked, "A blocked attack was not told from a hit");
            step(tick++, 0, 0, 22);
            Require(counted.View().moves[0].seen && !counted.View().moves[0].live && counted.View().moves[0].recovery == 4 && !counted.View().moves[1].seen, "A finished move lost its frames");
            // After a pause the next move's frames start counting at once, and
            // it starts the bars again from their left edge.
            for (int idle = 0; idle < 40; ++idle) step(tick++, 0, 0, 0);
            Require(counted.View().frozen, "The blocked move was not held");
            step(tick++, 16, 1, 0);
            Require(!counted.View().frozen && counted.View().frames.size() == 1 && counted.View().moves[0].active == 0 && counted.View().advantage.attacker == -1,
                "A new move after a pause kept the old bars or the old frames");
            step(tick++, 16, 2, 22);
            Require(!counted.View().frozen && counted.View().frames.size() == 2, "The new move's contact did not go on from it");
            // A projectile thrown after a held exchange: the throw starts the
            // bars, which are held once the thrower recovers; its startup and
            // its thrower's recovery stay kept through the idle frames it
            // travels, and its contact shows them.
            {
                FrameMeter thrown; std::array<FighterSample, 2> fireball; int at = 0;
                const auto frameOf = [&](unsigned own, unsigned other) {
                    for (int side = 0; side < 2; ++side) {
                        auto& fighter = fireball[side]; fighter = FighterSample{};
                        fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false;
                        fighter.status = side ? other : own; fighter.action = fighter.status ? 100 + side : 0; fighter.actionFrame = static_cast<float>(at);
                    }
                    thrown.Observe(at++, fireball);
                };
                frameOf(16, 0); frameOf(16, 22); frameOf(0, 22);
                for (int idle = 0; idle < 40; ++idle) frameOf(0, 0);
                Require(thrown.View().frozen, "The blocked exchange before the projectile was not held");
                const int thrownAt = at;
                for (int move = 0; move < 6; ++move) frameOf(16, 0);
                for (int travel = 0; travel < 20; ++travel) frameOf(0, 0);
                Require(thrown.View().frozen, "The projectile's flight took the held exchange away");
                frameOf(0, 21);
                Require(!thrown.View().frozen && thrown.View().frames.front().frame == thrownAt && thrown.View().frames.size() == 27 &&
                    thrown.View().frames.front().cells[0].phase == Phase::Attack, "The projectile's startup and recovery were lost before it landed");
            }
            // One whose flight outlasts the frames that hold the bars: from a
            // fresh meter, and soon after an exchange that was measured, whose
            // bars are held by then.
            for (const bool after : {false, true}) {
                FrameMeter live; std::array<FighterSample, 2> fireball; int at = 0;
                const auto frameOf = [&](unsigned own, unsigned other) {
                    for (int side = 0; side < 2; ++side) {
                        auto& fighter = fireball[side]; fighter = FighterSample{};
                        fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false;
                        fighter.status = side ? other : own; fighter.action = fighter.status ? 100 + side : 0; fighter.actionFrame = static_cast<float>(at);
                    }
                    live.Observe(at++, fireball);
                };
                frameOf(0, 0); frameOf(0, 0);
                if (after) {
                    frameOf(16, 0); frameOf(16, 0); frameOf(16, 22); frameOf(0, 22); frameOf(0, 0);
                    Require(live.View().advantage.valid, "The exchange before the projectile was not measured");
                    for (int idle = 0; idle < 5; ++idle) frameOf(0, 0);
                }
                Require(live.View().frozen == after, "The bars were held before the projectile, or the measured exchange was not");
                const int thrownAt = at;
                for (int move = 0; move < 6; ++move) frameOf(16, 0);
                for (int travel = 0; travel < 35; ++travel) frameOf(0, 0);
                Require(live.View().frozen, "Idle frames during the flight did not hold the bars");
                frameOf(0, 21);
                Require(!live.View().frozen && live.View().frames.front().frame == thrownAt && live.View().frames.size() == 42 &&
                    live.View().frames.front().cells[0].phase == Phase::Attack, "A projectile thrown while the bars ran lost its frames when they were held");
            }
            attack.lastActiveFrame = -1;
            Require(at(5) == MeterKind::Attack, "An attack with no boundary was split");
            attack.status = 22;
            Require(at(5) == MeterKind::Guard && ClassifyMeter(FighterSample{}) == MeterKind::Unknown, "A blocking or missing fighter misfiled");
            attack.status = 24;
            Require(at(5) == MeterKind::Sequence, "A throw's sequence was not told from an unknown state");
            attack.status = 19; Require(at(5) == MeterKind::Down, "Lying down misfiled");
            attack.status = 20; Require(at(5) == MeterKind::Rise, "Getting up was not told from lying down");
            // A throw: 3 startup frames, 5 frames holding the other, who is let go to fall while the thrower takes 4 more.
            counted.Reset(); tick = 0;
            step(tick++, 0, 0, 0);
            for (float frame : {1.f, 2.f, 3.f}) step(tick++, 16, frame, 0);
            for (int held = 0; held < 5; ++held) step(tick++, 24, 0, 24);
            Require(ClassifyMeter(counted.View().current[0]) == MeterKind::Sequence && counted.View().advantage.attacker == 0 && counted.View().advantage.pending, "A throw that connected was not a contact");
            for (int free = 0; free < 4; ++free) step(tick++, 24, 0, 19);
            Require(ClassifyMeter(counted.View().current[0]) == MeterKind::Recovery && ClassifyMeter(counted.View().current[1]) == MeterKind::Down &&
                counted.View().moves[0].live && counted.View().moves[0].recovery == 4, "The thrower's frames after the throw were not its recovery");
            // The thrower is free 6 frames before the other is up.
            for (int idle = 0; idle < 6; ++idle) step(tick++, 0, 0, 20);
            Require(!counted.View().moves[0].live && counted.View().moves[0].recovery == 4 && !counted.View().meatyValid[0], "A throw's recovery was lost, or a meaty read before any attack");
            step(tick++, 0, 0, 0);
            Require(counted.View().advantage.valid && counted.View().advantage.knockdown && counted.View().advantage.frames[0] == 6, "A throw's knockdown advantage miscounted");
            // Down again, and an attack that first is active in the frame before the other is seen up,
            // the frame it can be hit on: a meaty that meets it with its first active frame.
            for (int down = 0; down < 4; ++down) step(tick++, 0, 0, 19);
            for (float frame : {1.f, 2.f, 3.f, 4.f}) step(tick++, 16, frame, 20);
            step(tick++, 16, 5, 0);
            Require(counted.View().meatyValid[0] && counted.View().meatyFrames[0] == 0 && !counted.View().meatyValid[1], "Meaty timing miscounted");
            Require(counted.View().frames.back().fighters[1].wake && !counted.View().frames.back().fighters[0].wake && !counted.View().frames[counted.View().frames.size() - 2].fighters[1].wake, "The first frame up was not marked, or more than it");
        }
        {
            // Balrog's headbutt: six startup frames in one script, then a second script that
            // opens active. Its startup is seven, not one counted again from the second script.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
            pair[0].status = 16; pair[0].action = 346; pair[0].hits = bac::HitPhase::Before;
            for (int frame = 1; frame <= 6; ++frame) { pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[0].action = 369; pair[0].hits = bac::HitPhase::Out; pair[0].actionFrame = 0; counted.Observe(7, pair);
            pair[0].actionFrame = 1; counted.Observe(8, pair);
            Require(counted.View().startupFrames[0] == 7 && counted.View().moves[0].active == 2, "A move's active second script restarted its startup");
        }
        {
            // Vega's roll: the first script is active 3 frames and ends with one more, the second
            // (same command move) winds up and hits 1, then 4 frames recover. Its frame 0 is a
            // frame too, so 3(3)1: active 7.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
            int tick = 1;
            pair[0].status = 16; pair[0].action = 416; pair[0].commandMove = 0x1000;
            for (auto phase : {bac::HitPhase::Before, bac::HitPhase::Out, bac::HitPhase::Out, bac::HitPhase::Out, bac::HitPhase::Done}) { pair[0].hits = phase; pair[0].actionFrame = static_cast<float>(tick); counted.Observe(tick++, pair); }
            Require(counted.View().moves[0].active == 3 && counted.View().moves[0].recovery == 1, "The first script's hits or its tail miscounted");
            pair[0].action = 417;
            int frame = 0;
            for (auto phase : {bac::HitPhase::Before, bac::HitPhase::Before, bac::HitPhase::Out, bac::HitPhase::Done, bac::HitPhase::Done, bac::HitPhase::Done, bac::HitPhase::Done}) { pair[0].hits = phase; pair[0].actionFrame = static_cast<float>(frame++); counted.Observe(tick++, pair); }
            Require(counted.View().startupFrames[0] == 2 && counted.View().moves[0].active == 7 && counted.View().moves[0].recovery == 4,
                "A two-script move's gap or recovery miscounted");
        }
        {
            // Poison's 236LP: the fireball spawns at script frame 33 (boundary 34) while the
            // script plays 2 frames a step; the step from 32.99 to 34.99 spawns it, the next hits.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
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
        for (const bool input : {false, true}) {
            // Seth's 214LK plays two scripts, the second opening in startup, under one command
            // move: one move, startup counted from the first. A target combo is a new input
            // (another command move) and so a new move with its own startup.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
            pair[0].status = 16; pair[0].action = 450; pair[0].hits = bac::HitPhase::Before; pair[0].commandMove = 0x1000;
            // The target combo's first hit is out on frame 5; Seth's first script has none.
            for (int frame = 1; frame <= 5; ++frame) { pair[0].hits = input && frame == 5 ? bac::HitPhase::Out : bac::HitPhase::Before; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[0].action = 464; pair[0].commandMove = input ? 0x2000 : 0x1000;
            for (int frame = 0; frame < 7; ++frame) { pair[0].hits = frame < 6 ? bac::HitPhase::Before : bac::HitPhase::Out; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(6 + frame, pair); }
            Require(counted.View().startupFrames[0] == (input ? 7 : 12), input ? "A target combo's own startup was not counted" : "A move's own next script restarted its startup");
        }
        {
            // Ryu's c.LP chained into c.LP: one command move plays its script twice, 2 startup,
            // 2 active, then recovery. The second jab is a move of its own: its windup is no
            // recovery of the first, and its hits do not take the first's in.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
            int tick = 1;
            pair[0].status = 16; pair[0].action = 210; pair[0].commandMove = 0x5000;
            const auto jab = [&](int frames) {
                for (int frame = 1; frame <= frames; ++frame) {
                    pair[0].hits = frame < 3 ? bac::HitPhase::Before : frame < 5 ? bac::HitPhase::Out : bac::HitPhase::Done; pair[0].actionFrame = static_cast<float>(frame);
                    counted.Observe(tick++, pair);
                    if (frames > 6 && frame == 1)
                        Require(counted.View().startupFrames[0] < 0 && counted.View().moves[0].active == 0, "A chained jab kept the first jab's startup or active frames");
                }
            };
            jab(6);
            Require(counted.View().startupFrames[0] == 3 && counted.View().moves[0].active == 2 && counted.View().moves[0].recovery == 2, "The first jab miscounted");
            jab(7);
            Require(counted.View().startupFrames[0] == 3 && counted.View().moves[0].active == 2 && counted.View().moves[0].recovery == 3,
                "A normal chained into itself merged with the first into one move");
            int begun = 0;
            for (const auto& frame : counted.View().frames) begun += frame.cells[0].newAction && frame.cells[0].split;
            Require(begun == 1, "The bars did not start a count where the same normal played again");
        }
        {
            // A move held in its own hitstop (armour taking a hit) while the other fighter walks
            // on: the frames the game holds it are none of its startup, though the bars draw them.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
            int tick = 1;
            pair[0].status = 16; pair[0].action = 220; pair[1].status = ActorStatus::Forward; pair[1].action = 16;
            const auto step = [&](float frame, bac::HitPhase phase, bool held) {
                pair[0].actionFrame = frame; pair[0].hits = phase; pair[0].hitStop = held;
                pair[1].actionFrame = static_cast<float>(tick);
                counted.Observe(tick++, pair);
            };
            for (int frame = 1; frame <= 3; ++frame) step(static_cast<float>(frame), bac::HitPhase::Before, false);
            for (int held = 0; held < 4; ++held) step(3, bac::HitPhase::Before, true);
            step(4, bac::HitPhase::Before, false); step(5, bac::HitPhase::Out, false);
            Require(counted.View().startupFrames[0] == 5, "The other fighter's moving counted a held fighter's hitstop");
            Require(counted.View().frames.size() == 9, "Frames the other fighter moved on in were left out of the bars");
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
            FrameMeter counted; std::array<FighterSample, 2> pair;
            for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
            counted.Observe(0, pair);
            pair[0].status = 16; pair[0].action = 288;
            for (int frame = 1; frame <= 3; ++frame) { pair[0].hits = frame < 3 ? bac::HitPhase::Before : bac::HitPhase::Out; pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
            pair[0].action = 292; pair[0].hits = bac::HitPhase::Before; pair[1].status = 24; pair[1].action = 293;
            for (int frame = 0; frame < 5; ++frame) { pair[0].actionFrame = static_cast<float>(frame); pair[1].actionFrame = static_cast<float>(frame); counted.Observe(4 + frame, pair); }
            Require(counted.View().startupFrames[0] == 3 && counted.View().frames.back().cells[0].kind == MeterKind::Sequence,
                "A throw played in Skill restarted the thrower's startup, or was not drawn as its sequence");
            pair[0].hits = bac::HitPhase::Done; pair[1].status = 20; pair[1].action = 34;
            for (int frame = 5; frame < 8; ++frame) { pair[0].actionFrame = static_cast<float>(frame); pair[1].actionFrame = static_cast<float>(frame); counted.Observe(4 + frame, pair); }
            Require(counted.View().startupFrames[0] == 3 && counted.View().moves[0].recovery == 3, "A thrower's frames after the throw were not its recovery");
        }
        {
            // A move's recovery is its recovery cells: first=3, last=5 is 2
            // startup, 2 active and from frame 5 recovery cells. Ending it,
            // in a guard, a hit, blockstun or standing, adds none.
            for (unsigned finish : {0u, 3u, 14u, 15u, 21u, 22u}) for (int cells : {0, 2}) {
                FrameMeter counted; std::array<FighterSample, 2> pair;
                for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
                counted.Observe(0, pair);
                pair[0].status = 16; pair[0].action = 100; pair[0].firstActiveFrame = 3; pair[0].lastActiveFrame = 5;
                for (int frame = 1; frame <= 5; ++frame) { pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
                Require(counted.View().startupFrames[0] == 3 && counted.View().moves[0].active == 2 &&
                    counted.View().moves[0].live && counted.View().moves[0].recovery == 1, "A move's first recovery cell miscounted");
                for (int cell = 1; cell <= cells; ++cell) {
                    pair[0].actionFrame = static_cast<float>(5 + cell); counted.Observe(5 + cell, pair);
                }
                Require(counted.View().moves[0].recovery == 1 + cells, "Live recovery cells miscounted");
                // The script holds its last frame: in hitstop that is no cell; otherwise (Adon's
                // crouching MK) it is one more.
                pair[0].hitStop = pair[1].hitStop = true; counted.Observe(6 + cells, pair);
                pair[0].hitStop = pair[1].hitStop = false;
                Require(counted.View().moves[0].recovery == 1 + cells, "A frame left out of the bars was counted as recovery");
                pair[1].actionFrame = 1; counted.Observe(7 + cells, pair);
                Require(counted.View().moves[0].recovery == 2 + cells, "A held last frame the bars draw was not counted as recovery");
                // Both still outside hitstop (Seth's 623HP landing while Ryu lies down) is a frame.
                counted.Observe(8 + cells, pair);
                Require(counted.View().moves[0].recovery == 3 + cells, "A frame both fighters held outside hitstop was not counted");
                // So is one where only the other is in hitstop (Cody's EX Bad Stone: the rock's hit).
                pair[1].hitStop = true; counted.Observe(9 + cells, pair); pair[1].hitStop = false;
                const int recovery = 4 + cells;
                Require(counted.View().moves[0].recovery == recovery, "The other fighter's hitstop held the mover's frame");
                pair[0].status = finish; pair[0].action = 0; pair[0].actionFrame = 0;
                counted.Observe(10 + cells, pair);
                Require(!counted.View().moves[0].live && counted.View().moves[0].recovery == recovery, "Ending a move changed its recovery");
                counted.Observe(11 + cells, pair);
                pair[0].status = 0; counted.Observe(12 + cells, pair);
                Require(counted.View().moves[0].recovery == recovery, "A later recovered state changed completed recovery");
            }
        }
        std::puts("Frame meter checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
