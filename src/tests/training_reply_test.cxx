#include "../training/TrainingSession.hxx"
#include <cstdio>
#include <stdexcept>
// The dummy's reply: what it came out of and when it is free (DummyWatch),
// when a reply is due (ReplyDue), where a reply's button met the dummy
// (ReplyTrace), and what a plan or a recording schedules.
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
int main() {
    try {
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
        const auto next = watch.Observe(sample(0, 0, 1), 0);
        Require(first.freed == 1 && !next.freed, "A hit's end was not reported once");
        // While free it counts the frames since, and what it came out of.
        Require(first.free == 0 && first.out == 1 && next.free == 1 && next.out == 1, "The frames since the dummy was free were miscounted");
        // The same hit again counts down to its free frame, and a second hit in it starts over.
        auto seen = watch.Observe(sample(21, 300, 0, 10), 100);
        Require(seen.until == 5 && seen.stretch != first.stretch && seen.free == -1 && !seen.out, "A seen stun was not timed");
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
        // Free, a later timing waits past the free frame: due as many frames after it as the timing is past the lead.
        DummySeen after; after.free = 0;
        Require(ReplyDue(after, 3, 4) && !ReplyDue(after, 3, 5), "A reply past the free frame was due too soon");
        after.free = 1;
        Require(ReplyDue(after, 3, 5), "A reply past the free frame was not due on its frame");
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
        // Guard stance with nothing landed (the dummy held back while an
        // attack is near) is no stun, standing or crouching.
        for (unsigned guard : {14u, 15u}) {
            watch.Reset();
            Require(!watch.Observe(sample(0, 0, 0), 0).freed, "An idle dummy was freed");
            for (int frame = 0; frame < 4; ++frame) {
                const auto near = watch.Observe(sample(guard, 600, static_cast<float>(frame)), 100);
                Require(!near.held && !near.freed, "Guard stance with no hit held the dummy");
            }
            Require(!watch.Observe(sample(0, 0, 0), 0).freed, "Guard stance with no hit was replied to as a block");
        }
        // Blockstun into guard stance: free when the blockstun ends, the
        // stance after it neither delays that nor counts as a stun.
        watch.Reset();
        for (int frame = 0; frame < 3; ++frame) Require(watch.Observe(sample(22, 501, static_cast<float>(frame)), 100).held == 2, "Blockstun did not hold the dummy");
        const auto blocked = watch.Observe(sample(14, 600, 0), 100);
        Require(blocked.freed == 2 && !blocked.held, "The end of blockstun was not reported where guard stance followed");
        Require(!watch.Observe(sample(14, 600, 1), 100).freed && !watch.Observe(sample(0, 0, 0), 0).freed, "Guard stance after blockstun reported it again");
        const auto again = watch.Observe(sample(22, 501, 0), 100);
        Require(again.held == 2 && again.until == 3 && again.stretch != blocked.stretch, "Blockstun was not timed to its own end");
        // Hitstun into crouching guard stance, and a throw's sequence between frames, which neither ends nor starts a stun.
        watch.Reset();
        watch.Observe(sample(21, 300, 0, 10), 100);
        Require(watch.Observe(sample(15, 600, 0), 100).freed == 1, "The end of hitstun was not reported where guard stance followed");
        Require(!watch.Observe(sample(24, 0, 0), 100).held && !watch.Observe(sample(0, 0, 0), 0).freed, "A throw's sequence was taken for a stun");
        watch.Observe(sample(23, 310, 0, 10), 100); watch.Observe(sample(24, 0, 0), 100);
        Require(watch.Observe(sample(16, 700, 0), 0).freed == 1, "A launch did not reach its end across a sequence");
        {
            // Where a reply's button met the dummy: each reply traced once, from the frame
            // it is decided in, in frames the trace counts itself.
            Session replies; replies.Enter(); replies.SetReady(true);
            ReplyTrace trace; ReplyTiming timing; int reports = 0;
            const std::vector<Input> move(12, Input{0x10, 0x10, 0, 0});
            const auto stun = [](int held, int freed, unsigned stretch) { DummySeen seen; seen.held = held; seen.freed = freed; seen.stretch = stretch; return seen; };
            // One frame as the runtime runs it: the playback's input, then
            // the dummy seen, a reply decided if asked, then the trace.
            // stop: a new stun drops the reply playing first.
            const auto frame = [&](DummySeen seen, bool moving, bool decide = false, bool stop = false) {
                const bool pressed = replies.Replying();
                replies.Commit(replies.Prepare(Frame{}));
                if (stop) replies.StopReply();
                if (decide) Require(replies.Reply(move, ReplySchedule{}), "The reply did not start");
                const bool done = trace.Observe(replies.Replies(), replies.Replying(), pressed, seen, moving, 3, timing);
                reports += done;
                return done;
            };
            const auto idle = [&](int frames, bool moving) { for (int i = 0; i < frames; ++i) frame(stun(0, 0, 0), moving); };
            idle(3, true);
            Require(reports == 0, "A trace began with no reply");
            // Decided in a timed stun, the button sent the frame after, the dummy free and in its move the frame after that.
            frame(stun(2, 0, 7), false, true); frame(stun(2, 0, 7), false);
            Require(frame(stun(0, 2, 7), true) && timing.sent == 1 && timing.freed == 2 && timing.move == 2, "A reply's button, free frame or move was misplaced");
            // The log's differences: on time, the move starts on the free frame and the frame the button is acted on.
            Require(FramesBetween(timing.freed, timing.move) == "0" && FramesBetween(timing.Acted(), timing.move) == "0" && timing.timing == 3,
                "A traced reply's timing was misreported");
            // The reply plays on after its report, the dummy in its move: no second report.
            idle(2 * ReplyTrace::Grace, true);
            Require(reports == 1 && !replies.Replying(), "A reply was traced again after its report");
            // A new stun stops a reply and another is decided in the same frame: what played in it was the old one's.
            frame(stun(2, 0, 8), false, true);
            frame(stun(2, 0, 9), false, true, true);
            frame(stun(2, 0, 9), false);
            // Freed with no move ever coming: reported Grace frames after the reply's last frame.
            frame(stun(0, 2, 9), false);
            int after = 3;
            while (!frame(stun(0, 0, 0), false)) ++after;
            Require(reports == 2 && after == static_cast<int>(move.size()) - 1 + ReplyTrace::Grace && timing.sent == 1 && timing.freed == 2 && timing.move == ReplyTiming::NotSeen,
                "A reply whose move never came was not reported after its grace, or took the stopped reply's press");
            Require(FramesBetween(timing.freed, timing.move) == "none" && FramesBetween(timing.Acted(), timing.move) == "none" &&
                FramesBetween(timing.Acted(), timing.freed) == "0" && ReplyTiming{}.Acted() == ReplyTiming::NotSeen && SeenAt(timing.move) == "none",
                "A move never seen was given a timing");
            // Hit again: dropped, and not traced again while the same reply plays on.
            frame(stun(0, 1, 10), false, true);
            frame(stun(1, 0, 11), false);
            idle(2 * ReplyTrace::Grace, true);
            Require(reports == 2, "A trace went on across a new stun, or began again for the same reply");
            // A reset or a gap in the frames: dropped, and not traced again for the same reply.
            frame(stun(0, 1, 12), false, true);
            trace.Reset();
            idle(2 * ReplyTrace::Grace, true);
            Require(reports == 2, "A trace went on across a reset");
            // The battle closes with a trace begun, and another is entered:
            // nothing of it is reported there, and its first reply is its own.
            frame(stun(2, 0, 13), false, true); frame(stun(2, 0, 13), false);
            trace.Reset(); replies.Reset(); replies.Enter(); replies.SetReady(true);
            frame(stun(0, 2, 1), true); idle(2 * ReplyTrace::Grace, true);
            Require(reports == 2, "A trace begun in a closed battle was reported in the next");
            frame(stun(0, 1, 1), false, true); frame(stun(0, 0, 1), false);
            Require(frame(stun(0, 0, 1), true) && reports == 3 && timing.sent == 1 && timing.freed == 0 && timing.move == 2,
                "A reply in a new battle was not traced on its own");
            // Decided past the free frame by a later timing: the free frame comes before the decision.
            idle(2 * ReplyTrace::Grace, false);
            DummySeen late = stun(0, 0, 1); late.free = 2;
            frame(late, false, true); frame(stun(0, 0, 1), false);
            Require(frame(stun(0, 0, 1), true) && reports == 4 && timing.freed == -2 && timing.sent == 1 && timing.move == 2 &&
                FramesBetween(timing.freed, timing.move) == "4", "A reply decided past the free frame lost it");
        }
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
        // A recording lands from its first attack button, from its first pressed frame.
        const auto recorded = RecordingSchedule(dragon, 2);
        Require(ReplyStart(dragon) == 0 && recorded.anchor == 9 && recorded.hold == -1 && recorded.timing == 2 && ReplyLead(recorded) == 9 &&
            ReplyStart({}) == -1 && ReplyLead(RecordingSchedule({{8, 8, 0, 0}}, 0)) == 0, "A reply's start or lead misread");
        std::puts("Training reply checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
