#include "../training/TrainingSession.hxx"
#include "../training/ConfirmedSamples.hxx"
#include <cstdio>
#include <stdexcept>
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
int main() {
    try {
        Session session;
        Require(!session.Apply({Action::AdvanceRecording, 0}), "Inactive session accepted recording");
        session.Enter(); session.SetReady(true);
        const auto generation = session.GetView().generation;
        auto apply = [&](Action a) { return session.Apply({a, generation}); };
        auto select = [&](int slot) { Command c{Action::Select, generation}; c.slot = slot; return session.Apply(c); };
        auto loop = [&](bool on) { Command c{Action::Loop, generation}; c.loop = on; return session.Apply(c); };
        Require(!select(-1) && !select(SlotCount), "Invalid slot accepted");
        Require(!apply(Action::Play), "Empty playback accepted");
        // Loaded input plays on the side it names; a recording plays on the dummy again.
        {
            Command load; load.action = Action::Load; load.generation = session.GetView().generation; load.side = 0;
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
            // Loaded input follows the loop choice as a recording does.
            Require(session.GetView().loop, "Loop is not the default");
            session.Commit(frame);
            Require(session.GetView().mode == Mode::Playback && session.GetView().cursor == 0, "Loaded input did not loop");
            apply(Action::Stop);
            // A waiting frame repeats, buttons held, until the fight shows its
            // cue, then its offset's frames more; it gives up after a while.
            Command timed; timed.action = Action::Load; timed.generation = generation; timed.side = 0;
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
            // F8 and the pad's Play alternate as the session stands when each runs, however many queue before a frame.
            Require(session.Apply(timed) && apply(Action::AdvancePlayback) && session.GetView().mode == Mode::Playback, "A Play toggle did not play");
            Require(apply(Action::AdvancePlayback) && session.GetView().mode == Mode::Idle, "A second Play toggle did not stop");
            Require(apply(Action::AdvancePlayback) && session.GetView().mode == Mode::Playback, "A third Play toggle did not play again");
            apply(Action::Stop);
            // With the script predicting the free frame, a link's press lands
            // on that frame plus the offset: before it when negative. A seen
            // free frame still releases it. A hit is never predicted.
            Command early; early.action = Action::Load; early.generation = generation; early.side = 0;
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
            Command kept; kept.action = Action::Load; kept.generation = generation; kept.side = 0;
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
        // Standby, then recording; the slot keeps what it held until the recording stops.
        Require(apply(Action::AdvanceRecording) && session.GetView().mode == Mode::Standby && apply(Action::AdvanceRecording), "Record rejected");
        Frame physical{{Input{0x10, 0x10}, Input{0x80, 0x80}}};
        const auto output = session.Prepare(physical);
        Require(output[0].raw == 0 && output[1].raw == 0x10, "P1 did not control dummy");
        for (int i = 0; i < 5; ++i) session.Prepare(physical);
        Require(session.GetView().cursor == 0 && session.GetView().lengths[0] == 5, "Repeated or paused reads advanced recording");
        session.Commit(output);
        physical[0] = {0x20, 0x20}; session.Commit(session.Prepare(physical));
        Require(apply(Action::Stop) && loop(false) && apply(Action::Play), "Playback start failed");
        Require(session.Prepare(physical)[1].raw == 0x10, "First recorded frame skipped");
        session.Commit(session.Prepare(physical));
        Require(session.Prepare(physical)[1].raw == 0x20, "Playback order changed");
        session.Commit(session.Prepare(physical));
        Require(session.GetView().mode == Mode::Idle, "Single playback failed to stop");
        Require(loop(true) && apply(Action::Play), "Loop rejected");
        for (int i = 0; i < 10; ++i) session.Commit(session.Prepare(physical));
        Require(session.GetView().cursor == 0 && session.GetView().mode == Mode::Playback, "Loop boundary failed");
        Require(!select(1) && !apply(Action::Clear), "Slot mutated during playback");
        apply(Action::Stop); select(1); apply(Action::AdvanceRecording); apply(Action::AdvanceRecording);
        for (int i = 0; i < MaxFrames + 5; ++i) session.Commit(session.Prepare(physical));
        Require(session.GetView().lengths[1] == MaxFrames && session.GetView().mode == Mode::Idle, "Recording limit failed");
        Require(session.GetView().lengths[0] == 2, "Second slot overwrote first");
        // Record (F7 and the pad's) on the recorded slot asks about
        // overwriting it, as the session stands when it runs: at rest and
        // during playback it asks and changes nothing; while recording it
        // stops, so a second Record queued with it asks rather than recording over.
        {
            // Each Record names its request; the ask names the one it was for.
            const auto record = [&](std::uint64_t id) { Command c{Action::Record, generation}; c.requestId = id; return session.Apply(c); };
            Require(record(1) && session.GetView().mode == Mode::Idle && session.GetView().overwriteAsk == 1, "Record at rest on a recorded slot did not ask");
            Require(apply(Action::Play) && record(2) && session.GetView().mode == Mode::Playback && session.GetView().overwriteAsk == 2, "Record during playback did not ask");
            Require(apply(Action::AdvanceRecording) && record(3) && session.GetView().mode == Mode::Recording && session.GetView().overwriteAsk == 2, "Record on standby did not record");
            session.Commit(session.Prepare(physical));
            Require(record(4) && session.GetView().mode == Mode::Idle && session.GetView().lengths[1] == 1 && session.GetView().overwriteAsk == 2 && record(5) &&
                session.GetView().mode == Mode::Idle && session.GetView().overwriteAsk == 5, "A second queued Record recorded over the slot");
        }
        Require(session.GetView().history[0].size() <= HistoryRows, "History unbounded");
        session.SetReady(false); Require(!apply(Action::AdvanceRecording), "Loading allowed record");
        session.SetReady(true); session.SetCheckpoint(true);
        Require(apply(Action::Restore) && session.GetView().history[0].empty(), "Reset kept stale history");
        session.Reset(); session.Enter(); session.SetReady(true);
        Require(!apply(Action::AdvanceRecording) && !session.GetView().checkpoint && session.GetView().lengths[0] == 0, "Battle generation isolation failed");
        // The session is the one check every command passes: one sent for
        // another battle is refused there, whatever its action, and changes
        // nothing. The runtime's own actions pass it before the fight starts.
        {
            Session checked; checked.Enter();
            const auto current = checked.GetView().generation;
            // PlayOrder is the last action.
            for (int a = 0; a <= static_cast<int>(Action::PlayOrder); ++a) {
                Command stale{static_cast<Action>(a), current + 1};
                stale.slot = 3; stale.loop = false; stale.frames = {Input{1, 1}};
                Require(!checked.Apply(stale), "A command for another battle was accepted");
                Require(checked.GetView().selected == 0 && checked.GetView().loop && checked.GetView().lengths[0] == 0,
                    "A command for another battle changed the session");
            }
            for (Action a : {Action::Place, Action::ExportSlot, Action::DummyPlan, Action::DummyState, Action::Loop})
                Require(checked.Apply({a, current}), "A command for this battle was refused before the fight");
            Command plan{Action::DummyPlan, current}; plan.plan.when = 5;
            Require(!checked.Apply(plan), "An invalid reply plan was accepted");
            Command load{Action::Load, current}; load.frames = {Input{1, 1}};
            checked.SetReady(true); load.side = 2;
            Require(!checked.Apply(load), "Input loaded for a side that does not exist");
            // A save and then a reset, in order, with no position saved before:
            // the reset is the session's to decide once the save has run.
            Session positions; positions.Enter(); positions.SetReady(true);
            const auto battle = positions.GetView().generation;
            Require(positions.Apply({Action::Save, battle}), "A save was refused");
            positions.SetCheckpoint(true);
            Require(positions.Apply({Action::Restore, battle}), "A reset after a save that worked was refused");
            Session failed; failed.Enter(); failed.SetReady(true);
            Require(failed.Apply({Action::Save, failed.GetView().generation}), "A save was refused");
            failed.SetCheckpoint(false);
            Require(!failed.Apply({Action::Restore, failed.GetView().generation}), "A reset after a failed save was taken");
        }
        // The call out of Training: the countdown follows the call that stands
        // for the battle, as state read before every tick, so the battle
        // leaves once, for that call, whether its banner ran out or the
        // player chose go now; a call that ended never takes it away; and a
        // go now chosen while the battle's command queue is full still acts.
        {
            const std::uint64_t battle = 40;
            const auto call = [&](std::uint64_t serial, bool hurry = false) { CallControl c; c.call = serial; c.generation = battle; c.hurry = hurry; return c; };
            // One update: the countdown follows the state, then ticks.
            const auto update = [&](LeaveCountdown& countdown, const CallControl& control, bool fighting = true) {
                countdown.Follow(control, battle, 120);
                return countdown.Tick(fighting);
            };
            const auto leaves = [&](LeaveCountdown& countdown, const CallControl& control, int frames) {
                int fired = 0;
                for (int frame = 0; frame < frames; ++frame) if (update(countdown, control)) ++fired;
                return fired;
            };
            LeaveCountdown timer;
            Require(leaves(timer, CallControl{}, 300) == 0, "The battle left with no call");
            Require(timer.Follow(call(7), battle, 120) && !timer.Follow(call(7), battle, 120), "The call's countdown started twice");
            Require(leaves(timer, call(7), 300) == 1 && timer.Left() == 0, "The banner did not end in exactly one leave");
            // Go now: the next update that may leave does, once.
            LeaveCountdown early;
            int fired = 0, firedAt = -1;
            for (int frame = 0; frame < 300; ++frame) if (update(early, call(7, frame >= 30))) { ++fired; firedAt = frame; }
            Require(fired == 1 && firedAt == 30, "Go now did not leave once, at once");
            // Go now only leaves from a running fight, as the pause menu's exit does.
            LeaveCountdown intro;
            Require(!update(intro, call(7, true), false) && !update(intro, call(7, true), false) && intro.Left() == 1, "Go now left a fight that was not running");
            Require(update(intro, call(7, true)) && !update(intro, call(7, true)), "Go now did not leave once the fight ran");
            // The call ends on the countdown's last frame, while the fight is
            // not running: the next update stops it, before it could tick.
            LeaveCountdown last;
            for (int frame = 0; frame < 119; ++frame) update(last, call(7), false);
            Require(last.Left() == 1, "The countdown did not reach its last frame");
            Require(!update(last, CallControl{}) && last.Left() == 0 && leaves(last, CallControl{}, 200) == 0, "A call that ended took the battle away");
            // A go now chosen when the command queue would have been full is
            // state, not a message: once the countdown runs, it takes effect,
            // once and for its own call.
            LeaveCountdown waiting;
            Require(!update(waiting, call(7, true), false) && waiting.Left() == 1 && waiting.Call() == 7, "A go now chosen before the countdown ran was lost");
            Require(update(waiting, call(7, true)) && leaves(waiting, call(7, true), 300) == 0, "Go now took the battle away twice");
            // Another call replaces the count whole, its hurry and all, and
            // the earlier call's go now never reaches it.
            for (const bool hurried : {false, true}) {
                LeaveCountdown replaced;
                leaves(replaced, call(7, hurried), hurried ? 0 : 30);
                if (hurried) update(replaced, call(7, true), false);
                Require(replaced.Follow(call(8), battle, 120) && replaced.Left() == 120 && replaced.Call() == 8, "A replacement call did not start its own count");
                Require(!update(replaced, call(8)) && replaced.Left() == 119, "The replaced call's go now reached the new count");
                Require(leaves(replaced, call(8), 300) == 1, "The new call's count did not run its own length");
            }
            // A call for another battle is no call for this one.
            CallControl elsewhere = call(9); elsewhere.generation = battle + 1;
            LeaveCountdown other;
            Require(!other.Follow(elsewhere, battle, 120) && leaves(other, elsewhere, 300) == 0, "A call for another battle took this one away");
            // The battle closing forgets the call.
            LeaveCountdown closed;
            closed.Follow(call(7), battle, 120); closed.Reset();
            Require(closed.Left() == 0 && closed.Call() == 0 && closed.Follow(call(7), battle, 120), "A closed battle kept its call");
        }

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
            set.dummy = DummyState{}; set.dummy.action = 6;
            Require(dummy.Apply(set) && dummy.GetView().dummy.action == 6, "The menu's own playback refused as a dummy action");
            set.dummy.action = 7;
            Require(!dummy.Apply(set), "A dummy action beyond the menu's choices accepted");
            set.dummy = DummyState{}; set.dummy.counterHit = 3;
            Require(!dummy.Apply(set) && !ValidDummyState(set.dummy), "Counter hit beyond the menu's choices accepted");
        }
        {
            // Ember plays Player 2 in a recording, a playback on its side and a
            // reply; a playback on Player 1's side leaves the dummy alone.
            Session drive; drive.Enter(); drive.SetReady(true);
            const auto send = [&](Action action) { Command command; command.action = action; command.generation = drive.GetView().generation; return drive.Apply(command); };
            Require(!DrivesDummy(drive.GetView()), "An idle session played Player 2");
            Require(send(Action::AdvanceRecording) && DrivesDummy(drive.GetView()) && send(Action::AdvanceRecording) && DrivesDummy(drive.GetView()),
                "Standby or a recording did not play Player 2");
            drive.Commit(drive.Prepare(Frame{{Input{0x10, 0x10}, Input{}}}));
            Require(send(Action::Stop) && send(Action::Play) && DrivesDummy(drive.GetView()), "A playback on Player 2's side did not play Player 2");
            send(Action::Stop);
            Command load; load.action = Action::Load; load.generation = drive.GetView().generation; load.frames = {{0x10, 0x10, 0, 0}}; load.side = 0;
            Require(drive.Apply(load) && send(Action::Play) && !DrivesDummy(drive.GetView()), "A playback on Player 1's side played Player 2");
            send(Action::Stop);
            // The reply keeps the schedule it began with until it ends.
            ReplySchedule schedule; schedule.start = 0; schedule.anchor = 0; schedule.timing = 4;
            Require(drive.Reply(std::vector<Input>{{0x10, 0x10, 0, 0}}, schedule) && DrivesDummy(drive.GetView()) && drive.Schedule().timing == 4,
                "A reply did not play Player 2 by its schedule");
            // A save and a reset leave a recording running; a reset ends a reply.
            drive.SetCheckpoint(true);
            Require(send(Action::Save) && drive.Replying(), "A save ended a reply");
            Require(send(Action::Restore) && !drive.Replying() && drive.GetView().mode == Mode::Idle && drive.Schedule().anchor == -1,
                "A reset left a reply playing, or its schedule");
            Require(send(Action::AdvanceRecording) && send(Action::Save) && send(Action::Restore) && drive.GetView().mode == Mode::Standby &&
                send(Action::AdvanceRecording) && send(Action::Save) && send(Action::Restore) && drive.GetView().mode == Mode::Recording,
                "A save or a reset stopped standby or a recording");
        }
        {
            // A reset starts a playback again from its first frame, and a wait
            // in it needs its cue from the restored timeline: neither a hit the
            // reset undid nor part of a wait's offset after one survives it.
            Session rewind; rewind.Enter(); rewind.SetReady(true); rewind.SetCheckpoint(true);
            const auto send = [&](Action action) { Command command; command.action = action; command.generation = rewind.GetView().generation; return rewind.Apply(command); };
            Command load; load.action = Action::Load; load.generation = rewind.GetView().generation; load.side = 1;
            load.frames = {Input{2, 2, 0}, Input{8, 8, WaitHit, 2}, Input{0x18, 0x18, 0}};
            Require(rewind.Apply(load) && send(Action::Play), "The waiting playback did not start");
            const auto step = [&](bool hit) { rewind.Observe(false, hit); rewind.Commit(rewind.Prepare(Frame{})); };
            const auto waits = [&] { for (int frame = 0; frame < 3; ++frame) step(false); return rewind.GetView().cursor == 1; };
            const auto cued = [&] { step(true); step(false); step(false); return rewind.GetView().cursor == 2; };
            // A hit before the wait, then a reset to before it.
            step(true);
            Require(rewind.GetView().cursor == 1, "The playback did not reach its wait");
            Require(send(Action::Restore) && rewind.GetView().mode == Mode::Playback && rewind.GetView().cursor == 0, "A reset did not start the playback again");
            step(false);
            Require(waits(), "A hit the reset undid answered the wait");
            Require(cued(), "A hit after the reset did not answer the wait");
            // A reset during the wait's offset after its hit.
            Require(send(Action::Stop) && send(Action::Play), "The waiting playback did not start again");
            step(false); step(true); step(false);
            Require(rewind.GetView().cursor == 1, "The wait's offset ended early");
            Require(send(Action::Restore) && rewind.GetView().cursor == 0, "A reset did not start the playback again");
            step(false);
            Require(waits(), "Part of an offset the reset undid carried over");
            Require(cued(), "The wait after the reset did not take its own cue and offset");
        }
        {
            // The dummy's own reply plays a recording's frames or typed moves (its watch, schedule
            // and trace: training_reply_test.cxx; the slot it picks: training_pending_reply_test.cxx).
            // 623HP: three directions of three frames, then the button.
            std::vector<Input> dragon;
            for (unsigned bits : {8u, 8u, 8u, 2u, 2u, 2u, 10u, 10u, 10u, 0x40au, 10u, 0u}) dragon.push_back({bits, bits, 0, 0});

            Session reply; reply.Enter(); reply.SetReady(true);
            Command command; command.generation = reply.GetView().generation;
            Require(!reply.Reply(reply.Slot(2), ReplySchedule{}, 2), "An empty slot replied");
            command.action = Action::Select; command.slot = 2; reply.Apply(command);
            command.action = Action::AdvanceRecording; reply.Apply(command); reply.Apply(command);
            // Two idle frames, as a player leaves before pressing, then two presses.
            for (unsigned buttons : {0u, 0u, 0x10u, 0x20u}) { Frame frame; frame[1].raw = frame[1].mapped = buttons; reply.Commit(frame); }
            command.action = Action::Stop; reply.Apply(command);
            command.action = Action::Select; command.slot = 5; reply.Apply(command);
            Require(reply.Reply(reply.Slot(2), ReplySchedule{}, 2) && reply.GetView().mode == Mode::Playback && reply.GetView().active == 2 &&
                reply.GetView().selected == 5, "The reply did not start, or took the selection");
            Require(!reply.Reply(reply.Slot(2), ReplySchedule{}, 2), "A reply started over a playback");
            Require(reply.Prepare(Frame{})[1].raw == 0x10 && reply.Prepare(Frame{})[0].raw == 0, "The reply did not start on its first press, on Player 2");
            reply.Commit(reply.Prepare(Frame{}));
            Require(reply.Prepare(Frame{})[1].raw == 0x20, "The reply did not advance");
            reply.Commit(reply.Prepare(Frame{}));
            Require(reply.GetView().mode == Mode::Idle && reply.GetView().selected == 5 && reply.GetView().active == -1, "The reply looped or changed the selection");
            // The typed reply plays as a slot of its own; hit again, it is dropped.
            Require(reply.Reply(dragon, ReplySchedule{}) && reply.Replying() && reply.GetView().active == ReplySlot && reply.GetView().selected == 5 &&
                reply.Prepare(Frame{})[1].raw == 4, "The typed reply did not start, or not for the dummy facing left");
            reply.StopReply();
            Require(!reply.Replying() && reply.GetView().mode == Mode::Idle && reply.GetView().selected == 5 && reply.GetView().lengths[2] == 4, "A dropped reply changed the selection or touched a slot");
            command.action = Action::Select; command.slot = ReplySlot;
            Require(!reply.Apply(command), "The reply's own slot could be selected");
            // Play stops a reply and plays a slot that is on, whatever is selected.
            for (bool typed : {false, true}) {
                Require(typed ? reply.Reply(dragon, ReplySchedule{}) : reply.Reply(reply.Slot(2), ReplySchedule{}, 2), "The reply did not start");
                command.action = Action::Play;
                Require(reply.Apply(command) && !reply.Replying() && reply.GetView().mode == Mode::Playback && reply.GetView().active == 2 &&
                    reply.GetView().selected == 5 && reply.GetView().playbackSide == 1 && reply.GetView().cursor == 0, "Play did not replace the reply with a slot that is on");
                command.action = Action::Stop; reply.Apply(command);
            }
            // With no slot on, Play is refused after stopping the reply, and the pads pass as they are.
            command.action = Action::SlotOn; command.slot = 2; command.on = false;
            Require(reply.Apply(command) && reply.Reply(dragon, ReplySchedule{}), "The typed reply did not start with no slot on");
            command.action = Action::Play;
            Require(!reply.Apply(command) && !reply.Replying() && reply.GetView().mode == Mode::Idle && reply.GetView().cursor == 0, "Play started with no slot on");
            Frame physical; physical[0] = {0x10, 0x10, 0, 0}; physical[1] = {0x20, 0x20, 0, 0};
            const auto output = reply.Prepare(physical);
            Require(output[0].raw == 0x10 && output[1].raw == 0x20, "A refused playback changed physical input");
            // A reply takes Player 2 and gives a loaded slot's side back.
            command.action = Action::Select; command.slot = 2;
            Require(reply.Apply(command), "The recorded slot could not be selected");
            command.action = Action::Load; command.side = 0; command.frames = {{0x40, 0x40, 0, 0}};
            Require(reply.Apply(command) && reply.Reply(dragon, ReplySchedule{}), "The reply did not start over a loaded selection");
            command.action = Action::Play;
            Require(reply.Apply(command) && !reply.Replying() && reply.GetView().mode == Mode::Playback &&
                reply.GetView().active == 2 && reply.GetView().playbackSide == 0 && reply.GetView().cursor == 0,
                "Play did not play the loaded slot on its side");
            Require(reply.Prepare(Frame{})[0].raw == 0x40 && reply.Prepare(Frame{})[1].raw == 0,
                "Play used the reply instead of the loaded recording");
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
            // A tracked Record, then an untracked hotkey command and a tracked save
            // in the same game frame: both tracked results stay readable.
            Acks acks; bool accepted = false;
            acks.Note(5, true); acks.Note(0, false); acks.Note(6, false);
            Require(acks.Find(5, accepted) && accepted, "An untracked command replaced a tracked acknowledgement");
            Require(acks.Find(6, accepted) && !accepted, "A second tracked acknowledgement in one frame was lost");
            Require(!acks.Find(0, accepted) && !acks.Find(7, accepted), "An unsent request read as acknowledged");
            // Only newer tracked results push an old one out.
            for (int i = 0; i < 7; ++i) acks.Note(0, true);
            Require(acks.Find(5, accepted), "Untracked commands aged out a tracked acknowledgement");
            for (std::uint64_t id = 10; id < 17; ++id) acks.Note(id, true);
            Require(!acks.Find(5, accepted) && acks.Find(6, accepted) && acks.Find(16, accepted), "Acknowledgements did not keep the newest eight");
        }
        {
            // Combo check's ComboPlay: once on Player 1, then the dummy's side is back.
            Session session; RecordingBank bank; bank.Replace(0, {Input{0x10, 0x10, 0, 0}});
            session.Enter(bank); session.SetReady(true);
            Command play; play.generation = session.GetView().generation; play.action = Action::ComboPlay; play.frames = {Input{0x20, 0x20, 0, 0}, Input{0x40, 0x40, 0, 0}};
            Require(session.Apply(play) && session.GetView().playbackSide == 0, "ComboPlay plays on Player 1");
            Frame none;
            Frame out = session.Prepare(none); Require(out[0].raw == 0x20, "Player 1 gets the combo's first frame"); session.Commit(out);
            out = session.Prepare(none); Require(out[0].raw == 0x40, "and then its second"); session.Commit(out);
            Require(session.GetView().mode == Mode::Idle && session.GetView().playbackSide == 1, "It plays once, then the dummy's side is back");
            Require(session.Slot(0).size() == 1, "The slots are left alone");
        }
        {
            // "[4]6MK": its charge as frames to hold back, then its hold.
            std::vector<Input> charge(5, Input{8, 8, 0, 0});
            charge[3].wait = WaitReply; charge.push_back({4, 4, 0, 0}); charge.push_back({0x24, 0x24, 0, 0});
            ReplySchedule schedule; schedule.start = 0; schedule.hold = 3; schedule.anchor = 5;
            Session session; session.Enter(); session.SetReady(true);
            // The runtime keeps the charges full: the reply starts on its hold.
            Require(session.Reply(charge, schedule) && session.GetView().cursor == 3 && session.Holding(), "A kept charge was played");
            session.StopReply(); session.Commit(Frame{});
            std::vector<Input> lead = charge; lead[0].raw = lead[0].mapped = 2;
            Require(session.Reply(lead, schedule) && session.GetView().cursor == 0, "A lead of other inputs was skipped");
        }
        std::puts("Training session checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
