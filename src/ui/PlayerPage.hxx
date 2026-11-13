#pragma once
// Another player's profile (docs/design/PROFILE.md, "Another player's profile"): a page of its own with the tabs Card,
// Fighters, Sets and Awards, opened from a room, a friend, a ranking, a replay or a search, and the player's own Sets
// tab, which is the same list of sets for their own name.
//
// What the page shows and what it offers is decided here by plain functions that need no frame
// (player_page_test.cxx): which state the open profile is in and which tabs it has, the Primary action by relation and
// presence, More's entries and whether each can be done now, the words of each state, the subject the Full card is
// drawn from, and where the focus opens. PlayerPage holds the little the page keeps between frames, and the drawing
// (PlayerPage__Draw.cxx) only draws. Every action goes through the paths the Friends tab and the room's member page
// already have (FriendsPanel), so it is gated, logged and answered in words alike.
//
// Opening is one call, ApplicationShell::OpenPlayer; the page then sends ProfileOpen as soon as there is a service to
// send it to, and ProfileClose when it is left.
#include "ArchiveTab.hxx"
#include "CardModel.hxx"
#include "CardSets.hxx"
#include "FriendsPanel.hxx"
#include "GameMenu.hxx"
#include "MenuNavigation.hxx"
#include "PlayerCard.hxx"
#include "ShellAction.hxx"
#include "WatchQueue.hxx"
#include "../netplay/Friends.hxx"
#include "../netplay/ProfileRequests.hxx"
#include "../netplay/Profiles.hxx"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;

