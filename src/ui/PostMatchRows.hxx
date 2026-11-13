#pragma once
// The two players' rows on the post-match prompt (docs/design/PROFILE.md, "After a set: the post-match prompt"): the fighter
// each played in a picture, then the player's tag, then their answer as a chip standing right after the name it describes.
// The opponent's row ends at its right edge in the Add friend box, the row's full height; a flat one (Request sent, Friends)
// is drawn here, a live one is the caller's to press (PostMatchFriend.hxx).
#include "PostMatchFriend.hxx"
#include "V2.hxx"
#include "../session/RoomTypes.hxx"
#include <imgui.h>
#include <optional>
#include <string>

namespace sf4e { namespace ui {
struct ShellView;

namespace post_match_rows {
// A player's row is this tall (logical px), a picture and a line of words, and the Add friend box is this wide at most.
constexpr float RowHeight = 64, ButtonWidth = 176;

struct Player {
    // The id the text probe is told the answer under.
    const char* id = "";
    const room::Member* member = nullptr;
    // The answer shown: ready (they chose Rematch), seated (still at the table), `left` (this player went back to the room or
    // to Character select: the chip is muted, being no answer to wait on), and its words.
    bool ready = false, seated = false, left = false;
    std::string word;
};

// The box the Add friend button takes at the right edge of `row`.
v2::Box ButtonBox(v2::Box row);
// Draws `player`'s row in `row`. For the opponent `edge` is the box at the row's right edge: a flat state is drawn in it and
// nothing is returned; a live one is left for the caller, which gets the box to draw and press.
std::optional<v2::Box> DrawPlayer(const ShellView& view, ImDrawList* draw, v2::Box row, const Player& player, post_match_friend::State edge);
}

} }
