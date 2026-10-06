#pragma once
#include "BcmFile.hxx"
#include "ComboBook.hxx"

// Joins combo notation to a fighter's command file: which moves a typed step
// may start, and how a move is typed.
namespace sf4e { namespace combo {
namespace detail {
inline int Count(unsigned bits) { int n = 0; for (; bits; bits &= bits - 1) ++n; return n; }
// One typed direction against a mask, the way the game tests it.
inline bool Fits(char digit, unsigned mask, unsigned how) {
    const unsigned have = bcm::DirectionMask(digit);
    return how == 1 ? (have & mask & bcm::Directions) != 0 : how == 2 ? have == (mask & bcm::Directions) : true;
}
// The typed directions against a move's motion: each stored step takes one
// typed direction, in order, the first step the first and the last the last.
// "41236" so fits a half circle stored as back, down, forward.
inline bool MotionFits(const bcm::File& file, const bcm::Move& move, char held, const std::string& typed) {
    static const bcm::Motion none;
    const bcm::Motion& all = move.motionIndex >= 0 ? file.motions[static_cast<std::size_t>(move.motionIndex)] : none;
    std::size_t from = 0;
    const bcm::Charge* charge = nullptr;
    if (!all.empty() && all[0].type == bcm::ChargeStep) {
        if (all[0].input >= file.charges.size()) return false;
        // A charged button is not typed as a direction.
        if (!(file.charges[all[0].input].input & bcm::Buttons)) charge = &file.charges[all[0].input];
        from = 1;
    }
    if ((charge != nullptr) != (held != 0)) return false;
    if (charge) {
        const unsigned want = charge->input & bcm::Directions, have = bcm::DirectionMask(held);
        if ((charge->flags & 0xF) == 2 ? (have & want) != want : !(have & want)) return false;
    }
    bcm::Motion steps;
    for (std::size_t i = from; i < all.size(); ++i) {
        // A full turn is typed as "360", never as directions.
        if (all[i].type == bcm::Rotation) return false;
        if (all[i].type != bcm::Direction || !(all[i].match & 0xF)) continue;
        if (all[i].input & bcm::Buttons) return false;
        steps.push_back(all[i]);
    }
    // Starting from neutral goes without saying.
    if (!steps.empty() && (steps[0].input & bcm::Directions) == bcm::Neutral) steps.erase(steps.begin());
    if (steps.empty() || typed.empty()) return steps.empty() && typed.empty();
    const std::size_t last = steps.size() - 1, end = typed.size() - 1;
    if (typed.size() < steps.size() || !Fits(typed[0], steps[0].input, steps[0].match & 0xF)) return false;
    if (!last) return !end;
    if (!Fits(typed[end], steps[last].input, steps[last].match & 0xF)) return false;
    std::size_t at = 1;
    for (std::size_t k = 1; k < last; ++k, ++at) {
        while (at < end && !Fits(typed[at], steps[k].input, steps[k].match & 0xF)) ++at;
        if (at >= end) return false;
    }
    return true;
}
}

// Would the input a step writes satisfy this move's command? Only the
// command is judged: stick, buttons, edge, mash, air or ground, close or far.
// Meter, the selected ultra, stance, distance and what the move may cancel
// from are the game's to decide, so a step can satisfy several moves:
//   "236P" the three Hadoken strengths, "5LP" the close and the far jab,
//   "236236LP" the super and, without meter, the Hadoken inside it,
//   "j.214K" the air move and a ground one the file allows in any state.
// Of the moves it allows the game starts the one latest in File::moves.
// Never satisfied: FADC (two inputs: judge "MP+MK" and "66" apart), and a
// lone direction other than a dash (walking and jumping are not moves in the
// file; their action ids are the same for every fighter).
inline bool Satisfies(const bcm::File& file, const bcm::Move& move, const Step& step) {
    using namespace detail;
    if (move.motionIndex >= static_cast<std::int32_t>(file.motions.size())) return false;
    if (step.mash != move.mash) return false;
    if (move.state && !(move.state & (step.air ? bcm::Airborne : bcm::Standing | bcm::Crouching))) return false;
    if (step.range != Range::Any && move.position != (step.range == Range::Close ? bcm::CloseOnly : bcm::FarOnly)) return false;

    if (!step.buttons != !move.buttons) return false;
    if (step.buttons) {
        const int hit = Count(step.buttons & move.buttons), all = Count(move.buttons);
        const unsigned test = move.flags & 0xF0u;
        // Any one of the move's buttons: one typed button, or the whole mask
        // by name ("]MP+MK[" for a focus release that takes either).
        if (test == 0x10) { if (step.need == 1 ? !hit : step.buttons != move.buttons || step.need != all) return false; }
        // All of them: by name, or "P" for a super that has one move per strength.
        else if (test == 0x20) { if (step.need == Count(step.buttons) ? step.buttons != move.buttons : step.need != 1 || !hit || all != 1) return false; }
        else if (test == 0x50) { if (step.need < 2 || hit < 2) return false; }
        else return false;
        if (step.edge == Edge::Release ? !move.release : !move.press) return false;
    }

    std::string typed = step.motion == "5" ? "" : step.motion;
    char held = 0;
    if (step.charge) {
        if (typed.empty()) return false;
        held = typed[0]; typed.erase(0, 1);
    }
    const unsigned how = move.flags & 0xFu, stick = move.input & bcm::Directions;
    const bool own = (how == 1 || how == 2) && stick > bcm::Neutral;
    if (typed == "360" || typed == "720") {
        if (move.motionIndex < 0 || held || own) return false;
        const bcm::Motion& steps = file.motions[static_cast<std::size_t>(move.motionIndex)];
        return !steps.empty() && steps[0].type == bcm::Rotation && (steps[0].input <= 1) == (typed == "360");
    }
    if (typed.size() > 16) return false;
    // The same direction twice means the stick went back to neutral between.
    std::string seq;
    for (char digit : typed) {
        if (!bcm::DirectionMask(digit)) return false;
        if (!seq.empty() && seq.back() == digit) seq += '5';
        seq += digit;
    }
    if (!own) return MotionFits(file, move, held, seq);
    // The move's own direction is the last one typed; a motion before it may
    // or may not end on it.
    if (seq.empty() || !Fits(seq.back(), stick, how)) return false;
    return MotionFits(file, move, held, seq.substr(0, seq.size() - 1)) || (move.motionIndex >= 0 && MotionFits(file, move, held, seq));
}

// How a move is typed. False when the notation cannot say it; see
// bcm::Move::spelled. A standing normal that has a close and a far version
// comes back as "cl." or "far.".
inline bool MoveStep(const bcm::Move& move, Step& step) {
    step = Step{};
    if (!move.spelled) return false;
    step.motion = move.motion; step.charge = move.charge; step.air = move.air; step.mash = move.mash;
    step.buttons = move.buttons;
    // Canonical spells need only for "P" and "K", so named buttons count all.
    step.need = move.buttons == Punches || move.buttons == Kicks ? move.need : detail::Count(move.buttons);
    if (!move.press && move.release) step.edge = Edge::Release;
    if (step.motion.empty() && !step.air) {
        step.motion = "5";
        if (move.position == bcm::CloseOnly) step.range = Range::Close;
        if (move.position == bcm::FarOnly) step.range = Range::Far;
    }
    return true;
}
// "236LP", "[4]6LP", "j.214KK", "cl.HP"; empty when the notation cannot say it.
inline std::string MoveNotation(const bcm::Move& move) {
    Step step;
    return MoveStep(move, step) ? Canonical(step) : std::string();
}
// The step for a move the game started, by its script index as the training
// session samples it: the most specific spelled move of the file that runs
// that script, a named button ("236HP") before any-button masks ("236P").
// Empty when no move does (a walk, a jump, a follow-up the notation cannot say).
inline std::string ActionStep(const bcm::File& file, int script, bool cancel) {
    std::string best; int bestScore = 1 << 30;
    for (const auto& move : file.moves) {
        Step step;
        if (move.script != script || !MoveStep(move, step)) continue;
        const int score = step.buttons == Punches || step.buttons == Kicks ? 100 + step.need : detail::Count(step.buttons);
        if (score >= bestScore) continue;
        step.cancel = cancel;
        best = Canonical(step); bestScore = score;
    }
    return best;
}
} }
