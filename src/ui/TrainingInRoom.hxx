#pragma once
// What a member who waits in Training sees of their room over the battle: a
// line saying where they stand there, and on the call back (TRAINING_IN_ROOMS.md)
// who called and the go now prompt. All of it reads the room snapshot the
// overlay already has; nothing here sends anything.
#include "../common/PadKind.hxx"
#include "../session/RoomModel.hxx"
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// The room's name, then "Table 2", the member's place there ("Queued 2 of 3",
// "Waiting for an opponent") and the unread chat, in that order. The name is
// the player's text and is kept apart so it alone is shortened to fit.
struct TrainingRoomStatus {
    std::string room;
    std::vector<std::string> parts;
    bool Empty() const { return room.empty() && parts.empty(); }
};
// Empty without a joined room or a local member in it. unread: the room
// chat's unread count, as the room screen shows it.
TrainingRoomStatus DescribeTrainingRoom(const room::Snapshot& room, unsigned unread);

// The challenger banner's extras: who sat down opposite, by name and fighter
// (-1 when not shared), and the go now prompt's glyph (MenuGlyphs), null when
// it cannot be pressed.
struct ChallengerCall {
    std::string opponent;
    int fighter = -1;
    const char* goNowGlyph = nullptr;
};
// The member opposite the local one at their table, or nobody.
ChallengerCall DescribeChallenger(const room::Snapshot& room);

// Go now on the Training call: the banner can be cut short instead of waited
// out. Enter on the keyboard, or View on an Xbox pad: the button that resets
// the position otherwise, which the pad's gesture then takes for go now alone
// (common/TrainingPad.hxx). A DirectInput pad's buttons have no prompt art,
// so it is offered Enter. promptDevice: the device the training HUD's prompts
// show (GameMenu.hxx: MenuPromptDevice), so the two always agree.
inline const char* GoNowGlyph(int promptDevice) { return promptDevice == input::PadXInput ? "View" : "Enter"; }
} }