namespace player_page {
namespace profiles = netplay::profiles;
namespace nf = netplay::friends;
using Relation = netplay::levels::Relation;

// The screens. Card is not "player": that is Settings' Controls page.
constexpr const char* CardScreen = "player-card";
constexpr const char* FightersScreen = "player-fighters";
constexpr const char* SetsScreen = "player-sets";
constexpr const char* AwardsScreen = "player-awards";
constexpr const char* PickScreen = "player-pick";
// The pages over the Sets tab: the archive's filters with the Player row fixed, and its fighter page.
constexpr const char* SetsFilterScreen = "player-sets-filter";
constexpr const char* SetsFighterScreen = "player-sets-fighter";
// The rows the Card tab adds to the card's own: the Primary action and More (the hero's two buttons), and a player in
// the pick list.
constexpr const char* PrimaryRow = "player-primary";
constexpr const char* MoreRow = "player-more";
constexpr const char* HitPrefix = "player-hit:";

// A screen of another player's profile: its tabs, its pick list and the pages over its Sets.
bool OnProfile(const std::string& screen);

// What the page is showing. Reading: the open has not been answered (or reached the view, or has no service to go to);
// Card: a card is on show; NoCard: the service has none for them; Blocked: the viewer blocked them; Nobody: no player
// goes by the name; Several: more than one does, and the pick list is asked; Unreachable and Unsupported: the service
// could not be reached, or has no profiles.
enum class Page : std::uint8_t { Reading, Card, NoCard, Blocked, Nobody, Several, Unreachable, Unsupported };

// The profile open as the page reads it.
struct Shown {
    Page page = Page::Reading;
    profiles::Key key;
    profiles::Known known;
    // The card token to act by (empty when opened by friend id, request id or code, or a name that has not been searched).
    std::string token;
    // The card on show (Card), and the card of a player the viewer blocked when the service sent one; else null.
    const netplay::levels::Card* card = nullptr;
    std::vector<profiles::Hit> hits;
    // False for a card from a service without profiles: points, fighters and relation only.
    bool profiled = true;
    // The name the page says: the card's, else what the opener knew; empty when neither.
    std::string name;
};
// The page's reading of `profile` for the open the page asked (`key`, `known`: what the opener held; a null key means the
// view's profile as it stands). `service`: there is a service to ask. `offering`: whether it offers profiles. `blocks`:
// who the viewer blocked, which tells a blocked player's missing card from any other.
Shown ShownOf(const profiles::Profile& profile, const profiles::Key& key, const profiles::Known& known, bool service, profiles::Service offering,
    const nf::State& friends);

// Whether opening `key` is opening the player themselves: their exact name (and country, when the opener gave one).
bool OpensOwn(const profiles::Key& key, const profiles::Known& known, const std::string& ownName, const CountryCode& ownCountry);

// The tabs a page has: screen id and label. Reading and a card (and a failed read) have all four; a card from a service
// without profiles has no Awards; no card, nobody by the name and a service without profiles have Card and Sets; a
// blocked player has Card; Several has none (the pick list).
std::vector<std::pair<std::string, std::string>> TabsOf(const Shown& shown);
// Whether `screen` (a tab) is one `shown` has.
bool HasTab(const Shown& shown, const std::string& screen);

// How the viewer stands with the player: what the Primary and More read. Made by StandingOf from the page's reading, the
// friends and the room, so the choices are plain data.
struct Standing {
    Page page = Page::Reading;
    Relation relation = Relation::None;
    // There is a name to list sets by.
    bool named = false;
    // How they can be named to the friends paths.
    bool token = false, byRequest = false, byCode = false;
    // A friend's id and whether the list shows them, in a joinable public room, online; whether the viewer has a room to offer.
    std::string friendId;
    bool joinable = false, online = false, canInvite = false;
    // The id of the block of this player, when the blocks list holds one.
    std::string blockId;
    // The member opened from shares a Steam profile.
    bool steam = false;
    // What the friends paths allow now: a friend's or a request's action (gate), and a member's request or block (member).
    friends_panel::Gate gate, member;
    // The friend choices the Friends tab offers (Join, Watch, Invite, Remove, Remove and block), with their reasons;
    // empty for anyone who is not a friend.
    std::vector<MenuChoice> friendChoices;
};
// The friends and room facts the Standing needs besides the page's reading.
struct Links {
    friends_panel::Gate gate, member;
    bool canInvite = false;
    bool steam = false;
};
Standing StandingOf(const Shown& shown, const nf::State& friends, const Links& links);

// The one thing most worth doing now (docs/design/PROFILE.md, "The Primary action and More"). `does` is the choice id the
// press carries out, empty when it does nothing (Reading, Request sent).
struct Primary {
    enum class Kind : std::uint8_t { Reading, TryAgain, AddFriend, AcceptRequest, RequestSent, JoinRoom, Invite, ViewSets, Unblock } kind = Kind::Reading;
    std::string label, detail, does;
    bool enabled = false;
};
Primary PrimaryOf(const Standing& standing);

// More's entries, each listed even when it cannot be done now, with the reason in its detail: Add friend, Accept request
// or Request sent; for a friend Join their room, Watch and Invite to my room; View sets; Steam profile; for a friend Remove
// friend and Remove and block; Block, or Unblock for a blocked player. Ids: add, accept, sent, join, watch, invite, sets,
// steam, remove, remove-block, block, unblock.
std::vector<MenuChoice> MoreOf(const Standing& standing);
// Whether choosing `choice` asks again first (Remove friend, Remove and block, Block, Unblock), and its question: one
// option, "yes", worded as the choice and naming the player.
bool Asks(const std::string& choice);
std::vector<MenuChoice> QuestionOf(const std::string& choice, const std::string& name);

// The words of a page with no card to draw (one paragraph each); none for a card.
std::vector<std::string> NotesOf(Page page, const std::string& name);

// What the hero's foot and the Full card's right edge are made from. `member`: a member of the room this PC is in, and
// the fighter they picked there (-1: none).
struct Around {
    const nf::Friend* friendEntry = nullptr;
    bool inRoom = false;
    int roomFighter = -1;
    std::uint64_t now = 0;
};
player_card::Subject SubjectOf(const Shown& shown, const Standing& standing, const Around& around, const player_card::SetsView& sets);
// The rows of the Card tab: the card's own parts, then the Primary and More (More carrying `ask`'s question when one is
// being asked).
std::vector<MenuEntry> CardRows(const player_card::FullCardView& view, const Primary& primary, const Standing& standing, const std::string& ask,
    const std::string& name);
// The row the page opens with the focus on: the Primary when it can be pressed, else the card's first strip tile, else More.
std::string FocusOnOpen(const Primary& primary, const player_card::FullCardView& view);

// The Fighters tab's rows (their fighters, info rows, the played ones first) and the Awards wall's (every family but Friends).
std::vector<MenuEntry> FighterRows(const Shown& shown);
std::vector<MenuEntry> AwardRows(const Shown& shown);
// The pick list: a row for each player that goes by the name.
std::vector<MenuEntry> PickRows(const Shown& shown);
// Whether the Card tab has a More (every state that has a card to act on, and the states that name one); the hint bar names Y only then.
bool OffersMore(const Shown& shown);
}

// The page: what it keeps between frames, and the calls the shell makes for its screens (ApplicationShell::PlayerScreens).
class PlayerPage {
public:
    using Submit = std::function<bool(ShellAction)>;
    // The player's own Sets tab (ProfilePanel::SetsScreen) is the page's too.
    static bool Owns(const std::string& screen);

    PlayerPage();

