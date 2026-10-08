#pragma once
#include <cstdint>

// The shared save and reset of a table under the Training rule. A press is
// part of a player's input, so both sides and every spectator see it on the
// same frame, confirmed and in order, and a mispredicted one is rolled back
// as any input is. What is decided here is a pure function of the frame's
// inputs and of a state that is saved and restored with the frame.
namespace sf4e { namespace training {
// Two bits of the pad's raw word that the game is never shown: they are put
// in before the input goes to GGPO and taken out before it reaches the game.
constexpr unsigned PracticeReset = 0x20000000, PracticeSave = 0x40000000, PracticeMask = PracticeReset | PracticeSave;
enum class PracticeStep : std::uint8_t { None, Save, Reset };
struct PracticeState {
    // Which of the four presses were down in the frame before: Player 1's
    // reset and save, then Player 2's. A press counts as it goes down. A
    // predicted input repeats the last one received, so a prediction never
    // makes a press that was not there.
    std::uint8_t held = 0;
    // The round was being fought in the frame before.
    bool fight = false;
    // The last step taken, by whom (-1 the round's own start), and how many
    // were: what the players are told.
    PracticeStep last = PracticeStep::None;
    std::int8_t by = -1;
    std::uint16_t count = 0;
};
// What this frame does before the game plays it. The position is saved as the
// round's fight begins, so a reset always has one to go back to. A save and a
// reset in one frame is a save: the reset would load what was just saved.
inline PracticeStep DecidePractice(PracticeState& state, unsigned padOne, unsigned padTwo, bool fight) {
    const auto bits = [](unsigned pad) { return static_cast<std::uint8_t>((pad & PracticeReset ? 1 : 0) | (pad & PracticeSave ? 2 : 0)); };
    const auto now = static_cast<std::uint8_t>(bits(padOne) | bits(padTwo) << 2);
    const auto pressed = static_cast<std::uint8_t>(now & ~state.held);
    const bool began = fight && !state.fight;
    state.held = now; state.fight = fight;
    if (!fight) return PracticeStep::None;
    PracticeStep step = PracticeStep::None;
    int by = -1;
    if (began) step = PracticeStep::Save;
    else if (pressed & 0xA) { step = PracticeStep::Save; by = pressed & 0x2 ? 0 : 1; }
    else if (pressed & 0x5) { step = PracticeStep::Reset; by = pressed & 0x1 ? 0 : 1; }
    if (step == PracticeStep::None) return step;
    state.last = step; state.by = static_cast<std::int8_t>(by); ++state.count;
    return step;
}
} }
