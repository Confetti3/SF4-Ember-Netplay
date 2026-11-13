#pragma once
// The Friends tab of Profile and the pages over it (docs/design/PROFILE.md,
// "Friends tab"): the friends the bridge lists in four groups, a request's
// Accept, Join, Watch and Invite, More, Add a friend by code, the Sent and
// Blocked pages, Appear offline, and the question a Join asks when the player
// is in a room or a game. The data is ShellView::friends (netplay/FriendsView.hxx)
// and every ask is one of the commands::Friend* of netplay/FriendCommands.hxx.
//
// What the screens show is chosen here, without a frame, so friends_panel_test.cxx
// reads it: how the friends are grouped and ordered, what A does on a row, what
// More offers and which of it can be done now. The drawing is FriendsBoard.cxx.
#include "GameMenu.hxx"
#include "MenuNavigation.hxx"
#include "PlayerCard.hxx"
#include "ShellAction.hxx"
#include "ToastAction.hxx"
#include "../netplay/FriendsView.hxx"
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;

namespace friends_panel {
namespace nf = netplay::friends;

// The friends state the view holds, an empty one before there is any.
const nf::State& StateOf(const nf::View& view);
// What the tab can say of the friends now: Loading (nothing read yet), a player
// without a Discord account, a service without friends or no service, a read
// that failed with nothing before it, friends the player turned off, and the list.
enum class Mode { Loading, NoAccount, Off, Unavailable, Unreachable, Listed };
// `service`: the interface has a service it can use.
Mode ModeOf(const nf::View& view, bool service);
// The list is on show but the last sync failed (the state it kept is stale), as
// opposed to an action the bridge refused, which leaves the state current.
bool Stale(const nf::View& view);
// Whether an action of the bridge (accept, invite, remove, ...) may be asked now,
// and whether Join and Watch may (they do not wait on the one action at a time).
// `actWhy` and `goWhy` are the catalog ids of the words that say why not, null when they may.
struct Gate { bool act = false, go = false; const char *actWhy = nullptr, *goWhy = nullptr; };
Gate GateOf(const nf::View& view, bool service, bool pending);

// The requests newest first and the friends in the three groups, each in the
// order the tab shows them: in a room the joinable first and then by name,
// online by name, offline by when they were last seen.
struct Groups {
    std::vector<const nf::Incoming*> requests;
    std::vector<const nf::Friend*> inRoom, online, offline;
};
Groups Arrange(const nf::State& state);

// A friend's card, as DrawOtherCard draws it: their main and its points and, when the bridge sent them, the account's
// as a second line; without those the one line of the main (a friend's state carries nothing more).
OtherCardView CardOf(const nf::Friend& entry, FighterArt art);
// A friend's public room can be joined now.
bool Joinable(const nf::Friend& entry);
// What A does on a friend's row: Join their room, Invite them to this player's
// room, or open More. Offline friends have only More.
enum class Press { More, Join, Invite };
Press PressOf(const nf::Friend& entry, bool canInvite);

// Row ids: a request, a friend (named by the id their state row carries), a
// Sent request, a Blocked player; and the rows and pages that have a name.
constexpr const char* RequestPrefix = "request:";
constexpr const char* FriendPrefix = "friend:";
constexpr const char* SentPrefix = "sent:";
constexpr const char* BlockedPrefix = "blocked:";
constexpr const char* AddRow = "friends-add";
constexpr const char* SentRow = "friends-sent";
constexpr const char* BlockedRow = "friends-blocked";
constexpr const char* AppearRow = "friends-appear-offline";
constexpr const char* NewCodeRow = "friends-new-code";
constexpr const char* ForgetRow = "friends-turn-off";
constexpr const char* TurnOnRow = "friends-turn-on";
constexpr const char* TheirCodeRow = "friends-their-code";
constexpr const char* SendRow = "friends-send";
constexpr const char* NoteRow = "friends-note";
constexpr const char* StayRow = "friends-visit-stay";
constexpr const char* GoRow = "friends-visit-go";
// The id after `prefix`, empty when `row` has another prefix.
std::string IdOf(const std::string& row, const char* prefix);

// "3 h ago" or "2 d ago", `at` against `now` (unix seconds); an hour at least,
// and empty when either is not known.
std::string Ago(std::uint64_t now, std::uint64_t at);
// Where a friend is: "Playing at table 1 · Player 5's room", "At the menus", "Last online 3 h ago", ...
std::string Status(const nf::Friend& entry, std::uint64_t now);
// A request's second line: "Played you 6 games, last 1 d ago" or that they have not.
std::string PlayedLine(const nf::Incoming& request, std::uint64_t now);
// The digits of a friend code typed so far, counted as the bridge reads them (letters and numbers; dashes and spaces
// are dropped), and the code in the form it is shown, XXXX-XXXX.
std::size_t CodeSymbols(const std::string& typed);
std::string CodeShown(const std::string& typed);
constexpr std::size_t CodeLength = 8;

// The choices More offers on a friend or a request, each listed with the
// reason in its detail when it cannot be done now, and the one question that
// asks again before the choice `ask` ("remove", "remove-block", "decline",
// "decline-block") goes ahead.
std::vector<MenuChoice> FriendChoices(const nf::State& state, const nf::Friend& entry, bool canInvite, Gate gate);
std::vector<MenuChoice> RequestChoices(Gate gate);
std::vector<MenuChoice> AskChoices(const std::string& ask, const std::string& name);

// How an action the panel sent stands in the view, from `finished` and `refusals` as the view had them when it was
// sent: still waiting, refused (the refusal's own words say why), gone through, gone through to find friends off, or
// no answer by `deadline`. The answer is read before the deadline, so one that came while Ember was hidden is said.
enum class Outcome { Waiting, Refused, Done, Off, GivenUp };
Outcome OutcomeOf(const nf::View& view, std::uint64_t finished, std::uint64_t refusals, double deadline, double now);
// How long the panel waits on an action before it says there was no answer: past the runtime's own limit for a lost
// request (netplay::BridgeFlights::GiveUpMs, 60 s), which ends it with a refusal first. The interface's clock, seconds.
constexpr double GiveUpSeconds = 75;
// What a request accepted says: "You and Player 2 are now friends.", by the name the request carries; empty when the
// state holds no such request.
std::string AcceptedWords(const nf::State& state, const std::string& request);

// Words or a toast Ember said, and whether the player had the time to see them: Ember hid at `now` (the last frame
// it showed) before their card's `seconds` were up, or a toast came over the post-match prompt, whose keys answer
// the prompt and not the toast. Unseen, they are said once more on the next free menu.
struct Told {
    double at = 0, seconds = 0;
    bool unseen = false;
    void Hidden(double now) { if (now - at < seconds) unseen = true; }
    bool Lapsed(double now) const { return !unseen && now - at >= seconds; }
};
}

