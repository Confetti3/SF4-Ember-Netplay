#pragma once
// The News list's sample releases, shared by the UI lab (--screen news) and the render test, so both draw the text
// News really gets: five releases as GitHub sends their bodies, each cut at the update check's 2,000 bytes and made
// plain text (github_release_client.cxx, ReadRelease), with headings, lists, a link, a long line and Japanese.
// --news stress adds a release whose title is far longer than the pane, whose tag is long and whose notes hold a long
// word with no space in it.
#include "../ShellView.hxx"
#include "../../common/ReleaseNotesText.hxx"
#include <string>
#include <utility>
#include <vector>

namespace uilab {
namespace news_detail {
// A release body in this project's own shape: a lead, the attribution line, how to update, the changes, a thanks.
inline std::string Body(const std::string& version, const std::string& lead, const std::string& changes) {
    return "# SF4 Ember Netplay v" + version + "\r\n\r\n" + lead + "\r\n\r\n"
        "Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by its original authors](https://example.invalid/sf4e). "
        "Preserve upstream attribution and bundled licenses.\r\n\r\n"
        "## Updating\r\n\r\n"
        "- **From the last release, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.\r\n"
        "- **The zip and setup.exe are on the [release page](https://example.invalid/releases/v" + version + ")** with their `.sha256` files.\r\n"
        "- **Everyone in a room needs " + version + ",** spectators included.\r\n\r\n"
        "## Known issues\r\n\r\n"
        "- Replays recorded on Nightly may not open on Stable: export them from the Archive before you switch channels.\r\n"
        "- A controller that falls asleep while a room is idle is found again when a button is pressed, not before.\r\n\r\n"
        "## Changes\r\n\r\n" + changes + "\r\n## Thanks\r\n\r\nSample Player and Sample Tester for the logs and the retests.\r\n";
}
// What the update check keeps: the first 2,000 bytes, then plain text.
inline std::string Notes(const std::string& markdown) {
    return sf4e::updates::PlainReleaseNotes(markdown.size() > 2000 ? markdown.substr(0, 2000) + "..." : markdown);
}
inline sf4e::launcher::ReleaseNews Release(const char* tag, const char* title, const char* date, bool newer, const std::string& markdown) {
    sf4e::launcher::ReleaseNews news{tag, title, date, Notes(markdown), "https://example.invalid/releases/" + std::string(tag)};
    news.stable = news.tag.find('-') == std::string::npos;
    news.newer = newer;
    return news;
}
}

// Five releases, newest first, as the update check lists them: Stable and Nightly mixed, two newer than the installed 1.5.0.
inline std::vector<sf4e::launcher::ReleaseNews> NewsReleases() {
    using news_detail::Body;
    using news_detail::Release;
    return {
        Release("v1.6.0-nightly.3", "Nightly 1.6.0.3", "2026-11-13", true, Body("1.6.0-nightly.3",
            "Nightly 1.6.0.3 reworks the News screen and gives the Ember tier a colour of its own.",
            "### Interface\r\n\r\n"
            "- **News is rebuilt as a hub.** Notifications, All, Updates and Change log are tabs now, and each release opens in a reader that scrolls with the pad, the keys and the wheel.\r\n"
            "- **Long titles and long tag names wrap instead of running out of the pane.** A release with a very long name is still read whole in its pane and in the reader, and its card shows the start of it.\r\n"
            "- **The Ember tier has its own red-orange.** It no longer looks like the focused item, and the tier's name is written next to its colour wherever a tier is shown.\r\n\r\n"
            "### Rooms\r\n\r\n"
            "- **Arcade rooms keep the winner on the machine.** The loser steps off, the next player in the queue sits down, and the room says who is next on every table card and in the chat.\r\n"
            "- **A table whose players both left is closed on every PC in the same frame,** so nobody is left looking at an empty cabinet that still says Playing.\r\n"
            "- **The room list shows the room's type** (Classic, Arcade, King of the Hill) next to its name, and a filter picks one type.\r\n"
            "- **Nothing changes for a room that is already open.** Its type is set when it is created and stays the same until it closes, whatever version each player has.\r\n")),
        Release("v1.5.7", "Ember 1.5.7", "2026-11-12", true, Body("1.5.7",
            "v1.5.7 gives Europe an Ember relay. Europe was the last region besides Asia still on a public relay, on the same domain that some routers' security filters have blocked since November 8.",
            "### Connections\r\n\r\n"
            "- **Ember's own relay for Europe.** Players in Europe now meet through a relay of Ember's own, in place of the public Europe relay. If the filters on some routers had extended their block to that relay, players behind them could no longer have joined a host in Europe. Asia keeps its public relay for now.\r\n"
            "- **Nothing to set up.** Ember still picks the nearest relay that answers. If the Europe relay goes offline, Ember moves to the next nearest one for new connections.\r\n"
            "- **The relay only helps players find each other.** Matches still go direct, player to player, whenever your connection allows it. Like any relay, it sees the internet addresses of the players who connect through it; it cannot read a match, which is encrypted between the players.\r\n\r\n"
            "### Fixes\r\n\r\n"
            "- A spectator who joined while a set was between games no longer waits on a black screen until the next game starts.\r\n"
            "- The Archive page keeps its scroll position when a replay is opened and closed.\r\n")),
        Release("v1.6.0-nightly.2", "Nightly 1.6.0.2", "2026-11-10", false, Body("1.6.0-nightly.2",
            "Nightly 1.6.0.2 adds the player card's levels and the Awards wall to the Profile.",
            "### Profile\r\n\r\n"
            "- **Levels count every online game.** Points come from games played, sets won and days played in a row, and the card shows how far the next tier is. Points are a grind value, not a rank.\r\n"
            "- **The Awards wall lists every award family** with its pips, the tier reached and what the next tier asks. Select an award to wear it as the card's title or icon.\r\n"
            "- **The Fighters tab shows each fighter's tier** as a frame round the fighter's picture, with the tier names written under it.\r\n\r\n"
            "### Training\r\n\r\n"
            "- A kept charge now comes out as any stun ends, short blockstun included, and the Data panel page ends with the oil switch.\r\n"
            "- Dash cancels and the EX gauge note are back in the Training room after the repository merge.\r\n"
            "- The frame meter keeps its place when the panel is opened over a paused game.\r\n\r\n"
            "### Fixes\r\n\r\n"
            "- **Points are counted once per game.** A game that ended in a disconnect no longer counts for both players, and a set that was abandoned counts the games that were played.\r\n"
            "- The card no longer shows a stale tier for a moment after a game ends; it waits for the bridge to answer.\r\n")),
        Release("v1.5.6", "Ember 1.5.6", "2026-11-09", false, Body("1.5.6",
            "v1.5.6 is a small release for rooms and the archive.",
            "### Rooms\r\n\r\n"
            "- Rooms no longer freeze after a rematch on slow links, and the post-match prompt always says who asked first.\r\n"
            "- **The invitation link works from the room board's menu** and from the Friends tab, and says when it has expired.\r\n\r\n"
            "### Archive\r\n\r\n"
            "- **Sets are grouped by opponent and day.** A set opens with its score, its games and the fighters used, and a game opens in the replay viewer.\r\n"
            "- Find a player lists the players met lately and searches the archive for the exact name.\r\n\r\n"
            "### Fixes\r\n\r\n"
            "- **A replay exported while a second one is queued keeps its own caption.** The caption used to follow the last one queued, so a set of three games could end up with the score of the next set written over its video.\r\n"
            "- Spectators who were refused a room because it was full are told the room is full, and are offered the room list, instead of being shown a generic error.\r\n"
            "- The hint bar keeps the pad glyphs when the keyboard was used last and the pad is touched again, and keeps the keys the other way round.\r\n\r\n"
            "Full changes: https://example.invalid/compare/v1.5.5...v1.5.6-with-a-long-address-that-has-no-spaces-in-it-at-all-and-keeps-going-on\r\n")),
        Release("v1.5.5", "Ember 1.5.5", "2026-11-08", false, Body("1.5.5",
            "v1.5.5 fixes the window flicker and brings the Japanese room names.",
            "### Interface\r\n\r\n"
            "- **" "\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\xA0\xE5\x90\x8D\xE3\x81\x8C\xE9\x95\xB7\xE3\x81\x84\xE3\x81\xA8\xE3\x81\x8D\xE3\x82\x82\xE3\x80\x81\xE5\x8D\x93\xE3\x81\xAE\xE3\x82\xAB\xE3\x83\xBC\xE3\x83\x89\xE3\x81\xAB\xE5\x8F\x8E\xE3\x81\xBE\xE3\x81\xA3\xE3\x81\xA6\xE8\xA1\xA8\xE7\xA4\xBA\xE3\x81\x95\xE3\x82\x8C\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82" "** " "\xE3\x83\x97\xE3\x83\xAC\xE3\x82\xA4\xE3\x83\xA4\xE3\x83\xBC\xE3\x81\xAE\xE5\x90\x8D\xE5\x89\x8D\xE3\x81\xA8\xE3\x83\x81\xE3\x83\xA3\xE3\x83\x83\xE3\x83\x88\xE3\x81\xAE\xE6\x96\x87\xE5\xAD\x97\xE3\x82\x82\xE3\x80\x81\xE3\x83\xA1\xE3\x83\x8B\xE3\x83\xA5\xE3\x83\xBC\xE3\x81\xA8\xE5\x90\x8C\xE3\x81\x98\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xB3\xE3\x83\x88\xE3\x81\xA7\xE8\xA1\xA8\xE7\xA4\xBA\xE3\x81\x95\xE3\x82\x8C\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82" "\r\n"
            "- The window no longer flickers when the game goes fullscreen, and the Windows taskbar stays out of the way.\r\n"
            "- Interface size from 0.7 to 1.5 fits every screen of the menus; the smallest text stays readable at a desk.\r\n\r\n"
            "### Connections\r\n\r\n"
            "- Players on Wi-Fi are told once, in the room, that a wired connection plays better; nothing is blocked.\r\n"
            "- The connection report now names the relay that was used and how long each step took.\r\n\r\n"
            "### Fixes\r\n\r\n"
            "- **A rematch asked for from the post-match prompt is answered on both PCs in the same frame,** so one side no longer waits on a prompt the other has already closed after a slow link dropped a message.\r\n"
            "- The Settings rail keeps the open panel in view when the window is narrow, and its heads wrap onto a second line instead of being cut.\r\n"
            "- Chat messages with a long web address wrap inside the panel, and the address is cut to the room the line has.\r\n")),
    };
}

// The stress release, newest of all: a title far longer than the pane, a long tag, and a line that is one long word.
inline sf4e::launcher::ReleaseNews NewsStressRelease() {
    std::string word;
    for (int i = 0; i < 7; ++i) word += "abcdefghij0123456789";
    return news_detail::Release("v1.6.1-nightly.20261114.2-release-candidate-with-a-long-name",
        "Nightly 1.6.1: a release title written far longer than the card or the pane can show on one line, to see how it is cut and wrapped",
        "2026-11-14", true, news_detail::Body("1.6.1",
            "Nightly 1.6.1 is the longest-named release the list has seen.",
            "### Checks\r\n\r\n"
            "- A file name with no space in it: " + word + ".zip\r\n"
            "- " + word + word + "\r\n"
            "- **A long line of ordinary words.** It runs on and on, with commas, and clauses, and more clauses, so that it wraps to four or five lines in a narrow pane and every line stays inside the pane's edges at every interface size, in every language the menus speak.\r\n"
            "- Another file: https://example.invalid/releases/download/v1.6.1/sf4-ember-netplay-1.6.1-windows-x64-setup-with-a-very-long-file-name.exe.sha256\r\n"));
}
// --news stress: the stress release in front, the list still at the update check's five.
inline void AddNewsStress(std::vector<sf4e::launcher::ReleaseNews>& news) {
    news.insert(news.begin(), NewsStressRelease());
    if (news.size() > 5) news.pop_back();
}
}
