#pragma once
#include "ShellAction.hxx"
#include "MenuNavigation.hxx"
#include "PlayerOpening.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/Rankings.hxx"
#include "../netplay/TournamentStatus.hxx"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
enum class Tone;
class GameMenu;

// Rankings (docs/design/SKILL_BADGE.md, "Rankings"): the player's own class and
// their choice to take part, and the board of players by class. It works from
// the service the Ember ID panel makes usable, like public rooms.
//
// The bridge counts every ranking request against one small budget, and reading
// the standing marks the class as shown, so nothing here refreshes by itself:
// the standing is read when the player opens Rankings, the board when they open
// it, and either again on Options. The profile card shows the standing's
// class, so away from these screens the standing is also read once a session
// for each service and Ember ID (ReadForProfile), as a first visit would.
// While the player takes part and classes are
// shown, opening Rankings also sends the name and flag rooms show for them, when
// the board lists them otherwise: nobody is listed before seeing this page.
class RankingsPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static constexpr const char* Screen = "rankings";
    static constexpr const char* BoardScreen = "rankings-board";
    static constexpr const char* PointsScreen = "rankings-points";
    // Rankings' pages in tab order: your standing, the class board and the points board.
    struct Page { const char* screen; const char* title; };
    static constexpr Page Pages[] = {{Screen, "rankings.title"}, {BoardScreen, "rankings.board_title"}, {PointsScreen, "rankings.points_title"}};
    // The page `screen` is, or -1 when it is none of Rankings'.
    static int PageOf(const std::string& screen);
    static bool Owns(const std::string& screen) { return PageOf(screen) >= 0; }
    // A refused request or a failed read, by its stable code, in words.
    static std::string FailureText(const std::string& code);
    // How this player is listed: the name and flag rooms show for them.
    static netplay::rankings::Listing OwnListing(const netplay::PlayerPreferences& preferences, const CountryCode& automaticCountry);
    // The Online play row's value: the last standing this session from `bridge`,
    // or empty before Rankings has read one.
    static std::string EntryValue(const ShellView& view, const std::string& bridge);
    // Every frame: asks for what entering a screen or pressing Options wants, sends
    // the listing and the player's choice, and takes their answers. `bridge` is
    // the usable service, empty without one; `own` is OwnListing.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge,
                const netplay::rankings::Listing& own, const Submit& submit);
    // `identityPending` says the Ember ID is still being asked about.
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, bool identityPending, std::string& title) const;
    // True when the row was the panel's own. Connect Discord is the Ember ID panel's.
    bool Activate(const MenuAction& action, const ShellView& view, MenuNavigation& navigation);
    // The player a press on a row of the board opens (A): by their listed name and flag, with the class the board lists them
    // in. None for any other row.
    std::optional<player_opening::Opening> PlayerOf(const ShellView& view, const MenuAction& action) const;
    // The Options button or key (Y, T) reads the screen's answer again.
    bool Shortcut(const MenuAction& action);
    // The class board and the points board (`screen`), drawn as a side list and
    // the page it picks (RankingsBoard.hxx) in place of the menu's list; `rows`
    // are this frame's Rows.
    void DrawBoard(const ShellView& view, const std::string& screen, GameMenu& menu, const std::vector<MenuEntry>& rows, MenuAction& action);
    // The focus is in a board's page, where Back returns to the side list (GameMenu::ownsBack).
    static bool ListFocused(const std::string& screen, const std::string& focus);
    bool Refreshable() const { return Owns(lastScreen_) && !bridge_.empty(); }
    // What a request in flight or the board's own state says on `screen`.
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const;
    // What the panel has to say, once.
    bool TakeSaid(std::string& text);
    // The shell is hidden: what was not asked yet is dropped, a choice on its
    // way is kept, and showing it again reads afresh.
    void Conceal();
private:
    // The one choice the panel acts on, identified by Command::request as
    // public rooms identify theirs.
    struct Pending {
        netplay::tournament::Command command;
        bool sent = false;
    };
    // The standing from the usable service, when one has arrived.
    const netplay::rankings::Standing* Current(const ShellView& view) const;
    std::vector<MenuEntry> StandingRows(const ShellView& view, bool identityPending) const;
    std::vector<MenuEntry> BoardRows(const ShellView& view) const;
    std::vector<MenuEntry> PointsRows(const ShellView& view) const;
    const netplay::rankings::PointsBoard* ShownPoints(const ShellView& view) const;
    const std::vector<netplay::rankings::BoardEntry>* ShownBoard(const ShellView& view) const;
    // The class whose players the board shows: the one last on the side, else
    // your own, else the highest with players.
    std::uint8_t ShownClass(const std::vector<netplay::rankings::BoardEntry>& board) const;
    std::optional<MenuEntry> RatingRow(const ShellView& view) const;
    // The reads, in priority order: a standing read goes first, and the rating details
    // follow every standing read so they never describe an older standing.
    enum class Read : std::uint8_t { Standing, Board, Points, Rating };
    static Read ReadOf(int page);
    std::optional<Read> NextRead(const ShellView& view, const std::string& screen) const;
    void Want(Read read) { wanted_[static_cast<std::size_t>(read)] = true; }
    void Choose(bool ranked);
    void TakeAnswers(const ShellView& view, bool owned);
    void ReadForProfile(const ShellView& view, const std::string& bridge, const Submit& submit);
    void Say(std::string text) { said_ = std::move(text); }

    std::string bridge_, lastScreen_, said_;
    // The board's focus last frame, so a row the pad moves to scrolls into view.
    std::string focus_;
    // The service and Ember ID whose standing ReadForProfile asked for, or found loaded.
    std::string profileRead_;
    netplay::rankings::Listing own_;
    // Which reads are wanted, by Read.
    std::array<bool, 4> wanted_{};
    std::uint64_t pointsSeen_ = 0;
    // -1: all fighters, else a native fighter id.
    int pointsFighter_ = -1;
    // The class the side list last stood on, 0 before one.
    std::uint8_t class_ = 0;
    // A board arrived or opened: the pad goes to the class it shows, once (DrawBoard).
    bool landOnClass_ = false;
    // The visit sent its listing already, so an answer that still differs cannot loop.
    bool listingSent_ = false;
    std::optional<Pending> pending_;
    std::uint64_t lastRequest_ = 0;
    // The finished counts and standing revision last handled, so each answer is
    // acted on once. How old a value is comes with the status.
    std::uint64_t standingSeen_ = 0, boardSeen_ = 0, standingRevision_ = 0;
};
} }