// The panel: the state the tab keeps between frames (what it asked and is
// waiting on, the popout it is about to open, the code being typed) and the
// ScreenPanel calls ApplicationShell routes to it (ApplicationShell__Screens.cxx).
class FriendsPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static constexpr const char* Screen = "friends";
    static constexpr const char* AddScreen = friends_panel::AddRow;
    static constexpr const char* SentScreen = friends_panel::SentRow;
    static constexpr const char* BlockedScreen = friends_panel::BlockedRow;
    static constexpr const char* VisitScreen = "friends-visit";
    // The prompt over a room game's WIN (ApplicationShell__PostMatch.cxx): its keys are its own, so a toast over it
    // shows without them, and what was said over it unseen is said again on the next free menu.
    static constexpr const char* PromptScreen = "post-match";
    // Settings, Online, Friends: the panel of Appear offline, the friend code and Turn off friends (Turn on friends, once off).
    static constexpr const char* SettingsScreen = "tab-online/friends";
    // The tab or a page over it: where X, Y and Delete are the tab's even in a room.
    static bool OnTab(const std::string& screen) { return screen == Screen || screen == AddScreen || screen == SentScreen || screen == BlockedScreen; }
    // The tab, a page over it or the Settings panel: the screens the friends are kept fresh on.
    static bool Shown(const std::string& screen) { return OnTab(screen) || screen == SettingsScreen; }
    // The shortcuts the tab takes before a room's own: X (Add a friend), Y (More) and Delete (Remove friend).
    static bool Takes(unsigned shortcut) { return shortcut == MenuInput::Fighter || shortcut == MenuInput::Options || shortcut == MenuInput::Leave; }
    // The Settings panel's rows, which Activate takes as the tab's own.
    static bool SettingsRow(const std::string& row);
    // The Settings panel is Settings' own screen (it only borrows this panel's rows and chips), so it is not owned here.
    static bool Owns(const std::string& screen) { return OnTab(screen) || screen == VisitScreen; }

    // Every frame: Visit when the tab opens (again when its cadence lapsed) and Leave when it closes; the question
    // a Join asks, opened over whatever shows; a refusal's words, and what an action that went through says, or that
    // it had no answer. `bridge` is the usable service, empty without one.
    void Update(const ShellView& view, MenuNavigation& nav, const std::string& bridge, double now, const Submit& submit);
    // The shell is hidden: the cadence is told again when it shows, and words or a toast it hid before their time
    // are said once more on the next free menu.
    void Conceal();
    // The words to show as a notice (a refusal, an invite or request sent), once (once more if hidden unseen).
    bool TakeSaid(std::string& words, Tone& tone);
    // A friend's request or invite, said once as the state that holds it is read, with its buttons: Accept for the
    // request (Later leaves it in the list), Join for the invite (it takes the invite, then drops it) and Dismiss.
    // What the first state already held is not said (Home's Profile row counts the requests), nor an invite that
    // ran out, nor anything while the Friends screens show it. One a frame: a second waits for the next. One said
    // over the post-match prompt, or hidden before its time, is said once more on the next free menu while it stands.
    bool TakeToast(ToastRequest& toast);
    // A toast's command, sent as the panel sends its own when it is one the panel waits on (a request's Accept), so
    // its answer is said; false for any other, which the shell sends as it is.
    bool AnswerToast(const ShellView& view, const RuntimeRequest& command, const Submit& submit);
    // The toast with these words was answered (any of its buttons, by key, click or its Notifications row): it is
    // not said again.
    void ToastAnswered(const std::string& text);

    // The screen's rows. `dialog`: a choice or question is open over them.
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, bool dialog, std::string& title);
    // Select on a row, an option chosen from one and the code typed on Add a friend; false when the row is not the
    // panel's. `Shortcut` is X, Y and Delete.
    bool Activate(const ShellView& view, const MenuAction& action, MenuNavigation& nav, const Submit& submit);
    bool Choose(const ShellView& view, const MenuAction& action, const Submit& submit);
    bool Adjust(const MenuAction& action);
    void Shortcut(const ShellView& view, const MenuAction& action, MenuNavigation& nav);
    // The header's chips and the hint bar's shortcuts, and the popout asked for opened on its row.
    void Dress(const ShellView& view, const std::string& screen, const std::vector<MenuEntry>& rows, GameMenu& menu);
    // Settings, Online, Friends: Appear offline, the friend code (Select asks for a new one) and Turn off friends,
    // each disabled with the state's words while the friends cannot be used.
    std::vector<MenuEntry> SettingsRows(const ShellView& view);
    // A room member's Add friend (and Accept request, `accepts`: the bridge accepts a mutual request) and Block, by
    // the card token their ticket carried and their room name. A refusal or what went through is said as a notice.
    void RequestMember(const ShellView& view, const std::string& card, const std::string& name, bool accepts, const Submit& submit);
    void BlockMember(const ShellView& view, const std::string& card, const std::string& name, const Submit& submit);
    // The Friends tab's board at the cursor, in a body `height` tall.
    void Draw(const ShellView& view, GameMenu& menu, const std::vector<MenuEntry>& rows, MenuAction& action, float height);

