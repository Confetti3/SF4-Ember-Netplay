#pragma once
// The post-match prompt's own types (ApplicationShell__PostMatch.cxx): what it
// remembers of the game it follows, what it shows each frame, and the panel
// that frame is laid out in before it is drawn. The shell keeps one prompt
// (ApplicationShell::postMatch_) and draws it on its own screen, "post-match".
#include "MenuNavigation.hxx"
#include "../session/RoomModel.hxx"
#include <imgui.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace ui {

// What Back to room, Character select or no answer leaves to do once the
// game is over and the room takes it: leave the seat, or open fighter select.
enum class AfterPostMatch { None, LeaveSeat, FighterSelect };

// The prompt opened over the WIN screen (ShellView::matchResult). It keeps the
// table and the opponent of the game it follows, and the generation of that
// game: a new one at the table ends it, unless both fighters chose Rematch,
// when it stays until that game is loading.
struct PostMatchPrompt {
    int table = -1;
    room::MemberId opponent = 0;
    std::uint64_t generation = 0;
    // The game's result, once the runtime has it (FoughtGame::result).
    room::MatchResult result = room::MatchResult::Abort;
    double until = 0;
    bool chose = false, starting = false;
    // An Arcade pair's Rematch chosen here (Table::deciding): once both
    // chose it the room asks no more, and the prompt readies the player.
    bool pairRematch = false;
    // The runtime's Ready failures seen so far: a new one takes back the
    // choice, but the player still answered (no seat left for no answer).
    std::uint64_t failures = 0;
    bool answered = false;
    // Back to room or Character select over the WIN: the prompt shows the
    // choice until the game is over, then does `after`.
    bool left = false;
    AfterPostMatch after = AfterPostMatch::None;
    // The room's record of the set this game ended, as first seen: whether
    // the pair was still seated together then (they stayed to decide, or play
    // on). A pair that stayed and was parted later was parted by a New
    // opponent, not by the set's rotation, so no rotation words apply.
    bool setSeen = false, pairStayed = false;
};

// The prompt's focused button last frame, where it stood in the panel's
// content (not scrolled) and the panel's size: a change scrolls the button
// into view.
struct PostMatchBarView {
    std::string focus;
    float top = 0, bottom = 0, panelWidth = 0, panelHeight = 0;
    bool operator==(const PostMatchBarView& o) const {
        return focus == o.focus && top == o.top && bottom == o.bottom && panelWidth == o.panelWidth && panelHeight == o.panelHeight;
    }
};

// What the prompt shows this frame (ApplicationShell::BuildPostMatch): its
// title and eyebrow, the two fighters' answers, its rows (the buttons, then the
// Ultra) and the rules they were built by. Not open without a prompt.
struct PostMatchModel {
    bool open = false;
    int mine = -1, theirs = -1;
    bool chosen = false, theyChose = false, again = false, ultraOpen = false, newOpponentOpen = false;
    // Over the WIN with this PC's Ready given: Back and Character select are not offered.
    bool committed = false;
    const room::Member* local = nullptr;
    const room::Member* opponent = nullptr;
    std::string title, eyebrow, mineWord, theirWord;
    std::vector<MenuEntry> rows;
    std::size_t buttons = 0;
};

// The prompt's body laid out `width` wide: the two players' rows, the Ultra
// row under them, room for the longest detail, and the buttons' bar on as few
// lines as their labels let it (each button's need). height is all of it, in
// whole pixels.
struct PostMatchBody { float width = 0; std::vector<float> needs; int lines = 1; float words = 0, barHeight = 0, height = 0; };
PostMatchBody LayOutPostMatch(const std::vector<MenuEntry>& entries, std::size_t buttons, float width);

// The prompt's panel for a model: as tall as all its content and the menu's
// frame around it (GameMenu::ChromeHeight), capped by the screen's height, in
// whole pixels. body is its content laid out at the panel's width; fits says
// the cap left all of it in sight, so nothing scrolls.
struct PostMatchPanel { ImVec2 size; PostMatchBody body; bool fits = false; };
PostMatchPanel PostMatchPanelFor(const PostMatchModel& model);

// One frame of the prompt, built once (ApplicationShell::Draw sizes the window
// by it, DrawPostMatch draws it).
struct PostMatchFrame { PostMatchModel model; PostMatchPanel panel; };

} }
