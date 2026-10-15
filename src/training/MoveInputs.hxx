#pragma once
#include "MoveNotation.hxx"
#include "TrainingSession.hxx"
#include <algorithm>
#include <vector>

// Turns typed moves into pad input the training session can play back.
// Directions take a few frames each and a button two; between moves the
// input waits on the fight itself: a linked move for the fighter to be able
// to act again (plus the link gap), a cancelled move for the hit to land.
// The game decides what comes out.
namespace sf4e { namespace combo {
constexpr unsigned Up = 1, Down = 2, Left = training::LeftBit, Right = training::RightBit;
constexpr int FollowDelay = 10;
// Frames a charge is held. The fighters' command files ask 42 (M. Bison), 50
// (Guile's and Dee Jay's Sonic Boom), 55 (most charge moves and every charge
// super and ultra) and 60 (Crystal Flash, Gen); a typed move does not know
// its fighter, so it holds enough for all of them, plus a little.
constexpr int ChargeFrames = 62;
inline unsigned DirectionBits(char digit, bool facingRight) {
    const unsigned f = facingRight ? Right : Left, b = facingRight ? Left : Right;
    switch (digit) {
    case '1': return Down | b; case '2': return Down; case '3': return Down | f;
    case '4': return b; case '6': return f;
    case '7': return Up | b; case '8': return Up; case '9': return Up | f;
    default: return 0;
    }
}
// "PP" is any two punches: the lightest ones are pressed.
inline unsigned ChosenButtons(const Step& step) {
    if (step.buttons != Punches && step.buttons != Kicks) return step.buttons;
    const unsigned order[2][3] = {{LP, MP, HP}, {LK, MK, HK}};
    unsigned mask = 0;
    for (int i = 0; i < step.need && i < 3; ++i) mask |= order[step.buttons == Kicks][i];
    return mask;
}
// Typed moves as the dummy plays them for its reply, and when their first
// move lands (DummyPlan, ReplySchedule). frames: as a slot holds them, from
// the reply's first played frame. offset: the first move's own "@N", frames
// after the dummy is free it lands, added to the reply's timing. anchor: the
// frame the first move's input takes effect on: its button; for a move with
// none (a jump, a dash), its last direction; for a jumping attack, its jump.
// hold: the frame of its leading hold (WaitReply), -1 for none.
//
// How the reply is laid out in time, the reply anchored at the dummy's free
// frame (ReplyDue): holds and waits with a count before the first move lead
// into it; a hold with no count there is its leading hold (WaitReply), its
// length the stun's; the first move's anchor meets the free frame plus the
// timing and its "@N". Each later move waits on the one before (a link for
// the dummy to be free, a cancel for the hit, a follow-up a few frames), its
// "@N" counted from that cue, or lands on a frame of the reply ("#N", a later
// move's "@N" counted from that frame), counted from the reply's first played frame,
// which only frames of a known length before it can reach: "#N" after a wait
// on the fight is refused, as on the first move with nothing before it. A
// first move pressed and let go ("]HP[") comes out on the release, which the
// reply cannot time, so it is refused (ParseSteps).
struct Compiled { std::vector<training::Input> frames; int offset = 0, anchor = -1, hold = -1; };
// Directions are read for the facing; the buttons of a step are chosen once.
// steps: as ParseSteps leaves them. False, with out empty and error set, when
// a step is not a move or presses nothing, a "#N" cannot be laid out, there
// is no move to time the reply by, or the input would not fit in a slot
// (MaxFrames): the moves come out whole or not at all.
inline bool Synthesize(const std::vector<std::string>& steps, bool facingRight, Compiled& compiled, std::string& error) {
    compiled = {};
    auto& out = compiled.frames;
    bool full = false, waited = false;
    const auto add = [&](training::Input input) {
        // Nothing is played before the first input: the reply starts there.
        if (out.empty() && !input.raw && !input.wait) return;
        if (out.size() < training::MaxFrames) out.push_back(input);
        else full = true;
        waited = waited || input.wait;
    };
    const auto clamp = [](int timing) { return static_cast<signed char>((std::max)(training::MinOffset, (std::min)(training::MaxOffset, timing))); };
    const auto push = [&](unsigned bits, int frames) { for (; frames > 0; --frames) add({bits, bits, 0, 0}); };
    const auto wait = [&](unsigned char on, unsigned held, int timing) { add({held, held, on, clamp(timing)}); };
    // A direction held until the next move begins ("[1]", ParseSteps puts a
    // move after it): that move's frames before its own input carry it, its
    // wait included. Held before the first move, it is a reply's leading hold
    // (WaitReply), given way when the move is due, the move's offset its own.
    unsigned pending = 0;
    // Whether a move has played, so a move now waits on it. Holds and waits
    // with a count before the first move are input leading into it, not a
    // move to recover from: the first move plays straight after them.
    bool follows = false;
    const auto fail = [&](std::string why) { compiled = {}; error = std::move(why); return false; };
    for (const auto& text : steps) {
        Step step;
        if (!ParseStep(text, step, error)) return fail(error);
        if (step.hold) {
            const unsigned held = DirectionBits(step.motion[0], facingRight);
            if (step.frames) push(held, step.frames);
            else pending = held;
            continue;
        }
        const int timing = step.offset;
        // A move laid out on a frame ("#45") starts its input so it takes
        // effect there, or at once when that frame has passed; it waits for no cue.
        const bool timed = step.at >= 0;
        if (timed && (out.empty() || waited || (pending && !follows)))
            return fail("\"" + text + "\" needs frames of a known length before it to land on frame " + std::to_string(step.at));
        // after: an earlier move played, so this one waits on it; idle: what
        // its frames before its own input hold.
        const bool after = follows, afterLead = pending && !after;
        const unsigned idle = after ? pending : 0;
        if (afterLead) { compiled.hold = static_cast<int>(out.size()); wait(training::WaitReply, pending, 0); }
        if (!after) compiled.offset = timing;
        pending = 0;
        const auto gap = [&](int frames) { push(idle, frames); };
        const unsigned buttons = ChosenButtons(step);
        const bool red = step.buttons == (LP | MP | MK) && step.need == 3;
        const bool fadc = (step.buttons == (MP | MK) && step.need == 2) || red;
        const bool focusDash = fadc && (step.motion == "66" || step.motion == "44") && !step.charge && !step.air;
        const bool link = after && !step.cancel && !timed;
        // The directions, as a player does them: a linked move holds its
        // first direction through the recovery before it and presses on the
        // free frame; a cancelled move does its motion during the move before
        // it, all but the last direction, and finishes it with the button as
        // the hit lands, so the motion is fresh.
        std::string motion = step.motion;
        if (motion == "360" || motion == "720") { motion.clear(); for (int turn = step.motion == "720" ? 2 : 1; turn > 0; --turn) motion += "63214789"; }
        // What the move waits on before its own input: the move before for a
        // link, the hit for a cancel (a focus cancel's comes before its
        // focus, any other's before its last direction), a few frames for a
        // follow-up, which goes into the move before it with no cue, far
        // enough in for the move to take it (El Fuerte's run takes its stop
        // from about its sixth frame, measured in play at this delay; "@"
        // moves it); a move on a frame waits for none.
        // A link's wait holds what comes before its input: a hold, or the
        // motion's first direction.
        const unsigned linkHeld = idle ? idle : motion.empty() ? 0 : DirectionBits(motion[0], facingRight);
        if (step.follow && after && !timed) gap((std::max)(0, FollowDelay + timing));
        if (focusDash) { if (after && !timed) wait(training::WaitHit, idle, timing); }
        else if (link) wait(training::WaitActionable, linkHeld, timing);
        else if (after) gap(1);
        // The move's own input, and the frame of it the move takes effect on
        // (Compiled::anchor), which a "#N" lands on and the first move's
        // schedule is timed by: laid out together, so the two agree.
        std::vector<training::Input> own;
        int anchor = -1;
        const auto put = [&](unsigned bits, int frames) { for (; frames > 0; --frames) own.push_back({bits, bits, 0, 0}); };
        const auto mark = [&] { anchor = static_cast<int>(own.size()); };
        if (focusDash) {
            // A focus cancel: focus as the hit lands, then the dash at once.
            // The red focus is tapped and let go so the attack itself comes
            // out and lands, and the dash cancels that hit.
            const unsigned dash = DirectionBits(step.motion == "44" ? '4' : '6', facingRight);
            mark(); put(step.buttons, 2); put(0, 2);
            if (red) own.push_back({0, 0, training::WaitHit, 0});
            put(dash, 2); put(0, 2); put(dash, 2);
        } else {
            // A jumping move takes effect on its jump.
            if (step.air) { mark(); put(DirectionBits('8', facingRight), 2); put(0, 10); }
            unsigned last = 0;
            // The wait held the motion's first direction, or a hold in its
            // place; a lone direction goes in with the button either way,
            // after a leading hold too.
            const bool lone = motion.size() == 1 && buttons;
            const bool firstHeld = (link && !idle) || ((link || afterLead) && lone);
            const bool finish = step.cancel && !step.follow && after && !timed && buttons && !motion.empty();
            int lastAt = static_cast<int>(own.size());
            for (std::size_t i = 0; i + (finish ? 1 : 0) < motion.size(); ++i) {
                // The same direction again is a new tap: a neutral frame between (a "66" dash).
                if (i && motion[i] == motion[i - 1]) put(0, 1);
                last = DirectionBits(motion[i], facingRight);
                lastAt = static_cast<int>(own.size());
                const bool held = firstHeld && i == 0;
                put(last, step.charge && i == 0 ? ChargeFrames : held ? 0 : step.cancel ? 2 : 3);
            }
            // A move with no button takes effect on its last direction; one
            // with a button, on it.
            if (!buttons) { if (!step.air) anchor = lastAt; }
            else {
                if (step.cancel && !step.follow && after && !timed) own.push_back({last, last, training::WaitHit, clamp(timing)});
                if (finish) last = DirectionBits(motion.back(), facingRight);
                if (!step.air) mark();
                if (step.edge == Edge::Hold) put(last | buttons, 30);
                else if (step.edge == Edge::Release) { put(last | buttons, 30); put(last, 2); }
                else if (step.mash) {
                    // Five presses: several buttons are cycled one per frame (a piano),
                    // one button goes on and off a frame at a time.
                    std::vector<Step::MashPress> cycle = step.mashOrder;
                    const int presses = cycle.empty() ? 5 : static_cast<int>(cycle.size());
                    if (cycle.empty()) {
                        std::vector<unsigned> bits;
                        if (step.buttons == Punches) bits = {LP, MP, HP};
                        else if (step.buttons == Kicks) bits = {LK, MK, HK};
                        else for (unsigned bit = 1; bit; bit <<= 1) if (step.buttons & bit) bits.push_back(bit);
                        for (unsigned bit : bits) cycle.push_back({'0', bit});
                    }
                    unsigned previous = 0;
                    for (int press = 0; press < presses; ++press) {
                        const auto& one = cycle[press % cycle.size()];
                        const unsigned direction = one.direction != '0' ? DirectionBits(one.direction, facingRight) : last;
                        // The same button twice in a row needs a frame off between, to be a new press.
                        if (one.button == previous) put(direction, 1);
                        put(direction | one.button, 1); previous = one.button;
                    }
                }
                else put(last | buttons, 1);
                // The button comes up before the next move, so its press is its own edge.
                put(last, 1);
            }
        }
        // A move takes effect on input: one that presses nothing there (a
        // neutral "5") is no move, to time the reply by or to recover from,
        // wherever it stands, so the moves are refused whole. A linked lone
        // direction is its wait's: its own input has none after it.
        const unsigned takes = anchor < 0 ? 0u : anchor < static_cast<int>(own.size()) ? own[anchor].raw : link ? linkHeld : 0u;
        if (!takes) return fail("\"" + text + "\" presses nothing: a move needs a direction or a button");
        follows = true;
        // On a frame, the move's anchor lands there ("@N" from it, but on the
        // first move, whose "@N" is the reply's own), the frames before it
        // held as a move's frames before its own input hold.
        if (timed) gap(step.at + (after ? timing : 0) - anchor - static_cast<int>(out.size()));
        for (int i = 0; i < static_cast<int>(own.size()); ++i) {
            if (i == anchor && !after) compiled.anchor = static_cast<int>(out.size());
            add(own[i]);
        }
    }
    push(0, 1);
    if (full) return fail("the moves take more than " + std::to_string(training::MaxFrames) + " frames");
    // Holds and waits with no move after them lead into nothing the reply
    // could time.
    if (!steps.empty() && compiled.anchor < 0) return fail("a reply needs a move, not only holds and waits");
    return true;
}
} }
