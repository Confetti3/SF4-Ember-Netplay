#pragma once
#include "TrainingSession.hxx"
#include <algorithm>
#include <cstdint>
#include <string>
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
// Why a reply went before it played out, for the log: HitAgain, the dummy was
// hit again; NotRepliedTo, the stretch turned out to be none the plan replies to;
// NewStretch, a new stretch began while the reply waited; PlanChanged and
// SlotsChanged, the plan or the slots were edited meanwhile; PlayChanged, what the
// session plays changed (its epoch).
enum class ReplyDrop { None, HitAgain, NotRepliedTo, NewStretch, PlanChanged, SlotsChanged, PlayChanged };
inline const char* ReplyDropName(ReplyDrop why) {
    switch (why) {
    case ReplyDrop::HitAgain: return "hit again";
    case ReplyDrop::NotRepliedTo: return "stretch is not one the plan replies to";
    case ReplyDrop::NewStretch: return "new stretch";
    case ReplyDrop::PlanChanged: return "plan changed";
    case ReplyDrop::SlotsChanged: return "slots changed";
    case ReplyDrop::PlayChanged: return "session play changed";
    default: return "none";
    }
}
// What one frame's decision did. stopped: why a new stun ended the reply that
// played; dropped: why the pending reply went before it began; none: the
// stretch was to reply with a recording and no slot that is on can be one;
// declined: the stretch's chance roll said no reply.
struct ReplyStep { ReplyDrop stopped = ReplyDrop::None, dropped = ReplyDrop::None; bool made = false, started = false, none = false, declined = false; };
// Whether the plan replies to the stretch: to what holds the dummy, else to
// what it came out of. Typed replies that all begin with a hold (a charge)
// and reply on getting up begin with a hit, which may knock the dummy down,
// so the hold goes in from the hit; its move waits for the knockdown
// (ReplyFirstMoveDue), and it is let go if the hit ends standing.
// One choice without a hold would be made on the hit and played as it ends.
inline bool RepliesTo(const DummyPlan& plan, const DummySeen& seen) {
    const bool hold = !plan.choices.empty() && std::all_of(plan.choices.begin(), plan.choices.end(), [](const ReplyChoice& choice) { return choice.hold >= 0; });
    return StunTriggers(plan.when, seen) || (plan.when == 3 && seen.held == 1 && hold);
}
// One frame of the reply trace (the log, SF4E_TRAINING_TRACE): what the game
// says of the dummy (status raw and as classified, action, posture, combo
// damage), what the stun watch makes of it (stretch, held, until, free, out)
// and what the reply did, with the reason when one went. Nothing for a frame
// where the dummy is free (past its first few) and no reply waits or plays; a
// status that holds it in nothing known, as a throw's sequence, is traced.
inline std::string ReplyFrameTrace(int frame, const FighterSample& dummy, const DummySeen& seen, const ReplyStep& step, bool pending, bool replying) {
    const bool free = DummyHold(dummy) == 0 && (seen.free < 0 || seen.free > 3);
    if (!pending && !replying && free && step.stopped == ReplyDrop::None) return {};
    std::string line = "frame " + std::to_string(frame) + " dummy status " + std::to_string(dummy.status) + " (" + PhaseName(ClassifyStatus(dummy.status)) +
        ") action " + std::to_string(dummy.action) + " posture " + std::to_string(dummy.posture) + " combo damage " + std::to_string(dummy.comboDamage) +
        " | stretch " + std::to_string(seen.stretch) + " held " + std::to_string(seen.held) + " until " + std::to_string(seen.until) +
        " free " + std::to_string(seen.free) + " out " + std::to_string(seen.out) + " | pending " + (pending ? "yes" : "no") + ", replying " + (replying ? "yes" : "no");
    if (step.made) line += ", reply decided";
    if (step.declined) line += ", chance said no reply";
    if (step.none) line += ", no recording to reply with";
    if (step.started) line += ", reply began";
    if (step.dropped != ReplyDrop::None) line += std::string(", pending reply dropped: ") + ReplyDropName(step.dropped);
    if (step.stopped != ReplyDrop::None) line += std::string(", playing reply stopped: ") + ReplyDropName(step.stopped);
    return line;
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
        if (session.Replying()) {
            if (seen.held && seen.stretch != decided_) step.stopped = ReplyDrop::HitAgain;
            else if (seen.stretch == decided_ && !repliesTo) step.stopped = ReplyDrop::NotRepliedTo;
            if (step.stopped != ReplyDrop::None) session.StopReply();
        }
        if (pending_) {
            step.dropped = seen.stretch != reply_.stretch ? ReplyDrop::NewStretch : !repliesTo ? ReplyDrop::NotRepliedTo :
                !SamePlan(plan, reply_.plan) ? ReplyDrop::PlanChanged : session.Bank().Revision() != reply_.revision ? ReplyDrop::SlotsChanged :
                session.Epoch() != reply_.epoch ? ReplyDrop::PlayChanged : ReplyDrop::None;
            if (step.dropped != ReplyDrop::None) Reset();
        }
        if (!pending_ && repliesTo && seen.stretch != decided_) {
            decided_ = seen.stretch;
            if (roll % 100 < static_cast<unsigned>(plan.chance)) { step.made = Make(plan, seen.stretch, session); step.none = !step.made; }
            else step.declined = true;
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
            reply.slot = ReplySlot; reply.frames = choice.moves; reply.schedule = ChoiceSchedule(choice, plan.timing); reply.schedule.when = plan.when;
        } else {
            reply.slot = session.PeekReply();
            if (reply.slot < 0) return false;
            reply.frames = session.Bank().Slot(reply.slot);
            reply.schedule = RecordingSchedule(reply.frames, plan.timing); reply.schedule.when = plan.when;
        }
        reply_ = std::move(reply); pending_ = true;
        return true;
    }
};
// The frames stopped being consecutive (a gap or a reset): what the stun
// watch saw goes, and with it a reply that waits or is timed on it.
inline void ForgetStun(DummyWatch& watch, Session& session, ReplyDecision& replies) { ForgetStun(watch, session); replies.Reset(); }
} }
