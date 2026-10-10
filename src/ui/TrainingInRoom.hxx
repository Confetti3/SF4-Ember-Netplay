#pragma once
// What a member who waits in Training sees of their room over the battle: a
// line saying where they stand there, and on the call back (TRAINING_IN_ROOMS.md)
// who called and the go now prompt. All of it reads the room snapshot the
// overlay already has; nothing here sends anything.
#include "../common/PadKind.hxx"
#include "../common/TrainingCallInput.hxx"
#include "../session/RoomModel.hxx"
#include <cstdint>
#include <string>

namespace sf4e { namespace ui {
// The room's name, then "Table 2", the member's place there and the unread
// chat, in that order. Their place is either the opponent seated opposite,
// by name ("Opponent: {0}"), or Ember's own words ("Queued 2 of 3", "Waiting
// for an opponent", "Not queued"). The room's and the opponent's names are
// players' text and are kept apart, so only they are shortened to fit
// (TrainingRoomLine) and Ember's words never are.
struct TrainingRoomStatus {
    std::string room, opponent;
    std::string table, place, unread;
    bool Empty() const { return room.empty() && opponent.empty() && table.empty() && place.empty() && unread.empty(); }
};
// Empty without a joined room or a local member in it. unread: the room
// chat's unread count, as the room screen shows it.
TrainingRoomStatus DescribeTrainingRoom(const room::Snapshot& room, unsigned unread);
// The line with the two names as given, already fitted: its parts joined in
// order with a middle dot.
std::string TrainingRoomLine(const TrainingRoomStatus& status, const std::string& room, const std::string& opponent);

// The challenger banner's call and its extras: whether the call stands for
// this battle (input::CallIdentity), who sat down opposite by name and fighter
// (-1 when not shared), and the go now prompt's glyph (MenuGlyphs), null when
// it cannot be pressed.
struct ChallengerCall {
    bool called = false;
    std::string opponent;
    int fighter = -1;
    const char* goNowGlyph = nullptr;
};
// The call that stands: its opponent as the room knows them, whoever sits
// opposite now. Not called without a live call.
ChallengerCall DescribeChallenger(const room::Snapshot& room, const input::CallIdentity& call);

// Go now on the Training call: the banner can be cut short instead of waited
// out. Enter on the keyboard, or View on an Xbox pad: the button that resets
// the position otherwise, which the pad's gesture then takes for go now alone
// (common/TrainingPad.hxx). A DirectInput pad's buttons have no prompt art,
// so it is offered Enter. promptDevice: the device the training HUD's prompts
// show (GameMenu.hxx: MenuPromptDevice), so the two always agree.
inline const char* GoNowGlyph(int promptDevice) { return promptDevice == input::PadXInput ? "View" : "Enter"; }
} }
