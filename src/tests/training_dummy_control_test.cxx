// When the Training dummy plays from Player 2's pad (TrainingSession.hxx,
// DummyControl): the updates the pad covers, a pause-menu action under
// Ember, the input plan, the session and the control together through a
// held recording, resets and a restarted playback, and typed replies against
// the game's input path.
#include "../training/TrainingSession.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/PracticeSettings.hxx"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
sf4e::combo::Compiled Compile(const std::vector<std::string>& steps) {
    sf4e::combo::Compiled compiled; std::string error;
    Require(sf4e::combo::Synthesize(steps, true, compiled, error), "A move did not synthesize");
    return compiled;
}
// A typed reply's plan as BuildReplyPlan makes it (PracticeSettings.hxx).
DummyPlan Plan(const std::vector<std::string>& steps) {
    const auto compiled = Compile(steps);
    DummyPlan plan; plan.when = 4; plan.moves = compiled.frames;
    plan.offset = compiled.offset; plan.anchor = compiled.anchor; plan.hold = compiled.hold;
    Require(ValidDummyPlan(plan), "A typed reply made no plan");
    return plan;
}
// Frames a typed reply plays before its first move takes effect.
int Lead(const DummyPlan& plan) { return ReplyLead(PlanSchedule(plan, {})); }
int main() {
    try {
        {
            // The dummy plays from the pad for the updates that read Ember's
            // input and act on it: from the first one Ember supplies to the
            // simulated one after the last (DummyInputLag), and its own control
            // stands around them.
            using Step = DummyControl::Step;
            // One update: whether Ember supplies Player 2's input in it, then
            // whether the battle simulated it.
            const auto step = [](DummyControl& control, bool supplies, int action = 0, bool simulated = true) {
                const Step seen = control.Follow(supplies, action);
                if (simulated) control.Simulated();
                return seen;
            };
            const auto steps = [&](std::vector<bool> supplies) {
                DummyControl control; std::vector<Step> seen;
                for (const bool supply : supplies) seen.push_back(step(control, supply));
                return seen;
            };
            Require(steps({false, true, true, false, false}) == std::vector<Step>{Step::None, Step::Begin, Step::Keep, Step::Keep, Step::End},
                "The pad did not run from the update reading the first input to the one acting on the last");
            Require(steps({true, false, true, false, false}) == std::vector<Step>{Step::Begin, Step::Keep, Step::Keep, Step::Keep, Step::End},
                "The update acting on the first drive's last input left the pad");
            Require(steps({true, false, false, true}) == std::vector<Step>{Step::Begin, Step::Keep, Step::End, Step::Begin},
                "A gap that reads and acts on nothing of Ember's did not give the dummy its own control");
            {
                // Updates the battle does not simulate move nothing on: the
                // pad waits with them for the frame that acts on the last input.
                DummyControl held;
                Require(step(held, true) == Step::Begin && step(held, false, 0, false) == Step::Keep && step(held, false, 0, false) == Step::Keep &&
                    step(held, false) == Step::Keep && step(held, false) == Step::End,
                    "An update the battle did not simulate moved the input lag on");
            }
            // Ember's own changes to the action keep the pad; one from the
            // pause menu ends it and stands, and nothing begins again from the
            // update before it.
            DummyControl control; int native = 0;
            Require(!control.Reconcile(3), "An action change with Ember idle was taken for one under it");
            Require(step(control, true, native) == Step::Begin, "The pad did not begin");
            control.Set(native, 1);
            Require(!control.Reconcile(native) && step(control, true, native) == Step::Keep, "Ember's own change to the action ended the pad");
            native = 3;
            Require(control.Reconcile(native) && native == 3 && step(control, false, native) == Step::None,
                "A pause-menu action did not end the pad");
            // A pause-menu action in a drive's first update ends it, and the
            // input that update read is not acted on from the pad.
            DummyControl lagged;
            Require(step(lagged, true) == Step::Begin && lagged.Reconcile(3) && step(lagged, false, 3) == Step::None,
                "A pause-menu action in a drive's first update did not end the drive, or the pad went on after it");
            // A reset breaks the input's history, not whose action it is.
            DummyControl rewound;
            step(rewound, true);
            Require(rewound.Rewind() && rewound.Reconcile(3), "A reset lost the action Ember drove under");
            // The battle closes with the pad on, and with it off.
            DummyControl closing;
            step(closing, true);
            Require(closing.Release() && !closing.Release() && step(closing, false) == Step::None, "Closing the battle misreported the pad");
        }
        {
            // The input plan and the dummy's control together, update by
            // update as the runtime runs them: a recording held under the open
            // controls and resumed, a reset after a reply's first update and
            // later in it, a playback a reset starts again, and a pause-menu
            // action met in the same update as a reset.
            struct Bench {
                Session session; DummyControl control;
                // The Training menu's Action, as the pause menu leaves it.
                int action = 0;
                Bench() { session.Enter(); session.SetReady(true); session.SetCheckpoint(true); }
                bool Send(Action what) {
                    Command command; command.action = what; command.generation = session.GetView().generation; return session.Apply(command);
                }
                // An update's start, before its commands (ReconcileDummy), then
                // a reset among them if asked (RestoreCheckpoint). Whether the reset took.
                bool Start(bool reset) {
                    if (control.Reconcile(action)) Send(Action::Stop);
                    if (!reset) return true;
                    const bool restored = Send(Action::Restore);
                    control.Rewind();
                    return restored;
                }
                // The rest of a simulated update; menu: the controls hold the
                // pad. Whether Player 2 plays from the pad in it.
                bool Finish(bool menu = false) {
                    const InputPlan plan = PlanInput(session.GetView(), menu);
                    const auto step = control.Follow(plan.suppliesDummy, action);
                    if (plan.read) session.Commit(session.Prepare(Frame{}));
                    control.Simulated();
                    return step == DummyControl::Step::Begin || step == DummyControl::Step::Keep;
                }
                bool Update(bool menu = false) { Start(false); return Finish(menu); }
                std::vector<bool> Run(std::vector<bool> menus) {
                    std::vector<bool> pads;
                    for (const bool menu : menus) pads.push_back(Update(menu));
                    return pads;
                }
                // An update with a reset in it, up to its input.
                bool Reset() { return Start(true); }
            };
            const std::vector<Input> moves(8, Input{0x10, 0x10, 0, 0});
            {
                Bench recording;
                Require(recording.Send(Action::Record), "The recording did not start");
                const auto pads = recording.Run({false, false, true, true, false, false});
                Require(pads == std::vector<bool>{true, true, true, false, true, true},
                    "A recording held under the open controls kept the dummy on the pad, or did not take it back on resuming");
                Require(recording.session.GetView().mode == Mode::Recording && recording.session.GetView().cursor == 4,
                    "The held recording stopped or recorded the held updates");

                Bench reply;
                Require(reply.session.Reply(moves, ReplySchedule{}) && reply.Update(), "The reply's first input was not read from the pad");
                Require(reply.Reset() && !reply.Finish() && !reply.Update(), "A reset after the reply's first update left the dummy on the pad");
                Require(reply.session.Reply(moves, ReplySchedule{}) && reply.Run({false, false, false}) == std::vector<bool>{true, true, true}, "The reply did not play from the pad");
                Require(reply.Reset() && !reply.Finish(), "A reset during the reply left the dummy on the pad for the old timeline");

                Bench playback;
                Command load; load.action = Action::Load; load.generation = playback.session.GetView().generation; load.frames = moves; load.side = 1;
                Require(playback.session.Apply(load) && playback.Send(Action::Play), "The playback did not start");
                Require(playback.Run({false, false, false}) == std::vector<bool>{true, true, true}, "The playback did not play from the pad");
                Require(playback.Reset() && playback.session.GetView().mode == Mode::Playback && playback.session.GetView().cursor == 0,
                    "The reset did not start the playback again");
                Require(playback.Finish() && playback.Update(), "The restarted playback's input was not read from the pad");

                // The pause menu changed the action, and the first update to
                // see it resets the battle: the choice stands and Ember stops.
                Bench recordingTaken;
                Require(recordingTaken.Send(Action::Record) && recordingTaken.Run({false, false}) == std::vector<bool>{true, true},
                    "The recording did not play from the pad");
                recordingTaken.action = 3;
                Require(recordingTaken.Reset() && recordingTaken.session.GetView().mode == Mode::Idle && !recordingTaken.Finish() && !recordingTaken.Update(),
                    "A reset with a pause-menu action kept the recording playing the dummy");
                Bench playbackTaken;
                load.generation = playbackTaken.session.GetView().generation;
                Require(playbackTaken.session.Apply(load) && playbackTaken.Send(Action::Play) &&
                    playbackTaken.Run({false, false}) == std::vector<bool>{true, true}, "The playback did not play from the pad");
                playbackTaken.action = 3;
                Require(playbackTaken.Reset() && playbackTaken.session.GetView().mode == Mode::Idle && !playbackTaken.Finish() && !playbackTaken.Update(),
                    "A reset with a pause-menu action started the playback again under it");
            }
        }
        {
            // Typed replies, update by update as the runtime decides and plays
            // them (TrainingRuntime.cxx, BeforeUpdate and AfterUpdate), against
            // the game's side as IDA and the game's own traces show it. The game
            // reads Player 2's pad only under the pad control (0x599410), and
            // the dummy acts on it the next update, whether Lower input lag is
            // on or not (DummyInputLag), again only under the pad
            // control (0x5996F0). A press starts its move from the dummy's
            // first free frame on, a button only as a new press of all its bits
            // on that frame (0x598F60). A charge is the frames in a row its
            // direction reached the fighter (0x598590): the dummy's own guard
            // reads nothing, so it charges nothing. A new stun drops a reply
            // begun; guard stance after the stun, the dummy holding back while
            // an attack is near, does not.
            struct Game {
                std::vector<unsigned> read;
                unsigned last = 0;
                int run = 0, chargedAt = -1000;
                // One update: the pad control, Ember's input for Player 2, the
                // dummy's free frame (-1 before it is known), the move's
                // buttons and the direction it charges (0 none). Whether the move starts.
                bool Update(int frame, bool pad, unsigned input, int free, unsigned buttons, unsigned charge) {
                    read.push_back(pad ? input : 0u);
                    const unsigned acted = frame >= 1 ? read[frame - 1] : 0u, pressed = acted & ~last;
                    last = acted;
                    if (!(acted & charge)) run = 0;
                    else if (++run >= 55) chargedAt = frame;
                    const bool charged = !charge || frame - chargedAt <= 10;
                    return pad && free >= 0 && frame >= free && (pressed & buttons) == buttons && charged;
                }
            };
            // button: the update the reply's first attack button was read in;
            // free: the dummy's first free frame; move: the frame its move
            // starts; ended: the first update after the reply that no longer
            // plays it; padOff: the first one Player 2 is off the pad again;
            // read: Player 2's input as the game read it, per update.
            struct Run { int button = -1, free = -1, move = -1, ended = -1, padOff = -1; bool dropped = false; std::vector<unsigned> read; };
            // What the runtime keeps across stuns: the stun watch, the session
            // and the stretch a reply was last decided for. Faces right, as a
            // slot holds its frames, so a charge's back is Left.
            struct Dummy {
                DummyWatch watch; Session session; unsigned decided = ~0u;
                Dummy() { session.Enter(); session.SetReady(true); session.SetPositions(1, 0); }
            };
            // reply: the plan, played with the timing given. statuses: the
            // dummy's, one per frame; it is free from the first one after the
            // stun that holds it in none. gap: an update that moved the battle
            // more than a frame on, which the runtime does not observe
            // (TrainingRuntime.cxx, AfterUpdate), -1 for none. next: a plan
            // that replaces it from the frame given (a DummyPlan command).
            const auto play = [](Dummy& dummy, DummyPlan reply, int timing, const std::vector<unsigned>& statuses, unsigned charge,
                                 int gap = -1, const DummyPlan* next = nullptr, int nextAt = -1) {
                DummyWatch& watch = dummy.watch; Session& session = dummy.session; unsigned& decided = dummy.decided;
                reply.timing = timing;
                // The reply's frames, typed or recorded, for the game's side to find its button in.
                const std::vector<Input> frames = reply.moves.empty() ? session.Slot(reply.slot) : reply.moves;
                Run run; bool held = false, began = false;
                DummyControl control; Game game;
                const auto sample = [&](int frame) {
                    FighterSample dummy; dummy.valid = true; dummy.status = statuses[frame]; dummy.action = static_cast<int>(statuses[frame]) * 10;
                    return dummy;
                };
                int free = -1;
                for (int frame = 0, stunned = 0; frame < static_cast<int>(statuses.size()) && free < 0; ++frame) {
                    if (DummyHold(sample(frame)) > 0) stunned = 1;
                    else if (stunned) free = frame;
                }
                const auto press = std::find_if(frames.begin(), frames.end(), [](const Input& input) { return (input.raw & AttackButtons) != 0; });
                const unsigned buttons = press == frames.end() ? 0u : press->raw & AttackButtons;
                for (int frame = 0; frame < static_cast<int>(statuses.size()); ++frame) {
                    if (frame == nextAt) reply = *next;
                    const bool replying = session.Replying();
                    if (began && !replying && run.ended < 0) run.ended = frame;
                    const auto step = control.Follow(PlanInput(session.GetView(), false).suppliesDummy, 0);
                    const Frame output = session.Prepare(Frame{});
                    if (game.Update(frame, step == DummyControl::Step::Begin || step == DummyControl::Step::Keep, output[1].raw, free, buttons, charge) &&
                        run.move < 0)
                        run.move = frame;
                    if (began && step == DummyControl::Step::End && run.padOff < 0) run.padOff = frame;
                    control.Simulated();
                    if (frame == gap) { ForgetStun(watch, session); continue; }
                    // As the runtime observes the fight before committing the update (TrainingRuntime.cxx).
                    const DummySeen seen = watch.Observe(sample(frame), 100);
                    session.Observe(ClassifyStatus(statuses[frame]) == Phase::Neutral, false, -1,
                        session.Replying() && ReplyFirstMoveDue(session.Schedule(), seen));
                    session.Commit(output);
                    if (replying && run.button < 0 && (output[1].raw & AttackButtons)) run.button = frame;
                    if (seen.held) held = true;
                    else if (held && run.free < 0) run.free = frame;
                    if (seen.held && seen.stretch != decided && session.Replying()) { session.StopReply(); run.dropped = true; }
                    const int cause = seen.held ? seen.held : seen.out;
                    const ReplySchedule schedule = PlanSchedule(reply, session.Slot(reply.slot));
                    if (cause && seen.stretch != decided && ReplyBegins(schedule, seen)) {
                        decided = seen.stretch;
                        began = reply.moves.empty() ? session.Reply(reply.slot, schedule) : session.Reply(reply.moves, schedule);
                    }
                }
                run.read = game.read;
                return run;
            };
            // Idle, the stun, then what follows it, then idle until the reply is long over.
            const auto statuses = [](unsigned stun, int frames, unsigned after, int afterFrames) {
                std::vector<unsigned> out(5, 0u);
                out.insert(out.end(), frames, stun); out.insert(out.end(), afterFrames, after); out.insert(out.end(), 120, 0u);
                return out;
            };
            const unsigned guardCrouch = ActorStatus::GuardCrouch, walkBack = ActorStatus::Backward;
            using namespace sf4e::combo;
            // A button, two or three pressed together: all on one frame, with
            // none the frame before, so each is a new press of them all.
            const std::pair<const char*, unsigned> presses[] = {{"5MK", MK}, {"LP+LK", LP | LK}, {"MP+MK", MP | MK}, {"PPP", Punches}, {"KKK", Kicks}};
            for (const auto& pressed : presses) {
                const auto reply = Plan({pressed.first});
                const auto& frames = reply.moves;
                const int start = ReplyStart(frames);
                Require(start >= 0 && frames[start].raw == pressed.second && Lead(reply) == 0, "A press was not all its buttons on the reply's first frame");
                for (int timing = 0; timing <= MaxReplyTiming; ++timing) {
                    // Blockstun, seen first: replied to as it ends, later by
                    // timing. Seen again: at timing 0 the press is a reversal,
                    // its move starting on the dummy's free frame; a later one
                    // waits past it.
                    Dummy dummy;
                    const auto block = statuses(ActorStatus::DamageGuard, 18, 0u, 0);
                    const Run first = play(dummy, reply, timing, block, 0);
                    Require(first.free == 23 && first.move == first.free + (std::max)(2, timing) && !first.dropped,
                        "A press after a blockstun not seen before did not come out as the reply began");
                    const Run timed = play(dummy, reply, timing, block, 0);
                    Require(timed.button + 1 == timed.free + timing && timed.move == timed.free + timing && !timed.dropped,
                        "A press after a timed blockstun did not start its move on the free frame, plus timing");
                }
            }
            {
                // A direction held from the stun's first frame until the move
                // after it is due, then the move: down-back through a
                // blockstun, given way so its button is acted on the free
                // frame plus timing, the plan's and the move's own, whatever
                // the dummy does once free.
                for (const char* move : {"3HP", "623P", "236P", "22LK", "3HP@+3", "3HP@-2", "[HP]", "P(mash)"}) {
                    const auto reply = Plan({"[1]", move});
                    Step step; std::string error;
                    Require(ParseStep(move, step, error) && reply.hold == 0 && ReplyStart(reply.moves) == 0 && reply.offset == step.offset,
                        "A reply did not begin with its hold");
                    for (const unsigned after : {0u, guardCrouch}) for (int timing = -MaxReplyTiming; timing <= MaxReplyTiming; ++timing) {
                        const int at = timing + step.offset;
                        Dummy dummy;
                        const auto block = statuses(ActorStatus::DamageGuard, 18, after, 20);
                        const Run first = play(dummy, reply, timing, block, 0);
                        // Not timed, the hold gives way once the dummy is seen free, later by timing.
                        Require(first.read[6] == (Down | Left) && first.read[22] == (Down | Left) &&
                            first.button + 1 == first.free + (std::max)(Lead(reply) + 2, at) && !first.dropped,
                            "A hold over a blockstun not seen before did not begin with it, or gave way at the wrong frame");
                        const Run timed = play(dummy, reply, timing, block, 0);
                        bool heldThrough = true;
                        for (int frame = 6; frame < timed.button - Lead(reply); ++frame) heldThrough = heldThrough && timed.read[frame] == (Down | Left);
                        Require(heldThrough && timed.button + 1 == timed.free + at && (at < 0 || timed.move == timed.free + at) && !timed.dropped,
                            "A hold over a timed blockstun did not give way so its move's button met the free frame plus timing");
                    }
                }
                // Holds and waits with a count lead into the first move: the
                // reply starts so they and the move's motion end with its button
                // on the free frame plus timing, a hold with no count after them
                // giving way the same, whatever the dummy does once free. The
                // stun is long enough to hold them all in.
                for (const auto& line : std::vector<std::vector<std::string>>{{"[2]30f", "5MK"}, {"[4]30f", "[4]", "5MK"}, {"[2]30f", "[4]", "623P"}}) {
                    const auto reply = Plan(line);
                    Require(Lead(reply) == (line.size() == 2 ? 33 : line[2] == "5MK" ? 0 : 9), "A reply's lead did not count its leading input");
                    for (const unsigned after : {0u, walkBack, guardCrouch}) for (const int timing : {0, 2, MaxReplyTiming}) {
                        Dummy dummy;
                        const auto block = statuses(ActorStatus::DamageGuard, 90, after, 40);
                        play(dummy, reply, timing, block, 0);
                        const Run run = play(dummy, reply, timing, block, 0);
                        Require(run.free == 95 && run.button + 1 == 95 + timing && run.move == 95 + timing && !run.dropped, "A reply with input leading into its first move did not press on the free frame plus timing");
                        // "[2]30f > 5MK": the 30 frames of down, then the move's 3.
                        bool led = true;
                        for (int frame = run.button - 33; frame < run.button - 3 && line.size() == 2; ++frame) led = led && run.read[frame] == Down;
                        Require(led, "A reply's leading hold was not played before its move");
                    }
                }
                // The first move's own offset counts from the free frame with the
                // timing, whatever leads into it.
                for (const auto& line : std::vector<std::vector<std::string>>{{"[2]30f", "5MK@+3"}, {"[2]30f", "5MK@-2"}, {"[2]30f", "[4]", "5MK@+3"},
                         {"[2]30f", "[4]", "5MK@-2"}}) {
                    DummyPlan reply = Plan(line);
                    for (const unsigned after : {0u, walkBack}) for (const int timing : {0, 2}) {
                        reply.timing = timing;
                        const int at = PlanSchedule(reply, {}).timing;
                        Require(at == timing + (line.back().back() == '3' ? 3 : -2), "A reply's first move's offset was not in its timing");
                        Dummy dummy;
                        const auto block = statuses(ActorStatus::DamageGuard, 90, after, 40);
                        play(dummy, reply, timing, block, 0);
                        const Run run = play(dummy, reply, timing, block, 0);
                        Require(run.button + 1 == 95 + at && (at < 0 || run.move == 95 + at) && !run.dropped,
                            "A reply's first move did not keep its own offset");
                    }
                }
                // A later move on a frame of the reply: the hold before it lasts
                // up to its motion, and its button lands on that frame.
                for (const char* move : {"3HP#60", "623HP#60"}) {
                    const auto reply = Plan({"5LP", "[4]", move});
                    const int motion = move[1] == '2' ? 9 : 3;
                    Dummy dummy;
                    const auto block = statuses(ActorStatus::DamageGuard, 90, 0u, 120);
                    play(dummy, reply, 0, block, 0);
                    const Run run = play(dummy, reply, 0, block, 0);
                    bool held = true;
                    for (int frame = run.button + 2; frame < run.button + 60 - motion; ++frame) held = held && run.read[frame] == Left;
                    Require(run.move == 95 && held && (run.read[run.button + 60] & HP), "A move on a frame of the reply did not land there, or the hold before it broke");
                }
                // A move with no button, a jump or a dash, takes effect on its
                // last direction, which meets the free frame plus timing after
                // the input leading into it, alone or before a later attack.
                for (const auto& line : std::vector<std::vector<std::string>>{{"[2]30f", "8"}, {"[2]30f", "66"}, {"[2]30f", "8", "5HP"}, {"[2]30f", "66", "5HP"}}) {
                    const auto reply = Plan(line);
                    const bool jump = line[1] == "8";
                    Require(Lead(reply) == (jump ? 30 : 34), "A move with no button was not timed from its last direction");
                    for (const int timing : {0, 2}) {
                        Dummy dummy;
                        const auto block = statuses(ActorStatus::DamageGuard, 90, 0u, 40);
                        play(dummy, reply, timing, block, 0);
                        const Run run = play(dummy, reply, timing, block, 0);
                        const int acted = 95 + timing - DummyInputLag;
                        Require(run.read[acted] == (jump ? Up : Right) && run.read[acted - 1] == (jump ? Down : 0u) && (jump || run.read[acted - 2] == Right),
                            "A jump or a dash did not take effect on the free frame plus timing");
                    }
                }
                // A neutral "5" presses nothing: typed, after holds or before
                // an attack, the reply is refused, and the dummy keeps the plan
                // it had, its button acted on the free frame plus timing.
                {
                    DummyPlan settings; settings.when = 4; std::string error;
                    DummyPlan kept;
                    Require(BuildReplyPlan("[2]30f > 5MK", settings, kept, error) && ValidDummyPlan(kept), "The reply kept was not built");
                    for (const char* line : {"[2]30f > 5", "[2]30f > 3f > [4]10f > 5", "[2]30f > 5 > 5MK"}) {
                        DummyPlan reply = kept;
                        Require(!BuildReplyPlan(line, settings, reply, error) && !error.empty(), "A reply that presses nothing was built");
                        Require(reply.moves.size() == kept.moves.size() && reply.anchor == kept.anchor && reply.hold == kept.hold && reply.offset == kept.offset,
                            "A refused reply changed the plan");
                        for (const int timing : {0, 2}) {
                            Dummy dummy;
                            const auto block = statuses(ActorStatus::DamageGuard, 90, 0u, 40);
                            play(dummy, reply, timing, block, 0);
                            const Run run = play(dummy, reply, timing, block, 0);
                            Require(run.button + 1 == 95 + timing && run.move == 95 + timing && (run.read[run.button] & MK),
                                "The plan kept did not reply on the free frame plus timing");
                        }
                    }
                }
                // A recording replies from its own frames: from its first pressed
                // frame, its first attack button on the free frame plus timing.
                for (const int timing : {0, 2}) {
                    Dummy dummy;
                    Command select; select.action = Action::Select; select.generation = dummy.session.GetView().generation; select.slot = 3;
                    Command load = select; load.action = Action::Load; load.side = 1;
                    load.frames = {{0, 0, 0, 0}, {0, 0, 0, 0}, {Down, Down, 0, 0}, {Down, Down, 0, 0}, {Down | MK, Down | MK, 0, 0}, {0, 0, 0, 0}};
                    Require(dummy.session.Apply(select) && dummy.session.Apply(load), "The recording was not loaded");
                    DummyPlan recording; recording.when = 4; recording.slot = 3;
                    const auto block = statuses(ActorStatus::DamageGuard, 90, 0u, 40);
                    play(dummy, recording, timing, block, 0);
                    const Run run = play(dummy, recording, timing, block, 0);
                    Require(run.button + 1 == 95 + timing && run.move == 95 + timing && run.read[run.button - 1] == Down && run.read[run.button - 3] == 0,
                        "A recording did not reply from its own frames");
                }
                // A reply keeps the schedule it began with: the plan changed to other
                // moves, or to a recording, while its hold waits does not move it.
                {
                    const auto reply = Plan({"[1]", "3HP@+5"});
                    DummyPlan recording; recording.when = 4; recording.slot = 3;
                    for (const DummyPlan& next : {Plan({"5MK"}), recording}) {
                        Dummy dummy;
                        const auto block = statuses(ActorStatus::DamageGuard, 90, 0u, 40);
                        play(dummy, reply, 0, block, 0);
                        const Run run = play(dummy, reply, 0, block, 0, -1, &next, 30);
                        Require(run.button + 1 == 100 && run.move == 100 && !run.dropped, "A reply's hold gave way by a plan that replaced it");
                    }
                }
                // A gap in the frames takes what was seen of the stun, and the
                // hold waiting on it goes too: over the free frame, and in the
                // frames a later timing waits past it.
                for (const int gap : {23, 24}) {
                    Dummy dummy;
                    const auto reply = Plan({"[1]", "3HP"});
                    const auto block = statuses(ActorStatus::DamageGuard, 18, 0u, 0);
                    play(dummy, reply, MaxReplyTiming, block, 0);
                    const Run run = play(dummy, reply, MaxReplyTiming, block, 0, gap);
                    Require(run.ended == gap + 1 && run.padOff == gap + 2 && run.move < 0, "A gap in the frames left a hold waiting on the pad");
                }
                // A stun longer than any recovery, a dizzy: the hold waits it out.
                const auto reply = Plan({"[1]", "3HP"});
                {
                    Dummy dummy;
                    const auto dizzy = statuses(ActorStatus::Stun, 150, 0u, 0);
                    play(dummy, reply, 0, dizzy, 0);
                    Require(play(dummy, reply, 0, dizzy, 0).move == 155, "A hold gave up before a long stun ended");
                }
                // A hold later in a reply waits as any link does: walking back
                // or in guard stance the dummy is never seen neutral, and the
                // wait gives up rather than keep it on the pad.
                const auto later = Plan({"5LP", "[4]120f", "[4]", "6P"});
                for (const unsigned after : {walkBack, guardCrouch}) {
                    Dummy dummy;
                    const auto walking = statuses(ActorStatus::DamageGuard, 18, after, 400);
                    play(dummy, later, 0, walking, 0);
                    const Run run = play(dummy, later, 0, walking, 0);
                    Require(run.ended > run.free + 120 + MaxWaitFrames && run.ended < run.free + 120 + MaxWaitFrames + 12,
                        "A later hold in a reply kept the dummy on the pad past its wait");
                }
                // The hold is the charge only when the stun is long enough to
                // charge in: after a short blockstun "[1] > 6P" comes out uncharged.
                const auto charge = Plan({"[1]", "6P"});
                Dummy longDummy;
                const auto longBlock = statuses(ActorStatus::DamageGuard, 80, 0u, 0);
                play(longDummy, charge, 0, longBlock, Left);
                Require(play(longDummy, charge, 0, longBlock, Left).move == 85, "A charge held through a long blockstun was no reversal");
                Dummy shortDummy;
                const auto shortBlock = statuses(ActorStatus::DamageGuard, 18, 0u, 0);
                play(shortDummy, charge, 0, shortBlock, Left);
                Require(play(shortDummy, charge, 0, shortBlock, Left).move < 0, "A short blockstun charged a hold");
            }
            for (const char* move : {"[4]6P", "[2]8K"}) {
                const auto reply = Plan({move});
                const auto& frames = reply.moves;
                const unsigned back = move[1] == '4' ? Left : Down;
                int hold = 0;
                while (hold < static_cast<int>(frames.size()) && frames[hold].raw == back) ++hold;
                Require(hold == ChargeFrames && ChargeFrames >= 60 && ReplyStart(frames) == 0 && Lead(reply) == ChargeFrames + 3,
                    "A charge was held too briefly for some fighter, or its lead misread");
                for (const int timing : {-1, 0, 2}) {
                    // Getting up, and a blockstun with the charge held back
                    // through it, each long enough to charge in: the first time
                    // replied to as it ends; timed, the button lands on the free
                    // frame, plus timing, charged.
                    const std::pair<unsigned, int> stuns[] = {{static_cast<unsigned>(ActorStatus::Rise), 100}, {static_cast<unsigned>(ActorStatus::DamageGuard), 90}};
                    for (const auto& stun : stuns) {
                        const int length = stun.second;
                        Dummy dummy;
                        const auto held = statuses(stun.first, length, 0u, 0);
                        const Run first = play(dummy, reply, timing, held, back);
                        Require(first.free == 5 + length && first.button == first.free + 1 + ChargeFrames + 3 && first.move == first.button + 1 &&
                            !first.dropped, "A charge reply to a stun not seen before was not played as it ended");
                        const Run timed = play(dummy, reply, timing, held, back);
                        Require(timed.button + 1 == timed.free + timing && !timed.dropped,
                            "A charge reply's button did not land on the free frame, plus timing");
                        Require(timing < 0 || timed.move == timed.button + 1, "A charge held through the stun did not come out on its button");
                    }
                    // Blockstun too short to charge in, then guard stance standing or crouching:
                    // the reply begins on the stun's first frame, is not dropped, and comes out charged.
                    for (const unsigned guard : {static_cast<unsigned>(ActorStatus::GuardStand), static_cast<unsigned>(ActorStatus::GuardCrouch)}) {
                        Dummy blockDummy;
                        const auto block = statuses(ActorStatus::DamageGuard, 18, guard, 80);
                        play(blockDummy, reply, timing, block, back);
                        const Run blocked = play(blockDummy, reply, timing, block, back);
                        Require(blocked.free == 23 && blocked.button == 5 + 1 + ChargeFrames + 3 && blocked.move == blocked.button + 1 && !blocked.dropped,
                            "A charge reply over a short blockstun was dropped in guard stance, started late or came out uncharged");
                    }
                }
            }
        }
        std::puts("Training dummy control checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
