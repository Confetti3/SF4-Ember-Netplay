#pragma once
// Replays > Archive: the public replay archive's sets, browsed from the game
// (docs/design/REPLAY_ARCHIVE.md). Rows are sets, grouped as the archive page
// groups them (ArchiveSets.hxx), newest first; a Filters row opens one page of
// filter rows (player or event, fighter, class, country, event, round, sort);
// the end of the list asks for the next page (netplay/ArchiveRequests.hxx: Requests::Ask). Select on a
// set offers Watch now, which has the game downloaded and played as a replay
// link's file is, and Find this player's sets for each named player. The
// listing is read from ShellView::archiveBrowse, which the runtime fills
// (netplay/ArchiveRequests.hxx). ApplicationShell holds it and ArchiveScreens, a ScreenPanel,
// routes its screens here; the tab sits in Replays' strip (ReplaysPanel::Tabs).
#include "ArchiveSets.hxx"
#include "GameMenu.hxx"
#include "ShellAction.hxx"
#include "WatchQueue.hxx"
#include "MenuNavigation.hxx"
#include "../netplay/Archive.hxx"
#include <functional>
#include <imgui.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
enum class Tone;

class ArchiveTab {
public:
    using Submit = std::function<bool(ShellAction)>;
    static constexpr const char* Screen = "replay-archive";
    static constexpr const char* FilterScreen = "replay-archive-filter";
    static constexpr const char* FighterScreen = "replay-archive-fighter";
    static bool Owns(const std::string& screen) { return screen == Screen || screen == FilterScreen || screen == FighterScreen; }
    // The row a set has: "archive:" and its set ID, or an online group's newest game's digest.
    static constexpr const char* RowPrefix = "archive:";
    static bool IsRow(const std::string& id) { return id.compare(0, 8, RowPrefix) == 0; }
    // The fighter a card on the fighter page names ("af-<native id>"), if the id is one.
    static int FighterOf(const std::string& id);

    // The filter the player is browsing by.
    const netplay::archive::Filter& Wanted() const { return filter_; }
    // The public archive's sets of one player (an exact name), on this tab: for a profile's Find sets and a
    // row's Find this player's sets. Back returns to the screen it was called from.
    void OpenFor(const std::string& playerName, MenuNavigation& navigation) { Browse(netplay::archive::Filter::Named(playerName)); navigation.Push(Screen); }
    // Browse by `filter` from the first page (Find this player's sets sets only a name).
    void Browse(netplay::archive::Filter filter);

    // Every frame, before the rows: the service the archive is read from (empty when
    // none is set up), the focused row, and the page to ask for, if any.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge, const std::string& focus, const Submit& submit);
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, bool idle, std::string& title) const;
    // Select on a row of these screens: true when it was the tab's own.
    bool Activate(const MenuAction& action, MenuNavigation& navigation);
    // Left or Right, a pick of a value's words (the steps to it) or typed text on a filter row.
    bool Adjust(const MenuAction& action);
    // An option chosen on a set's row: Watch now fills `queue` with the set's games and starts the first, and
    // Find this player's sets browses by that name. `error` is what the shell shows when a request could not be
    // queued. False when the row is not a set's.
    bool Choose(const MenuAction& action, const ShellView& view, const Submit& submit, MenuNavigation& navigation, WatchQueue& queue, std::string& error);
    // The Filters shortcut and the Retry or Refresh shortcut (the hint bar's two).
    void Shortcut(const MenuAction& action, const ShellView& view, const std::string& screen, MenuNavigation& navigation);
    std::vector<LegendHint> Hints(const ShellView& view, const std::string& screen) const;
    // The last download's progress or failure, once the player asked for a game to watch.
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const;
    // The filter as header chips, and as the Filters row's value.
    std::vector<std::string> Chips() const;
    static std::string FilterSummary(const netplay::archive::Filter& filter);
    // A card (the players, score and fighters) and the pane (the games and Watch now) of a set's row.
    bool DrawCard(const ShellView& view, const MenuEntry& row, ImVec2 min, ImVec2 max) const;
    void DrawDetail(const ShellView& view, const std::string& id, MenuNavigation& navigation, const std::vector<MenuEntry>& rows) const;
    // Whether the fighter page's card is ticked.
    bool Ticked(int fighter) const { return filter_.fighter == fighter; }

private:
    // The grouping of the listing the runtime holds, remade only for a new one.
    void Regroup(const ShellView& view);
    const archivesets::Entry* Find(const std::string& id) const;
    bool Heard(const ShellView& view) const;
    static bool NearEnd(int at, std::size_t rows);
    std::vector<QueuedGame> Games(const archivesets::Entry& entry, const ShellView& view) const;
    void FilterRows(const ShellView& view, std::vector<MenuEntry>& rows) const;
    // What an event or country value steps through: nothing, then the archive's choices (and the one
    // chosen, should the archive no longer list it).
    std::vector<std::string> EventValues() const;
    std::vector<std::string> CountryValues() const;

    netplay::archive::Filter filter_;
    std::string bridge_;
    // What was last sent, and the listing it was sent for.
    commands::ArchiveList last_;
    std::uint64_t revision_ = 0;
    bool retry_ = false;
    // The listing the entries are of, which they point into.
    std::shared_ptr<const netplay::archive::Listing> held_;
    std::vector<archivesets::Entry> entries_;
    // The events and countries the archive offers to filter by, as last heard.
    std::vector<std::string> events_, countries_;
};
} }
