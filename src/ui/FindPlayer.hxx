#pragma once
// Find a player (docs/design/PROFILE.md, "Find a player"): the page of the Friends tab where a name or a friend code is
// typed and searched, and where the players this PC met lately are listed. What the page lists and what a row opens is
// decided here by plain functions that need no frame (friends_panel_test.cxx); FriendsPanel owns the page's text and sends
// the search, and draws nothing of its own beyond what DrawPick puts on a list row.
#include "MenuNavigation.hxx"
#include "PlayerOpening.hxx"
#include "../netplay/ProfileRequests.hxx"
#include "../netplay/Profiles.hxx"
#include <imgui.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;

namespace find_player {
namespace profiles = netplay::profiles;

// The rows a result and a recent opponent have: the prefix and the place in the list.
constexpr const char* HitPrefix = "find-player:";
constexpr const char* RecentPrefix = "find-recent:";
constexpr const char* SearchRow = "friends-search";
// The text takes as many bytes as a name has; a name is searched from LeastName.
constexpr std::size_t MaxText = profiles::MaxNameBytes, LeastName = 2;
// The one line that says how the search stands, or that nobody has been met.
constexpr const char* FindNoteRow = "friends-find-note";

// The text without the spaces and tabs at its ends.
std::string Trimmed(const std::string& text);
// Whether `typed` is a whole friend code: eight letters and numbers, with dashes and spaces among them and nothing else.
// The bridge reads a code that way (ember_protocol::friends::parse_friend_code), so the page offers Send request for it
// and searches it as a code.
bool WholeCode(const std::string& typed);

// What Search asks for from the text typed: a name, or a friend code. `ok` false: nothing, and `why` is the catalog id of
// the words that say why (a name under LeastName characters; no search by name on a service without profiles).
struct Ask {
    bool ok = false, code = false;
    std::string text;
    const char* why = nullptr;
};
Ask AskOf(const std::string& typed, profiles::Service offering);

// What the page lists: the text typed, the search as the view holds it, and the players met lately (newest first), at `now`
// (unix seconds).
struct Page {
    std::string typed;
    profiles::Service offering = profiles::Service::Unknown;
    const profiles::SearchView* search = nullptr;
    const std::vector<profiles::Recent>* recent = nullptr;
    std::uint64_t now = 0;
};
// The search's answer when it is the one for the text typed (the same text, asked the same way), else null: an answer for
// other text is not shown under this.
const profiles::SearchAnswer* AnswerOf(const Page& page);
// The search row: Primary, open from LeastName characters.
MenuEntry SearchEntry(const Page& page);
// The rows under Search and Send request: how the search stands, the players it found, the players met lately.
std::vector<MenuEntry> ResultRows(const Page& page);
// Who a row of ResultRows opens, none for any other row.
std::optional<player_opening::Opening> OpeningOfRow(const Page& page, const std::string& row);

// What a list row of players draws: their picture, flag, class and name, and the words under them. Shared with the pick
// list of a name several players go by (PlayerPage).
struct Pick {
    std::string name, detail;
    CountryCode country;
    SkillBadge badge;
    // The native fighter id shown, -1 for none.
    int fighter = -1;
    bool characterArt = false;
};
void DrawPick(ImDrawList* d, const Pick& pick, ImVec2 min, ImVec2 max);
// The Pick a row of ResultRows draws, none for any other row.
std::optional<Pick> PickOfRow(const Page& page, const std::string& row, bool characterArt);
}

} }
