#pragma once
#include "ComboBook.hxx"
#include "TrainingSession.hxx"
#include <vector>

// Turns a combo's steps into pad input the training session can play back,
// so a typed combo can be watched. Directions take a few frames each and a
// button two; between moves the input waits on the fight itself: a linked
// move for the fighter to be able to act again (plus the link gap), a
// cancelled move for the hit to land. The game decides what comes out.
namespace sf4e { namespace combo {
constexpr unsigned Up = 1, Down = 2, Left = 4, Right = 8;
constexpr int FollowDelay = 10;
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
// Directions are read for the facing; the buttons of a step are chosen once.
// offset: frames every move goes on waiting after its cue, added to the
// move's own "@" offset.
inline std::vector<training::Input> Synthesize(const std::vector<std::string>& steps, bool facingRight, int offset) {
    std::vector<training::Input> out;
    const auto push = [&](unsigned bits, int frames) { for (; frames > 0 && out.size() < training::MaxFrames; --frames) out.push_back({bits, bits, 0, 0}); };
    const auto wait = [&](unsigned char on, unsigned held, int timing) {
        if (out.size() < training::MaxFrames) out.push_back({held, held, on, static_cast<signed char>((std::max)(training::MinOffset, (std::min)(training::MaxOffset, timing)))});
    };
    for (std::size_t index = 0; index < steps.size(); ++index) {
        Step step; std::string error;
        if (!ParseStep(steps[index], step, error)) continue;
        const int timing = step.offset + offset;
        const unsigned buttons = ChosenButtons(step);
        // A move laid out on a frame ("#45") starts its input so the press
        // lands there, or at once when that frame has passed; it waits for no cue.
        const bool timed = step.at >= 0;
        const auto reach = [&](int frame) { push(0, frame - static_cast<int>(out.size())); };
        const bool fadc = (step.buttons == (MP | MK) && step.need == 2) || (step.buttons == (LP | MP | MK) && step.need == 3);
        if (fadc && (step.motion == "66" || step.motion == "44") && !step.charge && !step.air) {
            // A focus cancel, red or not: focus as the hit lands, then the dash.
            const unsigned dash = DirectionBits(step.motion == "44" ? '4' : '6', facingRight);
            if (timed) reach(step.at);
            else if (index) wait(training::WaitHit, 0, timing);
            push(step.buttons, 2); push(0, 2); push(dash, 2); push(0, 2); push(dash, 2);
            continue;
        }
        // The directions, as a player does them: a linked move holds its
        // first direction through the recovery before it and presses on the
        // free frame; a cancelled move does its motion during the move before
        // it, all but the last direction, and finishes it with the button as
        // the hit lands, so the motion is fresh.
        std::string motion = step.motion;
        if (motion == "360" || motion == "720") { motion.clear(); for (int turn = step.motion == "720" ? 2 : 1; turn > 0; --turn) motion += "63214789"; }
        if (step.air) { push(DirectionBits('8', facingRight), 2); push(0, 10); }
        unsigned last = 0;
        const bool link = index && !step.cancel && !timed;
        // A follow-up goes into the move before it with no cue, far enough in
        // for the move to take it: El Fuerte's run takes its stop from about
        // its sixth frame, measured in play at this delay. "@" moves it.
        if (step.follow && index && !timed) push(0, (std::max)(0, FollowDelay + timing));
        if (timed) {
            int lead = 0;
            for (std::size_t i = 0; i < motion.size(); ++i) lead += (step.charge && i == 0 ? 50 : step.cancel ? 2 : 3) + (i && motion[i] == motion[i - 1] ? 1 : 0);
            if (index) push(0, 1);
            reach(step.at - lead);
        } else if (link) wait(training::WaitActionable, motion.empty() ? 0 : DirectionBits(motion[0], facingRight), timing);
        else if (index) push(0, 1);
        const bool finish = step.cancel && !step.follow && index && !timed && buttons && !motion.empty();
        for (std::size_t i = 0; i + (finish ? 1 : 0) < motion.size(); ++i) {
            // The same direction again is a new tap: a neutral frame between (a "66" dash).
            if (i && motion[i] == motion[i - 1]) push(0, 1);
            last = DirectionBits(motion[i], facingRight);
            const bool held = link && i == 0;
            push(last, step.charge && i == 0 ? 50 : held ? 0 : step.cancel ? 2 : 3);
        }
        if (!buttons) continue;
        if (step.cancel && !step.follow && index && !timed) wait(training::WaitHit, last, timing);
        if (finish) last = DirectionBits(motion.back(), facingRight);
        if (step.edge == Edge::Hold) push(last | buttons, 30);
        else if (step.edge == Edge::Release) { push(last | buttons, 30); push(last, 2); }
        else if (step.mash) for (int press = 0; press < 8; ++press) { push(last | buttons, 2); push(last, 2); }
        else push(last | buttons, 1);
        // The button comes up before the next move, so its press is its own edge.
        push(last, 1);
    }
    push(0, 1);
    return out;
}
} }