    // Opens `key` on the page: the profile replaces the one open when this is called from a profile (so Back returns to
    // where the first was opened from), else is pushed. The player themselves open Profile on Card instead.
    void Open(const profiles::Key& key, const profiles::Known& known, MenuNavigation& navigation);
    // Every frame: ProfileOpen once there is a service (again for Try again), ProfileClose when the page is left, the sets'
    // listing while a Sets tab is on show, the pick list or a tab the state does not have, and the focus on the first
    // settled card. `bridge` is the usable service, empty without one; `ownName` and `ownCountry` are the player's own as
    // others see them: the name the own Sets tab lists by, and what tells the player's own name from another's.
    void Update(const ShellView& view, const std::string& screen, MenuNavigation& navigation, const std::string& bridge, const std::string& ownName,
        const CountryCode& ownCountry, const Submit& submit);
    // The shell is hidden: nothing asked is kept waiting.
    void Conceal();

    // What the page's presses act through: the navigation, the friends paths (FriendsPanel), the shared set queue, the
    // command queue and the shell's error line.
    struct Hands {
        MenuNavigation& navigation;
        FriendsPanel& friends;
        WatchQueue& queue;
        const Submit& submit;
        std::string& error;
    };
    // The screen's rows. `friends` says what the friends paths allow now; `idle`: no room is joined (a set can be watched);
    // `dialog`: a choice or question is open over the rows.
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, const FriendsPanel& friends, bool idle, bool dialog, std::string& title);
    // Select on a row, and an option chosen from one, which carry out what the row means. False: not the page's.
    bool Activate(const ShellView& view, const MenuAction& action, const Hands& hands);
    bool Choose(const ShellView& view, const MenuAction& action, const Hands& hands);
    // Text typed into the filters' Opponent row.
    bool Adjust(const MenuAction& action);
    // Y on the Card tab opens More; X and Y on a Sets list are the archive's (Filters, Refresh or Retry).
    void Shortcut(const ShellView& view, const MenuAction& action, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& navigation);
    bool TakesShortcut(const std::string& screen, const MenuAction& action) const;

    // The header (eyebrow, tabs, Earned chip, filters), the hint bar's shortcuts and the open question's popout.
    void Dress(const ShellView& view, const std::string& screen, const std::string& ownName, const std::vector<MenuEntry>& rows, GameMenu& menu);
    // The Card, Fighters and Awards tabs' boards, a set's card and its pane, and the pick list's rows.
    void Draw(const ShellView& view, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& navigation, MenuAction& action, float height);
    bool DrawCard(const ShellView& view, const std::string& screen, const MenuEntry& row, ImVec2 min, ImVec2 max) const;
    void DrawDetail(const ShellView& view, const std::string& id, MenuNavigation& navigation, const std::vector<MenuEntry>& rows) const;
    // The archive tab the Sets lists are, for the shell's status line, fighter page and filters.
    const ArchiveTab& Sets() const { return sets_; }
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const { return sets_.Status(view, screen, status, tone); }

    // The page's reading of the view now (the open profile, or what the page is waiting on).
    player_page::Shown ShownNow(const ShellView& view) const;

private:
    struct Raise {
        bool issued = false, seen = false;
        int waits = 0;
    };
    player_page::Links LinksOf(const ShellView& view, const FriendsPanel& friends, const player_page::Shown& shown) const;
    // What a press of the Primary or a choice of More does, once it has been asked and answered.
    void Do(const ShellView& view, const std::string& what, const player_page::Shown& shown, const player_page::Standing& standing, const Hands& hands);
    player_page::Around AroundOf(const ShellView& view, const player_page::Shown& shown, const player_page::Standing& standing) const;
    // The links as the page reads them with no friends panel at hand (nothing pending): what the focus and the subject need.
    player_page::Links PlainLinks(const ShellView& view, const player_page::Shown& shown) const;
    void Begin(const profiles::Key& key, const profiles::Known& known, MenuNavigation& navigation);
    player_card::Subject SubjectNow(const ShellView& view, const player_page::Shown& shown, const player_page::Standing& standing) const;
    player_page::Standing StandingNow(const ShellView& view, const player_page::Shown& shown, const player_page::Links& links) const;
    void Ask(const std::string& choice) { ask_ = choice; raise_ = Raise(); }

    // What the page was asked to open, and whether the ProfileOpen for it went out.
    profiles::Key key_;
    profiles::Known known_;
    bool asked_ = true, onProfile_ = false;
    std::string bridge_, focus_, ask_, ownName_;
    CountryCode ownCountry_;
    // The profile whose first settled card the focus was set for.
    std::uint64_t focused_ = 0;
    Raise raise_;
    // Their recent sets on the Card tab, and the sets lists (their own on Sets, the player's own on the own Sets tab).
    CardSets cardSets_;
    ArchiveTab sets_;
};

} }
