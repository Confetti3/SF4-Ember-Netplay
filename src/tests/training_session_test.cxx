#include "../training/TrainingSession.hxx"
#include "../training/ConfirmedSamples.hxx"
#include "../training/MatchPractice.hxx"
#include <cstdio>
#include <stdexcept>
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
int main() {
    try {
        MeterView longHistory;
        for (int frame = 0; frame < static_cast<int>(MeterHistory); ++frame) longHistory.frames.push_back({{}, frame});
        for (std::size_t cell = 0; cell < MeterShown; ++cell) {
            const int index = MeterFrameIndex(longHistory.frames.size(), cell);
            Require(index == 1080 + static_cast<int>(cell) && longHistory.frames[index].frame == index,
                "Meter did not draw the newest 120 of 1200 frames");
        }
        Require(MeterFrameIndex(0, 119) == -1 && MeterFrameIndex(3, 116) == -1 &&
            MeterFrameIndex(3, 117) == 0 && MeterFrameIndex(3, 119) == 2 && MeterFrameIndex(1200, 120) == -1,
            "Short meter history or visible cell boundary wrong");
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
        Require(startup.View().startupUnavailable[0] == MeasurementUnavailable::NoAttackBoundary,
            "Missing attack boundary had no explanation");
        move.action = 104; move.firstActiveFrame = 1; move.actionFrame = 1;
        move.boundaryProvenance = BoundaryProvenance::BacActionHeader; startup.Observe(11, startupSamples);
        Require(startup.View().startupFrames[0] == 3, "Multi-phase startup discarded its initial phase");
        Require(startup.View().startupBoundaryProvenance[0] == BoundaryProvenance::BacActionHeader,
            "Multi-phase startup retained provenance from the earlier boundary-less action");

        // DP -> focus cancel -> dash: an authored DP startup remains useful,
        // while advantage completes only once both fighters are actionable.
        FrameMeter fadc;
        std::array<FighterSample, 2> fadcSamples;
        for (auto& sample : fadcSamples) {
            sample.valid=true; sample.status=0; sample.action=0; sample.posture=0;
            sample.timeScale=1; sample.basicActionInhibited=false;
        }
        fadc.Observe(0,fadcSamples);
        fadcSamples[0].status=16; fadcSamples[0].action=300; fadcSamples[0].firstActiveFrame=4;
        for(int frame=1;frame<=4;++frame){fadcSamples[0].actionFrame=(float)frame;fadc.Observe(frame,fadcSamples);}
        Require(fadc.View().startupFrames[0]==4,"DP startup was not measured");
        fadcSamples[1].status=22;fadcSamples[1].action=400;fadcSamples[1].actionFrame=1;fadc.Observe(5,fadcSamples);
        fadcSamples[0].action=301;fadcSamples[0].actionFrame=1;fadcSamples[0].firstActiveFrame=-1;fadc.Observe(6,fadcSamples);
        fadcSamples[0].status=5;fadcSamples[0].action=302;fadcSamples[0].actionFrame=1;fadc.Observe(7,fadcSamples);
        Require(fadc.View().startupFrames[0]==4,"FADC discarded completed DP startup");
        fadcSamples[1].status=0;fadcSamples[1].action=0;fadcSamples[1].actionFrame=0;fadc.Observe(8,fadcSamples);
        Require(fadc.View().advantage.pending,"FADC measured advantage before the dash completed");
        fadcSamples[0].status=0;fadcSamples[0].action=0;fadcSamples[0].actionFrame=0;fadc.Observe(9,fadcSamples);
        Require(fadc.View().advantage.valid && fadc.View().advantage.frames[0]==-1,
            "FADC advantage did not use the first actionable post-dash frame");
        startup.Reset();
        Require(startup.View().startupFrames[0] == -1, "Reset retained startup");

        Session session;
        Require(!session.Apply({Action::Record, 0, 0}), "Inactive session accepted recording");
        session.Enter(); session.SetReady(true);
        const auto generation = session.GetView().generation;
        auto apply = [&](Action a, int value = 0) { return session.Apply({a, value, generation}); };
        Require(!apply(Action::Select, -1) && !apply(Action::Select, SlotCount), "Invalid slot accepted");
        Require(!apply(Action::Play), "Empty playback accepted");
        // Loaded input plays on the side it names; a recording plays on the dummy again.
        {
            Command load; load.action = Action::Load; load.generation = session.GetView().generation; load.value = 0;
            load.frames = {Input{9, 9}, Input{0x410, 0x410}};
            Require(session.Apply(load) && session.GetView().lengths[session.GetView().selected] == 2, "Load refused");
            Require(apply(Action::Play), "Loaded playback refused");
            Frame physical{}; physical[0] = {1, 1}; physical[1] = {2, 2};
            auto frame = session.Prepare(physical);
            Require(frame[0].raw == 9 && frame[1].raw == 2, "Loaded input did not replace Player 1");
            session.Commit(frame); frame = session.Prepare(physical);
            Require(frame[0].raw == 0x410, "Loaded input did not advance");
            Command empty; empty.action = Action::Load; empty.generation = load.generation;
            Require(!session.Apply(empty), "Empty load accepted while playing");
            // Loaded input plays one pass even with looping on.
            Require(session.GetView().loop, "Loop is not the default");
            session.Commit(frame);
            Require(session.GetView().mode == Mode::Idle, "Loaded input looped");
            apply(Action::Stop);
            // A waiting frame repeats, buttons held, until the fight shows its
            // cue, then its offset's frames more; it gives up after a while.
            Command timed; timed.action = Action::Load; timed.generation = generation; timed.value = 0;
            timed.frames = {Input{9, 9, 0}, Input{2, 2, WaitActionable, 0}, Input{0x82, 0x82, 0}, Input{8, 8, WaitHit, 2}, Input{0x18, 0x18, 0}};
            Require(session.Apply(timed) && apply(Action::Play), "Timed load refused");
            session.Commit(session.Prepare(physical));
            for (int held = 0; held < 5; ++held) { session.Observe(false, false); session.Commit(session.Prepare(physical)); }
            Require(session.Prepare(physical)[0].raw == 2 && session.GetView().cursor == 1, "Waiting frame did not hold its direction");
            session.Observe(true, false); session.Commit(session.Prepare(physical));
            Require(session.Prepare(physical)[0].raw == 0x82, "Free frame did not release the wait");
            session.Commit(session.Prepare(physical));
            session.Observe(false, true); session.Commit(session.Prepare(physical));
            session.Observe(false, false); session.Commit(session.Prepare(physical));
            Require(session.GetView().cursor == 3, "Late offset did not hold after the hit");
            session.Observe(false, false); session.Commit(session.Prepare(physical));
            Require(session.Prepare(physical)[0].raw == 0x18, "Late offset held too long");
            apply(Action::Stop);
            // A press waiting for a hit that never comes goes stale quickly.
            Require(session.Apply(timed) && apply(Action::Play), "Timed load refused again");
            for (int i = 0; i < 3; ++i) { session.Observe(true, false); session.Commit(session.Prepare(physical)); }
            // The give-up spans the cue's frames plus the offset's.
            for (int i = 0; i < MaxWaitHitFrames + 2; ++i) { session.Observe(false, false); session.Commit(session.Prepare(physical)); }
            Require(session.Prepare(physical)[0].raw == 0x18, "Waiting for a hit never gave up");
            apply(Action::Stop);
            // With the script predicting the free frame, a link's press lands
            // on that frame plus the offset: before it when negative. A seen
            // free frame still releases it. A hit is never predicted.
            Command early; early.action = Action::Load; early.generation = generation; early.value = 0;
            early.frames = {Input{9, 9, 0}, Input{2, 2, WaitActionable, 0}, Input{0x82, 0x82, 0}, Input{8, 8, WaitActionable, -2}, Input{0x18, 0x18, 0}};
            Require(session.Apply(early) && apply(Action::Play), "Early load refused");
            session.Commit(session.Prepare(physical));
            session.Observe(false, false, 5); session.Commit(session.Prepare(physical));
            session.Observe(false, false, 2); session.Commit(session.Prepare(physical));
            Require(session.GetView().cursor == 1, "Press went before the free frame");
            session.Observe(false, false, 1); session.Commit(session.Prepare(physical));
            Require(session.Prepare(physical)[0].raw == 0x82 && session.GetView().cursor == 2, "Press did not land on the predicted free frame");
            session.Commit(session.Prepare(physical));
            session.Observe(false, false, 8); session.Commit(session.Prepare(physical));
            session.Observe(false, false, 4); session.Commit(session.Prepare(physical));
            Require(session.GetView().cursor == 3, "Early press went before its frames");
            session.Observe(false, false, 3); session.Commit(session.Prepare(physical));
            Require(session.GetView().cursor == 4, "Press did not land two frames before the free frame");
            apply(Action::Stop);
            Require(session.Apply(early) && apply(Action::Play), "Early load refused again");
            session.Commit(session.Prepare(physical));
            session.Observe(true, false); session.Commit(session.Prepare(physical));
            session.Commit(session.Prepare(physical));
            session.Observe(true, false, 7); session.Commit(session.Prepare(physical));
            Require(session.GetView().cursor == 4, "Seen free frame did not release an early press");
            apply(Action::Stop);
            // A hit landing while the motion before a cancel's wait is still
            // going is kept for that wait; the press that started the move clears it.
            Command kept; kept.action = Action::Load; kept.generation = generation; kept.value = 0;
            kept.frames = {Input{0x82, 0x82, 0}, Input{2, 2, 0}, Input{0xa, 0xa, 0}, Input{0xa, 0xa, WaitHit, 0}, Input{0x408, 0x408, 0}};
            Require(session.Apply(kept) && apply(Action::Play), "Kept load refused");
            session.Observe(false, true); session.Commit(session.Prepare(physical));
            session.Observe(false, false); session.Commit(session.Prepare(physical));
            session.Observe(false, true); session.Commit(session.Prepare(physical));
            session.Observe(false, false); session.Commit(session.Prepare(physical));
            Require(session.GetView().cursor == 4, "A hit during the motion was lost");
            apply(Action::Stop);
            Require(session.Apply(kept) && apply(Action::Play), "Kept load refused again");
            session.Observe(false, true); session.Commit(session.Prepare(physical));
            for (int i = 0; i < 3; ++i) { session.Observe(false, false); session.Commit(session.Prepare(physical)); }
            Require(session.GetView().cursor == 3, "A hit before the press counted for the move after it");
            apply(Action::Stop);
        }
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
        for (int i = 51; i < 151 + static_cast<int>(MeterHistory); ++i) meter.Observe(i, fighters);
        Require(meter.View().frames.size() == MeterHistory, "Frame meter unbounded");
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
        // Dummy settings: a request within the menu's choices shows in the
        // view until the adapter's read-back; -1 leaves a setting alone.
        {
            Session dummy; dummy.Enter();
            Command set; set.action = Action::DummyState; set.generation = dummy.GetView().generation;
            set.dummy.action = 1; set.dummy.guard = 2; set.dummy.counterHit = 1;
            Require(dummy.Apply(set), "Dummy settings refused");
            DummyState expected; expected.action = 1; expected.guard = 2; expected.counterHit = 1;
            Require(dummy.GetView().dummy == expected, "Dummy settings not shown");
            set.dummy = DummyState{}; set.dummy.stun = 2;
            Require(dummy.Apply(set) && dummy.GetView().dummy.action == 1 && dummy.GetView().dummy.stun == 2, "Unchanged dummy setting was cleared");
            set.dummy = DummyState{}; set.dummy.action = 4;
            Require(!dummy.Apply(set), "Menu recorder value accepted as a dummy action");
            set.dummy = DummyState{}; set.dummy.counterHit = 3;
            Require(!dummy.Apply(set) && !ValidDummyState(set.dummy), "Counter hit beyond the menu's choices accepted");
        }
        {
            // A meter cell tells an attack's startup, active and recovery frames apart by the script's boundary.
            FighterSample attack; attack.valid = true; attack.status = 16; attack.firstActiveFrame = 4; attack.lastActiveFrame = 7;
            const auto at = [&](float frame) { attack.actionFrame = frame; return ClassifyMeter(attack); };
            // The counter is read after the update: a script active from frame 4 has four startup cells.
            Require(at(4) == MeterKind::Startup && at(5) == MeterKind::Active && at(7) == MeterKind::Active && at(8) == MeterKind::Recovery, "Attack frames misfiled");
            // A move of 4 startup, 3 active, 4 recovery frames, blocked on its first active frame with two frames of hitstop.
            FrameMeter counted; std::array<FighterSample, 2> pair;
            const auto step = [&](int tick, unsigned status, float frame, unsigned other) {
                for (auto& fighter : pair) { fighter = FighterSample{}; fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.basicActionInhibited = false; fighter.action = 0; }
                pair[0].status = status; pair[0].action = status == 16 ? 100 : 0; pair[0].actionFrame = frame; pair[0].firstActiveFrame = status == 16 ? 4 : -1; pair[0].lastActiveFrame = status == 16 ? 7 : -1;
                pair[1].status = other; pair[1].action = other ? 200 : 0; pair[1].actionFrame = static_cast<float>(tick);
                counted.Observe(tick, pair);
            };
            int tick = 0;
            step(tick++, 0, 0, 0);
            for (float frame : {1.f, 2.f, 3.f, 4.f, 5.f, 5.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f}) step(tick, 16, frame, frame >= 5 ? 22 : 0), ++tick;
            Require(counted.View().startupFrames[0] == 4, "The startup frames before the first active one miscounted");
            Require(counted.View().moves[0].live && counted.View().moves[0].active == 3 && counted.View().moves[0].recovery == 3, "A live move's active or recovery cells miscounted, or hitstop counted");
            Require(counted.View().advantage.attacker == 0 && counted.View().advantage.blocked, "A blocked attack was not told from a hit");
            step(tick++, 0, 0, 22);
            Require(counted.View().moves[0].seen && !counted.View().moves[0].live && counted.View().moves[0].recovery == 4 && !counted.View().moves[1].seen, "A finished move lost its frames");
            // After a pause the next move starts the bars from their left edge.
            for (int idle = 0; idle < 40; ++idle) step(tick++, 0, 0, 0);
            step(tick++, 16, 1, 0);
            Require(counted.View().frames.size() == 1 && counted.View().moves[0].active == 0 && counted.View().advantage.attacker == -1, "A new move after a pause kept the old bars or frames");
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
            for (int down = 0; down < 3; ++down) step(tick++, 0, 0, 19);
            for (float frame : {1.f, 2.f, 3.f, 4.f, 5.f}) step(tick++, 16, frame, 20);
            step(tick++, 16, 6, 0);
            Require(counted.View().meatyValid[0] && counted.View().meatyFrames[0] == 0 && !counted.View().meatyValid[1], "Meaty timing miscounted");
            Require(counted.View().frames.back().fighters[1].wake && !counted.View().frames.back().fighters[0].wake && !counted.View().frames[counted.View().frames.size() - 2].fighters[1].wake, "The first frame up was not marked, or more than it");
        }
        {
            // The omitted recovery frame belongs to completion, even when
            // first=3, last=5 and frames 1..5 contain no Recovery cell.
            // Guard posture completes a move; hit and blockstun interrupt it.
            for (unsigned finish : {0u, 3u, 14u, 15u, 21u, 22u}) for (int cells : {0, 2}) {
                FrameMeter counted; std::array<FighterSample, 2> pair;
                for (auto& fighter : pair) { fighter.valid = true; fighter.posture = 0; fighter.timeScale = 1; fighter.status = 0; fighter.action = 0; }
                counted.Observe(0, pair);
                pair[0].status = 16; pair[0].action = 100; pair[0].firstActiveFrame = 3; pair[0].lastActiveFrame = 5;
                for (int frame = 1; frame <= 5; ++frame) { pair[0].actionFrame = static_cast<float>(frame); counted.Observe(frame, pair); }
                Require(counted.View().startupFrames[0] == 3 && counted.View().moves[0].active == 2 &&
                    counted.View().moves[0].live && counted.View().moves[0].recovery == 0, "Zero-cell recovery fixture miscounted");
                for (const auto& frame : counted.View().frames) Require(ClassifyMeter(frame.fighters[0]) != MeterKind::Recovery, "Zero-cell fixture contains a recovery cell");
                for (int cell = 1; cell <= cells; ++cell) {
                    pair[0].actionFrame = static_cast<float>(5 + cell); counted.Observe(5 + cell, pair);
                }
                Require(counted.View().moves[0].recovery == cells, "Live recovery cells miscounted");
                pair[0].status = finish; pair[0].action = 0; pair[0].actionFrame = 0;
                counted.Observe(6 + cells, pair);
                const int recovery = cells + (finish == 21 || finish == 22 ? 0 : 1);
                Require(!counted.View().moves[0].live && counted.View().moves[0].recovery == recovery,
                    "Completion omitted or repeated the one recovery frame, or counted interruption as completion");
                counted.Observe(7 + cells, pair);
                pair[0].status = 0; counted.Observe(8 + cells, pair);
                Require(counted.View().moves[0].recovery == recovery, "A later recovered state changed completed recovery");
            }
        }
        {
            // Runtime ownership: edits and stance changes during a reply
            // affect the logical action, while native pad playback stays Stand.
            for (int finish = 0; finish < 3; ++finish) {
                Session reply; reply.Enter(); reply.SetReady(true);
                DummyAction action; int native = 1;
                Require(reply.Reply(std::vector<Input>{{0x10, 0x10, 0, 0}}), "Action ownership reply did not start");
                action.BeginReply(native);
                Require(native == 0 && action.Read(native) == 1, "Reply exposed the temporary Stand action");
                Command edit; edit.action = Action::DummyState; edit.generation = reply.GetView().generation; edit.dummy.action = 2;
                Require(reply.Apply(edit) && reply.Replying(), "Dummy action edit stopped the reply");
                action.Set(native, edit.dummy.action); action.BeginReply(native);
                Require(native == 0 && action.Read(native) == 2, "Reply edit touched playback or repeated override lost the setting");
                if (finish == 0) reply.Commit(reply.Prepare(Frame{}));
                else if (finish == 1) reply.StopReply();
                else {
                    Command play; play.action = Action::Play; play.generation = reply.GetView().generation;
                    Require(!reply.Apply(play), "An empty restored slot played");
                }
                Require(!reply.Replying(), "Action ownership reply did not end");
                action.EndReply(native); action.EndReply(native);
                Require(native == 2 && action.Read(native) == 2, "Reply completion discarded the edited action");
                native = 3; Require(action.Read(native) == 3, "Idle action ignored the game's own setting");
                action.Set(native, 1); action.BeginReply(native); action.Set(native, 0);
                Require(native == 0 && action.Read(native) == 0, "Vary stance did not update the logical action");
                action.EndReply(native); Require(native == 0, "Vary stance was not restored");
                Require(reply.Reply(std::vector<Input>{{0x10, 0x10, 0, 0}}), "Battle exit reply did not start");
                action.Set(native, 1); action.BeginReply(native); action.Set(native, 3);
                reply.Reset(); action.EndReply(native);
                Require(native == 3, "Battle exit discarded the edited action");
            }
        }
        {
            // The dummy's own reply: what it came out of, how long it stays there, and what it plays.
            DummyWatch watch;
            const auto sample = [](unsigned status, int action, float frame, float damage = 0) {
                FighterSample dummy; dummy.valid = true; dummy.status = status; dummy.action = action; dummy.actionFrame = frame; dummy.comboDamage = damage; return dummy;
            };
            // Hit by move 100 for five frames: nothing is known the first time.
            Require(!watch.Observe(sample(0, 0, 0), 100).freed, "An idle dummy was freed");
            for (int frame = 0; frame < 5; ++frame) {
                const auto seen = watch.Observe(sample(21, 300, static_cast<float>(frame), 10), 100);
                Require(seen.held == 1 && seen.until == -1 && !seen.freed, "An unseen stun was timed");
            }
            const auto first = watch.Observe(sample(0, 0, 0), 0);
            Require(first.freed == 1 && !watch.Observe(sample(0, 0, 1), 0).freed, "A hit's end was not reported once");
            // The same hit again counts down to its free frame, and a second hit in it starts over.
            auto seen = watch.Observe(sample(21, 300, 0, 10), 100);
            Require(seen.until == 5 && seen.stretch != first.stretch, "A seen stun was not timed");
            Require(watch.Observe(sample(21, 300, 1, 10), 100).until == 4, "The stun did not count down");
            seen = watch.Observe(sample(21, 300, 2, 20), 100);
            Require(seen.until == 5, "A second hit did not start the stun over");
            // A reply whose button is three frames in starts when four are left, one fewer with timing +1, and at once when freed.
            DummySeen left; left.held = 1; left.until = 5;
            Require(!ReplyDue(left, 3, 0), "The reply started too early");
            left.until = 4;
            Require(ReplyDue(left, 3, 0) && !ReplyDue(left, 3, 1) && ReplyDue(first, 0, 0), "The reply was not due on its frame");
            left.until = -1;
            Require(!ReplyDue(left, 3, 0), "A reply was due in an untimed stun");
            // Another move's hit is its own length; a knockdown is one whoever caused it.
            Require(watch.Observe(sample(21, 300, 0, 30), 101).until == -1, "Another move's stun was taken as known");
            watch.Reset();
            watch.Observe(sample(19, 400, 0), 100); watch.Observe(sample(20, 401, 0), 100); watch.Observe(sample(20, 401, 1), 100);
            Require(watch.Observe(sample(1, 0, 0), 0).freed == 3, "A knockdown was not reported as getting up");
            watch.Observe(sample(19, 400, 0), 555);
            Require(watch.Observe(sample(20, 401, 0), 777).until == 2, "Getting up was not timed across attackers");
            watch.Reset();
            watch.Observe(sample(22, 500, 0), 100);
            Require(watch.Observe(sample(0, 0, 0), 0).freed == 2, "A block's end was not reported");
            DummyPlan plan;
            Require(!DummyReplies(plan, 1, 0), "A dummy with no plan replied");
            plan.when = 1; plan.chance = 50;
            Require(DummyReplies(plan, 1, 49) && !DummyReplies(plan, 1, 50) && !DummyReplies(plan, 2, 0) && !DummyReplies(plan, 0, 0), "Reply chance or cause misjudged");
            plan.when = 4;
            Require(DummyReplies(plan, 3, 0) && ValidDummyPlan(plan), "A reply to anything missed a knockdown");
            plan.timing = MaxReplyTiming + 1;
            Require(!ValidDummyPlan(plan), "A reply timing beyond its range was accepted");
            plan.timing = 0; plan.slot = SlotCount;
            Require(!ValidDummyPlan(plan), "A reply slot beyond the slots was accepted");
            // 623HP: three directions of three frames, then the button.
            std::vector<Input> dragon;
            for (unsigned bits : {8u, 8u, 8u, 2u, 2u, 2u, 10u, 10u, 10u, 0x40au, 10u, 0u}) dragon.push_back({bits, bits, 0, 0});
            Require(ReplyStart(dragon) == 0 && ReplyLead(dragon) == 9 && ReplyStart({}) == -1 && ReplyLead({{8, 8, 0, 0}}) == 0, "A reply's start or lead misread");

            Session reply; reply.Enter(); reply.SetReady(true);
            Command command; command.generation = reply.GetView().generation;
            Require(!reply.Reply(2), "An empty slot replied");
            command.action = Action::Select; command.value = 2; reply.Apply(command);
            command.action = Action::Record; reply.Apply(command);
            // Two idle frames, as a player leaves before pressing, then two presses.
            for (unsigned buttons : {0u, 0u, 0x10u, 0x20u}) { Frame frame; frame[1].raw = frame[1].mapped = buttons; reply.Commit(frame); }
            command.action = Action::Stop; reply.Apply(command);
            command.action = Action::Select; command.value = 5; reply.Apply(command);
            Require(reply.Reply(2) && reply.GetView().mode == Mode::Playback && reply.GetView().selected == 2, "The reply did not start");
            Require(!reply.Reply(2), "A reply started over a playback");
            Require(reply.Prepare(Frame{})[1].raw == 0x10 && reply.Prepare(Frame{})[0].raw == 0, "The reply did not start on its first press, on Player 2");
            reply.Commit(reply.Prepare(Frame{}));
            Require(reply.Prepare(Frame{})[1].raw == 0x20, "The reply did not advance");
            reply.Commit(reply.Prepare(Frame{}));
            Require(reply.GetView().mode == Mode::Idle && reply.GetView().selected == 5, "The reply looped or kept the selection");
            // The typed reply has a slot of its own; hit again, it is dropped.
            Require(reply.Reply(dragon) && reply.Replying() && reply.GetView().selected == ReplySlot && reply.Prepare(Frame{})[1].raw == 8, "The typed reply did not start");
            reply.StopReply();
            Require(!reply.Replying() && reply.GetView().mode == Mode::Idle && reply.GetView().selected == 5 && reply.GetView().lengths[2] == 4, "A dropped reply kept the selection or touched a slot");
            command.action = Action::Select; command.value = ReplySlot;
            Require(!reply.Apply(command), "The reply's own slot could be selected");
            // Play stops a reply before checking the restored selection.
            for (bool typed : {false, true}) {
                Require(typed ? reply.Reply(dragon) : reply.Reply(2), "The reply did not start over an empty selection");
                command.action = Action::Play;
                Require(!reply.Apply(command) && !reply.Replying() && reply.GetView().mode == Mode::Idle &&
                    reply.GetView().selected == 5 && reply.GetView().cursor == 0, "Play started on an empty restored slot");
                Frame physical; physical[0] = {0x10, 0x10, 0, 0}; physical[1] = {0x20, 0x20, 0, 0};
                const auto output = reply.Prepare(physical);
                Require(output[0].raw == 0x10 && output[1].raw == 0x20, "A refused playback changed physical input");
            }
            command.action = Action::Select; command.value = 2;
            Require(reply.Apply(command), "The recorded slot could not be selected");
            command.action = Action::Load; command.value = 0; command.frames = {{0x40, 0x40, 0, 0}};
            Require(reply.Apply(command) && reply.Reply(dragon), "The reply did not start over a loaded selection");
            command.action = Action::Play;
            Require(reply.Apply(command) && !reply.Replying() && reply.GetView().mode == Mode::Playback &&
                reply.GetView().selected == 2 && reply.GetView().playbackSide == 0 && reply.GetView().cursor == 0,
                "Play did not restart the restored slot and side");
            Require(reply.Prepare(Frame{})[0].raw == 0x40 && reply.Prepare(Frame{})[1].raw == 0,
                "Play used the reply instead of the restored recording");
        }
        {
            // A rollback match: every played frame is captured, a replayed one over its first capture,
            // and a frame is given out once, in order, when its inputs are confirmed.
            ConfirmedSamples kept;
            std::array<FighterSample, 2> seen, out;
            const auto capture = [&](int frame, unsigned status) { seen[0].status = status; seen[0].valid = true; kept.Capture(frame, seen); };
            int frame = 0;
            Require(!kept.Next(100, frame, out), "A frame was given before any was captured");
            capture(1, 0); capture(2, 0); capture(3, 16);
            Require(!kept.Next(-1, frame, out), "A frame was given with no input confirmed");
            // Save frame N holds input N - 1: with input 0 confirmed only frame 1 is.
            Require(kept.Next(0, frame, out) && frame == 1 && !kept.Next(0, frame, out), "Frames were given past the confirmed input");
            // Frame 3 was a prediction: it is played again as another frame before it is confirmed.
            capture(3, 0); capture(4, 0);
            Require(kept.Next(2, frame, out) && frame == 2 && kept.Next(2, frame, out) && frame == 3 && out[0].status == 0 && !kept.Next(2, frame, out),
                "A predicted frame was given, or the replayed one was not");
            // Frames nobody captured: it goes on from the oldest one held.
            capture(10, 16);
            Require(kept.Next(50, frame, out) && frame == 4 && kept.Next(50, frame, out) && frame == 10 && out[0].status == 16 && !kept.Next(50, frame, out),
                "A hole in the captured frames stopped the meter");
            // A long match wraps the slots many times over.
            for (int at = 11; at < 11 + 5 * ConfirmedSamples::Capacity; ++at) {
                capture(at, static_cast<unsigned>(at % 7));
                Require(kept.Next(at - 1, frame, out) && frame == at && out[0].status == static_cast<unsigned>(at % 7), "A frame was lost as the slots wrapped");
            }
            // Unconfirmed for longer than the slots hold: the overwritten frames are skipped, none is given twice.
            for (int at = 400; at < 400 + 2 * ConfirmedSamples::Capacity; ++at) capture(at, 1);
            int given = 0, last = 0;
            while (kept.Next(1000, frame, out)) { Require(frame > last, "Frames were given out of order"); last = frame; ++given; }
            Require(given == ConfirmedSamples::Capacity && last == 399 + 2 * ConfirmedSamples::Capacity, "Overwritten frames were given");
            // The next match counts from one again.
            capture(1, 21);
            Require(kept.Next(0, frame, out) && frame == 1 && out[0].status == 21 && !kept.Next(900, frame, out), "A new match did not start the frames anew");
        }
        {
            // A Training table's shared save and reset, decided from a frame's inputs and the state that rolls back with it.
            PracticeState state;
            const unsigned reset = PracticeReset, save = PracticeSave;
            // Nothing before the fight, whatever is pressed; the press is remembered as held.
            Require(DecidePractice(state, reset | save, 0, false) == PracticeStep::None && state.count == 0, "A step was taken before the fight");
            // The fight begins: the position is saved by nobody, so a reset has one.
            Require(DecidePractice(state, reset | save, 0, true) == PracticeStep::Save && state.by == -1 && state.count == 1, "The round's start was not saved");
            // Still held from before: no press.
            Require(DecidePractice(state, reset | save, 0, true) == PracticeStep::None, "A held button counted as a press");
            Require(DecidePractice(state, 0, 0, true) == PracticeStep::None, "A release counted as a press");
            // A press counts once, as it goes down, and says whose it is.
            Require(DecidePractice(state, 0, reset, true) == PracticeStep::Reset && state.by == 1 && state.last == PracticeStep::Reset && state.count == 2, "Player 2's reset was not taken");
            Require(DecidePractice(state, 0, reset, true) == PracticeStep::None && state.count == 2, "A reset repeated while held");
            Require(DecidePractice(state, save, reset, true) == PracticeStep::Save && state.by == 0 && state.count == 3, "Player 1's save was not taken");
            // Both in one frame is a save, and Player 1's before Player 2's.
            (void)DecidePractice(state, 0, 0, true);
            Require(DecidePractice(state, reset, save, true) == PracticeStep::Save && state.by == 1, "A save and a reset in one frame was not a save");
            (void)DecidePractice(state, 0, 0, true);
            Require(DecidePractice(state, save, save, true) == PracticeStep::Save && state.by == 0, "Two saves in one frame were not Player 1's");
            // Other bits of the pad are not its business.
            (void)DecidePractice(state, 0, 0, true);
            Require(DecidePractice(state, 0x10 | 0x400 | 0x8, 0xFFFF, true) == PracticeStep::None, "A fight button was taken for a step");
            // A predicted frame repeats the last input, and a rollback brings the state back: the frame decides as it first did.
            PracticeState before = state;
            Require(DecidePractice(state, reset, 0, true) == PracticeStep::Reset, "A reset was not taken");
            const PracticeState after = state;
            Require(DecidePractice(state, reset, 0, true) == PracticeStep::None && DecidePractice(state, reset, 0, true) == PracticeStep::None, "A predicted repeat made a press");
            state = before;
            Require(DecidePractice(state, reset, 0, true) == PracticeStep::Reset && state.count == after.count && state.held == after.held, "A replayed frame decided otherwise");
            // The fight over and begun again saves again.
            Require(DecidePractice(state, 0, 0, false) == PracticeStep::None && DecidePractice(state, 0, 0, true) == PracticeStep::Save && state.by == -1, "A new fight did not save its start");
        }
        std::puts("Training session and frame meter checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
