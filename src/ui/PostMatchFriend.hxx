#pragma once
// Add friend on the post-match prompt (docs/design/PROFILE.md, "After a set: the post-match prompt"): the opponent's row
// carries a button at its right edge that asks them to be friends, so a rematch is not where the chance to add them is
// lost. What the box says, and whether it can be pressed, is decided here from how the opponent stands with the player;
// the shell sends the request through the friends panel's member path (gated, logged and answered in words), and the
// prompt's answer to the rematch, its countdown and its focus are none of this button's business.
#include "MenuNavigation.hxx"
#include "../netplay/Levels.hxx"
#include "../session/RoomTypes.hxx"
#include <cstdint>
#include <optional>
#include <string>

namespace sf4e { namespace ui {
struct ShellView;

namespace post_match_friend {
// The id of the row the live button is (Up from the buttons' bar reaches it, A presses it), and of the box's text for the
// text probe.
constexpr const char* RowId = "pm-add-friend";

// What the box at the opponent's right edge says.
//   None     nothing: no card to ask by (a private room, a player with no Discord account, a card not read yet), or the
//            opponent blocked this player or was blocked by them.
//   Add      "Add friend": a button.
//   Accept   "Accept request": a button (they asked first).
//   Sent     "Request sent": a flat box, also right after pressing Add friend.
//   Friends  "Friends": a flat box, also right after accepting.
struct State {
    enum class Kind : std::uint8_t { None, Add, Accept, Sent, Friends } kind = Kind::None;
    // The button can be pressed: it has a row, the View glyph and a hint. The flat boxes cannot be focused.
    bool Live() const { return kind == Kind::Add || kind == Kind::Accept; }
    bool Shown() const { return kind != Kind::None; }
};

struct Inputs {
    // The opponent has a card token and their card was read: there is a player to ask.
    bool card = false;
    netplay::levels::Relation relation = netplay::levels::Relation::None;
    // The button was pressed in this prompt, and no refusal has come since: the request is in flight or answered, whatever
    // the card still says.
    bool asked = false;
};
State StateOf(const Inputs& inputs);
// The inputs for `opponent`, a member of the room the prompt is in (null: gone). A member's card is the one the room's
// look-ups read (ShellView::memberCards), so the relation is as fresh as the Members list shows it.
Inputs InputsOf(const ShellView& view, const room::Member* opponent, bool asked);

// The catalog ids of the box's word and of the focused button's detail; null for a state with none.
const char* LabelOf(State::Kind kind);
const char* DetailOf(State::Kind kind);
// The row a state lists: only a live button is one, at the foot of the players' rows after the buttons' bar (a grid footer,
// MenuEntry::wide). Its detail names the opponent.
std::optional<MenuEntry> RowOf(State state, const std::string& opponentName);
}

} }
