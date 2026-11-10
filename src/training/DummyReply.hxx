#pragma once
#include "TrainingSession.hxx"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// When the dummy replies and with what: the one decision the runtime makes
// each observed frame (TrainingRuntime.cxx, AfterUpdate), which the tests call too.
namespace sf4e { namespace training {
inline bool SameInput(const Input& a, const Input& b) { return a.mapped == b.mapped && a.raw == b.raw && a.wait == b.wait && a.offset == b.offset; }
// Whether two plans reply the same way.
inline bool SamePlan(const DummyPlan& a, const DummyPlan& b) {
    const auto same = [](const ReplyChoice& x, const ReplyChoice& y) {
        return x.offset == y.offset && x.anchor == y.anchor && x.hold == y.hold && x.weight == y.weight &&
            x.moves.size() == y.moves.size() && std::equal(x.moves.begin(), x.moves.end(), y.moves.begin(), SameInput);
    };
    return a.when == b.when && a.chance == b.chance && a.timing == b.timing && a.order == b.order &&
        a.choices.size() == b.choices.size() && std::equal(a.choices.begin(), a.choices.end(), b.choices.begin(), same);
}
// The reply made for one stretch of stun, from the frame the stretch is first
// seen until the reply begins: the slot it plays (ReplySlot for typed moves,
// choice saying which of the plan's), its own copy of the frames and the
// schedule made from that copy, so it waits for the frames it will play
// whatever the slots do meanwhile. It is
// dropped if the plan, the slots (their revision) or what the session plays
// (its epoch) change before it begins, and when its stretch ends.
struct PendingReply {
    unsigned stretch = 0;
    int slot = -1, choice = -1;
    std::vector<Input> frames;
    ReplySchedule schedule;
    DummyPlan plan;
    std::uint64_t revision = 0, epoch = 0;
};
// What one frame's decision did. stopped: a new stun ended the reply that
// played; dropped: the pending reply went before it began; none: the stretch
// was to reply with a recording and no slot that is on can be one.
struct ReplyStep { bool stopped = false, made = false, started = false, dropped = false, none = false; };
// Whether the plan replies to the stretch: to what holds the dummy, else to
// what it came out of. Typed replies that all begin with a hold (a charge)
// and reply on getting up begin with a hit, which may knock the dummy down,
// so the hold goes in from the hit; it is let go if the hit ends standing.
// One choice without a hold would be made on the hit and played as it ends.
inline bool RepliesTo(const DummyPlan& plan, const DummySeen& seen) {
    const int cause = seen.held ? seen.held : seen.out;
    const bool hold = !plan.choices.empty() && std::all_of(plan.choices.begin(), plan.choices.end(), [](const ReplyChoice& choice) { return choice.hold >= 0; });
    return cause && (plan.when == 4 || plan.when == cause || (plan.when == 3 && seen.held == 1 && hold));
}
class ReplyDecision {
public:
    // Each observed frame, once the session has committed it. roll: any
    // random number; each stretch rolls its chance once, as it is first seen
    // to be one the plan replies to.
    ReplyStep Observe(const DummyPlan& plan, const DummySeen& seen, Session& session, unsigned roll) {
        ReplyStep step;
        const bool repliesTo = RepliesTo(plan, seen);
        // Hit again, or the stretch turned out to be none the plan replies
        // to: a reply already begun is dropped, and a new stretch decides anew.
        if (session.Replying() && ((seen.held && seen.stretch != decided_) || (seen.stretch == decided_ && !repliesTo))) {
            session.StopReply(); step.stopped = true;
        }
        if (pending_ && (seen.stretch != reply_.stretch || !repliesTo || !SamePlan(plan, reply_.plan) ||
                session.Bank().Revision() != reply_.revision || session.Epoch() != reply_.epoch)) {
            Reset(); step.dropped = true;
        }
        if (!pending_ && repliesTo && seen.stretch != decided_) {
            decided_ = seen.stretch;
            if (roll % 100 < static_cast<unsigned>(plan.chance)) { step.made = Make(plan, seen.stretch, session); step.none = !step.made; }
        }
        if (pending_ && ReplyBegins(reply_.schedule, seen)) {
            pending_ = false;
            // Ember plays the reply from the next update, and the dummy from the pad.
            step.started = session.Reply(std::move(reply_.frames), reply_.schedule, reply_.slot);
            if (step.started) choices_.Commit(reply_.choice);
        }
        return step;
    }
    // A reset, a gap in the frames or the battle closing: the reply waiting goes.
    void Reset() { pending_ = false; reply_ = PendingReply{}; }
    // The random draw among the choices, seeded with the session's (Session::Seed).
    void Seed(unsigned seed) { choices_.Seed(seed); }
    const PendingReply* Pending() const { return pending_ ? &reply_ : nullptr; }
private:
    // The stretch of stun a reply was last decided for, so each gets one roll.
    unsigned decided_ = 0;
    bool pending_ = false;
    PendingReply reply_;
    // Which of the plan's choices plays, as the session picks a recording:
    // proposed when the reply is made, moved on once it starts.
    SlotPicker choices_{3};
    bool Make(const DummyPlan& plan, unsigned stretch, Session& session) {
        PendingReply reply;
        reply.stretch = stretch; reply.plan = plan; reply.revision = session.Bank().Revision(); reply.epoch = session.Epoch();
        if (!plan.choices.empty()) {
            int weights[SlotCount] = {};
            for (std::size_t i = 0; i < plan.choices.size(); ++i) weights[i] = plan.choices[i].weight;
            reply.choice = choices_.Peek((1u << plan.choices.size()) - 1u, plan.order, weights);
            const ReplyChoice& choice = plan.choices[reply.choice];
            reply.slot = ReplySlot; reply.frames = choice.moves; reply.schedule = ChoiceSchedule(choice, plan.timing);
        } else {
            reply.slot = session.PeekReply();
            if (reply.slot < 0) return false;
            reply.frames = session.Bank().Slot(reply.slot);
            reply.schedule = RecordingSchedule(reply.frames, plan.timing);
        }
        reply_ = std::move(reply); pending_ = true;
        return true;
    }
};
// The frames stopped being consecutive (a gap or a reset): what the stun
// watch saw goes, and with it a reply that waits or is timed on it.
inline void ForgetStun(DummyWatch& watch, Session& session, ReplyDecision& replies) { ForgetStun(watch, session); replies.Reset(); }
} }
