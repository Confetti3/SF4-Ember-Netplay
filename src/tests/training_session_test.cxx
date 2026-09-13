#include "../training/TrainingSession.hxx"
#include <cstdio>
#include <stdexcept>
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
int main() {
    try {
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
        move.status = 16; move.action = 100; move.firstActiveFrame = 9;
        move.actionFrame = 3; startup.Observe(1, startupSamples);
        move.actionFrame = 6; startup.Observe(2, startupSamples);
        startup.Observe(3, startupSamples); // A frozen animation frame.
        move.actionFrame = 9; move.timeScale = 0; startup.Observe(4, startupSamples);
        Require(startup.View().startupFrames[0] == 3, "Startup counted BAC ticks or freeze instead of advancing frames");
        move.action = 101; move.actionFrame = 1; move.firstActiveFrame = 2; move.timeScale = 1;
        startup.Observe(5, startupSamples); move.actionFrame = 2; startup.Observe(6, startupSamples);
        Require(startup.View().startupFrames[0] == 2, "Target combo follow-up did not update startup");
        move.action = 102; move.actionFrame = 0; move.firstActiveFrame = -1;
        startup.Observe(7, startupSamples);
        Require(startup.View().startupFrames[0] == 2, "Recovery-only special action discarded startup");
        move.status = 0; startup.Observe(8, startupSamples);
        move.status = 16; move.action = 103; move.firstActiveFrame = -1; move.actionFrame = 1;
        startup.Observe(9, startupSamples); move.actionFrame = 2; startup.Observe(10, startupSamples);
        Require(startup.View().startupFrames[0] == -1, "Missing attack boundary fabricated startup");
        move.action = 104; move.firstActiveFrame = 1; move.actionFrame = 1; startup.Observe(11, startupSamples);
        Require(startup.View().startupFrames[0] == 3, "Multi-phase startup discarded its initial phase");
        startup.Reset();
        Require(startup.View().startupFrames[0] == -1, "Reset retained startup");

        Session session;
        Require(!session.Apply({Action::Record, 0, 0}), "Inactive session accepted recording");
        session.Enter(); session.SetReady(true);
        const auto generation = session.GetView().generation;
        auto apply = [&](Action a, int value = 0) { return session.Apply({a, value, generation}); };
        Require(!apply(Action::Select, -1) && !apply(Action::Select, SlotCount), "Invalid slot accepted");
        Require(!apply(Action::Play), "Empty playback accepted");
        Require(!apply(Action::Restore), "Missing checkpoint restored");
        Require(apply(Action::Record), "Record rejected");
        Frame physical{{Input{0x10, 0x10}, Input{0x80, 0x80}}};
        const auto output = session.Prepare(physical);
        Require(output[0].raw == 0 && output[1].raw == 0x10, "P1 did not control dummy");
        for (int i = 0; i < 5; ++i) session.Prepare(physical);
        Require(session.GetView().lengths[0] == 0, "Repeated or paused reads advanced recording");
        session.Commit(output);
        physical[0] = {0x20, 0x20}; session.Commit(session.Prepare(physical));
        Require(apply(Action::Stop) && apply(Action::Loop, 0) && apply(Action::Play), "Playback start failed");
        Require(session.Prepare(physical)[1].raw == 0x10, "First recorded frame skipped");
        session.Commit(session.Prepare(physical));
        Require(session.Prepare(physical)[1].raw == 0x20, "Playback order changed");
        session.Commit(session.Prepare(physical));
        Require(session.GetView().mode == Mode::Idle, "Single playback failed to stop");
        Require(apply(Action::Loop, 1) && apply(Action::Play), "Loop rejected");
        for (int i = 0; i < 10; ++i) session.Commit(session.Prepare(physical));
        Require(session.GetView().cursor == 0 && session.GetView().mode == Mode::Playback, "Loop boundary failed");
        Require(!apply(Action::Select, 1) && !apply(Action::Clear), "Slot mutated during playback");
        apply(Action::Stop); apply(Action::Select, 1); apply(Action::Record);
        for (int i = 0; i < MaxFrames + 5; ++i) session.Commit(session.Prepare(physical));
        Require(session.GetView().lengths[1] == MaxFrames && session.GetView().mode == Mode::Idle, "Recording limit failed");
        Require(session.GetView().lengths[0] == 2, "Second slot overwrote first");
        Require(session.GetView().timeline[0].size() <= 120 && session.GetView().history[0].size() <= HistoryRows, "History unbounded");
        session.SetReady(false); Require(!apply(Action::Record), "Loading allowed record");
        session.SetReady(true); session.SetCheckpoint(true);
        Require(apply(Action::Restore) && session.GetView().history[0].empty(), "Reset kept stale history");
        session.Reset(); session.Enter(); session.SetReady(true);
        Require(!apply(Action::Record) && !session.GetView().checkpoint && session.GetView().lengths[0] == 0, "Battle generation isolation failed");

        FrameMeter meter;
        std::array<FighterSample, 2> fighters;
        fighters[0].valid = fighters[1].valid = true;
        fighters[0].status = 16; fighters[1].status = 22;
        fighters[0].action = 100; fighters[1].action = 200;
        for (int i = 0; i < 5; ++i) meter.Observe(i, fighters);
        Require(meter.View().stateFrames[0] == 5, "State duration wrong");
        fighters[0].action = 101; meter.Observe(5, fighters);
        Require(meter.View().lastAttackFrames[0] == 5 && meter.View().actionFrames[0] == 1, "Cancelled action duration wrong");
        Require(ClassifyStatus(16) == Phase::Attack && ClassifyStatus(22) == Phase::Guard && ClassifyStatus(99) == Phase::Unknown, "Native status classification wrong");
        fighters[0].status = fighters[1].status = 0;
        for (int i = 6; i < 50; ++i) meter.Observe(i, fighters);
        Require(meter.View().frozen && meter.View().frames.back().frame == 34, "Idle did not hold exchange");
        fighters[0].status = 16; meter.Observe(50, fighters);
        Require(!meter.View().frozen && meter.View().frames.size() == 1, "Next exchange did not resume");
        for (int i = 51; i < 200; ++i) meter.Observe(i, fighters);
        Require(meter.View().frames.size() == 120, "Frame meter unbounded");
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
        std::puts("Training session and frame meter checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
