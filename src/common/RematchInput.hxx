#pragma once
#include <cstdint>

// What each fighter's post-match prompt says over the WIN of a game that may
// go on in place, carried to both games in the fighters' GGPO inputs, and what
// the WIN does with it. Both games read the same inputs on the same frame, so
// both make the same choice, in a frame of the present or its re-simulation.
//
// The WIN ends by itself when its last wait runs out (MATCH_RESULT step 5,
// 300 frames, at 0x5DDDC0; DRAW_RESULT step 2, 30 frames), or at once when
// either fighter presses a button in it (0x5DD100). That can come before a
// fighter has answered the prompt: the whole MATCH_RESULT is a wait for the
// result's animation and then 30 + 30 + 160 + 300 frames, about as long as
// the prompt's ten seconds, which open later. An answer that lands after it,
// or a frame a game predicted without it, leaves the battle instead of going
// on to the rematch both chose. So while either fighter is still asked, the
// WIN waits for them.
namespace sf4e { namespace rematch {
// Rematch chosen (one of TrainingRuntime.hxx's retired bits).
constexpr unsigned ChoiceBit = 0x20000000;
// The prompt is open and has no answer yet (the other retired bit).
constexpr unsigned AskingBit = 0x40000000;
constexpr unsigned Bits = ChoiceBit | AskingBit;
// How long a fighter counts as asked once the prompt is due: the prompt's ten
// seconds, and a second for a last-moment answer to reach the runtime.
constexpr std::uint64_t AskingMs = 11000;

// This fighter's bits over such a WIN: Rematch chosen, or still being asked,
// `dueForMs` after the prompt became due. None once the prompt's time is up.
// The runtime does not hear Back to room or Character select: those keep the
// WIN no longer than the prompt's time, which it mostly lasts anyway.
inline unsigned LocalBits(bool due, bool chosen, std::uint64_t dueForMs) {
    if (!due) return 0;
    if (chosen) return ChoiceBit;
    return dueForMs < AskingMs ? AskingBit : 0;
}

// What the WIN does this frame with both fighters' raw inputs. End: both chose
// Rematch, so its last wait ends now. Hold: one is still being asked, so its
// last wait does not run out and no press cuts it short. Run: as the game has it.
enum class Win { Run, Hold, End };
inline Win Decide(unsigned p1, unsigned p2) {
    if ((p1 & ChoiceBit) && (p2 & ChoiceBit)) return Win::End;
    return (p1 | p2) & AskingBit ? Win::Hold : Win::Run;
}
} }
