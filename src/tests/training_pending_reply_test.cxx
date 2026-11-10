// The dummy's recorded reply as the runtime decides it each observed frame
// (DummyReply.hxx): the slot and frames pinned when its stun is first seen,
// what drops it before it begins, and the picker moving on only once it starts.
#include "../training/DummyReply.hxx"
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
std::vector<Input> Frames(std::initializer_list<unsigned> raws) {
    std::vector<Input> frames;
    for (const unsigned raw : raws) frames.push_back({raw, raw, 0, 0});
    return frames;
}
// Blockstun with until frames left (-1: not timed), and the frames after it.
DummySeen Held(unsigned stretch, int until) { DummySeen seen; seen.held = 2; seen.stretch = stretch; seen.until = until; return seen; }
DummySeen Free(unsigned stretch, int free) {
    DummySeen seen; seen.stretch = stretch; seen.free = free; seen.out = 2;
    if (!free) seen.freed = 2;
    return seen;
}
// A press on its first frame (lead 0), and a 623 motion before its button (lead 9).
const std::vector<Input> Jab = Frames({0x40, 0});
const std::vector<Input> Dragon = Frames({8, 8, 8, 2, 2, 2, 10, 10, 10, 0x40a, 10, 0});
struct Bench {
    Session session; ReplyDecision replies; DummyPlan plan;
    explicit Bench(std::initializer_list<std::vector<Input>> slots, Order order = Order::InOrder) {
        RecordingBank bank; int slot = 0;
        for (const auto& frames : slots) bank.Replace(slot++, frames);
        bank.SetOrder(order);
        session.Enter(std::move(bank)); session.SetReady(true); session.SetCheckpoint(true);
        plan.when = 4;
    }
    ReplyStep Step(const DummySeen& seen) { return replies.Observe(plan, seen, session, 0); }
    bool Send(Action action, std::function<void(Command&)> fill = {}) {
        Command command; command.action = action; command.generation = session.GetView().generation;
        if (fill) fill(command);
        return session.Apply(command);
    }
    // A timed stun of 20 frames, then 10 free ones. Where the reply began,
    // in frames left of the stun, or -(frames after it) - 100; 1000 when not.
    // between: called on the stun's second frame, after the reply was made.
    int Stretch(unsigned stretch, const std::function<void()>& between = {}) {
        for (int until = 20; until >= 0; --until) {
            if (until == 19 && between) between();
            if (Step(Held(stretch, until)).started) return until;
        }
        for (int free = 0; free < 10; ++free) if (Step(Free(stretch, free)).started) return -free - 100;
        return 1000;
    }
};
int main() {
    try {
        {
            // Two recordings with different leads, in order: each stretch
            // waits for the one pinned for it, the jab until its button meets
            // the free frame, the dragon nine frames earlier.
            Bench bench({Jab, Dragon});
            Require(bench.Stretch(1) == 2 && bench.session.GetView().active == 0 && bench.session.Replying(), "The first reply did not wait for its own lead");
            bench.session.StopReply();
            bool pinned = false;
            const int began = bench.Stretch(2, [&] { pinned = bench.replies.Pending() && bench.replies.Pending()->slot == 1 && ReplyLead(bench.replies.Pending()->schedule) == 9; });
            Require(pinned && began == 11 && bench.session.GetView().active == 1, "The second reply did not wait for the slot pinned for it");
            // Played from its own copy, from its first press, for the dummy
            // on the right facing left.
            Require(bench.session.Prepare(Frame{})[1].raw == LeftBit && bench.session.GetView().activeLength == static_cast<int>(Dragon.size()), "The reply did not play its pinned frames");
        }
        {
            // Clear, Load or Mirror before the reply begins changes the slots:
            // the reply waiting is dropped and the stretch has none.
            const std::vector<std::function<bool(Bench&)>> changes{
                [](Bench& bench) { return bench.Send(Action::Clear); },
                [](Bench& bench) { return bench.Send(Action::Load, [](Command& command) { command.frames = Frames({0x80}); }); },
                [](Bench& bench) { return bench.Send(Action::Mirror); }};
            for (const auto& change : changes) {
                Bench bench({Dragon});
                bool changed = false;
                Require(bench.Stretch(1, [&] { changed = bench.replies.Pending() && change(bench); }) == 1000 && changed && !bench.replies.Pending(),
                    "A change to the slots before the reply began did not drop it");
            }
            // The plan going from a recording to typed moves drops it too.
            Bench bench({Dragon});
            ReplyChoice jab; jab.moves = Jab; jab.anchor = 0;
            DummyPlan typed = bench.plan; typed.choices = {jab};
            Require(ValidDummyPlan(typed), "The typed plan was not valid");
            Require(bench.Stretch(1, [&] { bench.plan = typed; }) == 1000, "A plan changed to typed moves kept the recorded reply");
            // The next stretch replies with the typed moves.
            Require(bench.Stretch(2) == 2 && bench.session.GetView().active == ReplySlot, "The typed plan did not reply after the change");
        }
        {
            // A timing that waits past the free frame: the reply made in the
            // stun still begins, on the free frame plus timing less its lead
            // and the input lag, here two frames after it.
            Bench bench({Jab});
            bench.plan.timing = MaxReplyTiming;
            for (int i = 0; i < 5; ++i) Require(!bench.Step(Held(1, -1)).started, "A reply to a stun not timed began in it");
            int began = -1;
            for (int free = 0; free < 10 && began < 0; ++free) if (bench.Step(Free(1, free)).started) began = free;
            Require(began == MaxReplyTiming - 1 - DummyInputLag && bench.session.Replying(), "A reply timed after the free frame did not begin");
        }
        {
            // A reset and an interruption before it begins drop it.
            for (const Action action : {Action::Restore, Action::Interrupt, Action::Play, Action::Stop, Action::AdvanceRecording}) {
                Bench bench({Dragon});
                Require(bench.Stretch(1, [&] { bench.Send(action); }) == 1000 && !bench.replies.Pending(), "A reset, interruption or command before the reply kept it");
            }
            Bench bench({Dragon});
            Require(bench.Stretch(1, [&] { bench.replies.Reset(); }) == 1000, "A gap in the frames kept the reply");
        }
        {
            // The order moves on only when a reply starts: one dropped leaves
            // the same slot for the next stretch.
            Bench bench({Jab, Frames({0x80, 0})});
            Require(bench.Stretch(1, [&] { bench.Send(Action::Interrupt); }) == 1000, "The interrupted reply began");
            Require(bench.Stretch(2) == 2 && bench.session.GetView().active == 0, "A reply that never started moved the order on");
            bench.session.StopReply();
            Require(bench.Stretch(3) == 2 && bench.session.GetView().active == 1, "A reply that started did not move the order on");
            // A recording that only stands is never pinned: with it alone on, there is no reply.
            Bench standing({Frames({0, 0, 0})});
            bool none = false;
            for (int until = 20; until >= 0; --until) none = standing.Step(Held(1, until)).none || none;
            Require(none && !standing.session.Replying(), "A recording that only stands was played as a reply");
        }
        {
            // Choices: drawn by weight, else in order, which as with recordings
            // moves on only once a reply starts.
            const auto choice = [](const std::vector<Input>& moves, int weight) { ReplyChoice c; c.moves = moves; c.anchor = 0; c.weight = weight; return c; };
            // Whether the dragon was pinned for the stretch, which then starts, or is dropped.
            const auto played = [&](Bench& bench, unsigned stretch, bool start) {
                bench.replies.Observe(bench.plan, Held(stretch, 19), bench.session, 0);
                const auto* pending = bench.replies.Pending();
                const bool second = pending && pending->frames.size() == Dragon.size();
                if (start) { Require(bench.Stretch(stretch) != 1000, "A choice did not start"); bench.session.StopReply(); }
                else bench.replies.Reset();
                return second;
            };
            Bench bench({});
            bench.plan.choices = {choice(Jab, 1), choice(Dragon, 3)};
            Require(ValidDummyPlan(bench.plan), "A plan with two choices was refused");
            int dragons = 0;
            for (unsigned stretch = 1; stretch <= 100; ++stretch) dragons += played(bench, stretch, false);
            Require(dragons > 60 && dragons < 90, "The random draw did not follow the weights");
            bench.plan.order = Order::InOrder;
            const bool a = played(bench, 101, false), b = played(bench, 102, true), c = played(bench, 103, true), d = played(bench, 104, false);
            Require(a == b && b != c && c != d, "The choices did not follow one another as replies started");
            bench.plan.choices[0].weight = 0;
            Require(!ValidDummyPlan(bench.plan), "A choice without a weight was accepted");
        }
        std::puts("Training pending reply checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