private:
    struct Pending {
        bool active = false;
        std::string row, say;
        // `finished` and `refusals` as the view had them when it was sent, and when to give up waiting.
        std::uint64_t finished = 0, refusals = 0;
        double deadline = 0;
        // The page that goes back once it went through (Add a friend).
        bool back = false;
    };
    struct Raise {
        // The row whose popout is to open, and the choice it asks about (empty: More itself).
        std::string row, ask;
        bool issued = false, seen = false;
        int waits = 0;
    };
    template <class Command>
    bool Send(const ShellView& view, Command command, const Submit& submit, const std::string& row = {}, std::string say = {}, bool back = false);
    void Say(std::string words, Tone tone);
    // `free`: the shell shows a menu nothing is open over, not the post-match prompt.
    void Announce(const ShellView& view, const std::string& screen, bool free);
    const std::string& Bridge(const ShellView& view) const;
    void Attach(std::vector<MenuEntry>& rows, const ShellView& view, bool dialog);
    void Ask(const std::string& row, const std::string& ask) { raise_ = Raise{row, ask}; }

    std::string bridge_, code_, said_, focus_;
    Tone saidTone_{};
    bool service_ = false, started_ = false, asked_ = false, sawVisiting_ = false, hasSaid_ = false;
    std::uint64_t refusalsSeen_ = 0, visitHandled_ = 0;
    double now_ = 0;
    Pending pending_;
    Raise raise_;
    // What was said as a toast or seen while the screens showed it, and whether the first state was read; the
    // toast waiting for the shell.
    std::set<std::string> told_;
    bool read_ = false;
    std::optional<ToastRequest> toast_;
    // The requests and invites said as a toast, by id, with the toast's words, until they were seen (friends_panel::Told).
    struct Toasted { friends_panel::Told told; std::string text; };
    std::map<std::string, Toasted> toasted_;
    // The last words taken, until they were seen; `again` once they are being said a second time, which is the last.
    struct Spoken { std::string words; Tone tone{}; friends_panel::Told told; };
    std::optional<Spoken> spoken_;
    bool again_ = false;
};
} }
