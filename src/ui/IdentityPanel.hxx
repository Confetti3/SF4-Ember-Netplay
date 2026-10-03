#pragma once
#include "MenuNavigation.hxx"
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/IdentityView.hxx"
#include "../netplay/TournamentStatus.hxx"
#include <optional>
#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
struct ShellAction;
enum class Tone;

// The Ember ID screens: the identity itself, its backup and restore, the
// accounts linked through tournament services, the matches those services
// assigned, which the player can play from here, and Connect Discord, which
// takes a player from no Ember ID to a connected Discord account step by
// step. It holds the passphrases the
// player types until they are sent, and wipes them when the player leaves
// these screens. One request is in flight at a time; the rest wait in order.
class IdentityPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    // Ember's own tournament service, the one most players trust.
    static constexpr const char* EmberService = "https://bridge.embernetplay.link";
    static bool Owns(const std::string& screen);
    // Why a tournament match stopped, from its stable code, in words.
    static std::string TournamentFailure(const std::string& code);
    // A match link named this match: the matches screen selects its service,
    // refreshes the list and focuses the match's row, for the player to press
    // Play. A service the player does not trust, or a match not in their
    // list, is said instead.
    void OpenMatch(const std::string& bridge, const std::string& match);
    // A tournament site asked the player to connect Discord on `bridge`: the
    // Connect Discord screen shows that service, from wherever the player is
    // in the steps. Nothing is sent to Discord until the player presses Connect.
    // `automatic`: the player just clicked the link at the menus, so the
    // steps run by themselves up to Discord's page; otherwise the screen
    // waits for Connect. A link for the sign-in already waited for only
    // shows it.
    void OpenDiscord(const std::string& bridge, bool automatic = false);
    // The row the matches screen should focus, once it is among `rows`.
    std::string TakeFocus(const std::vector<MenuEntry>& rows);
    // The Home entry's line: where to start, how many matches are ready to
    // play, or what the screens hold.
    std::string HomeDetail(const ShellView& view) const;
    // A playable match the player has not been told about yet: true once for
    // each new one, except while the matches screen already shows them.
    bool TakeAssigned(const ShellView& view);
    // Every frame, before the rows: notes answers, sends the next request,
    // and wipes what the player typed once they leave these screens.
    void Update(const ShellView& view, const std::string& screen, const Submit& submit, double now);
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, std::string& title) const;
    void Activate(const MenuAction& action, const ShellView& view, MenuNavigation& navigation);
    // A text row's accepted text or a choice's option.
    void Accept(const MenuAction& action, const ShellView& view);
    // The panel's own status line while one of its screens shows.
    bool Status(std::string& status, Tone& tone, double now) const;
    // The shell is hidden: wipes what was typed, as leaving the screens does,
    // drops requests not yet sent, and asks for a fresh status when the
    // screens show again.
    // Hiding Ember stops a Connect Discord setup, as leaving its screen does;
    // a wait for the browser goes on.
    void Conceal() {
        if (attempt_ == Attempt::Setup || attempt_ == Attempt::Opening) CancelAttempt();
        Wipe(); queue_.clear(); play_.reset(); onScreens_ = false; lastScreen_.clear();
    }
private:
    // A request waiting to be sent. A look-up's answer is a service the
    // player may trust, not the selected one's profile. Every request names
    // the Connect Discord journey it was queued in: once a new journey
    // starts, an earlier read (or Connect) is retired without effect, and an
    // earlier change still finishes but leaves the new journey's requests.
    struct Queued {
        netplay::IdentityRequest request;
        bool lookUp = false;
        std::uint64_t journey = 0;
        // An inspection only to read this service's Discord account, which
        // leaves the selected service as it is.
        std::string account;
    };
    bool Busy(const ShellView& view) const;
    bool Answered(const ShellView& view) const;
    void Finish(const ShellView& view);
    void Queue(netplay::IdentityRequest request, bool lookUp = false, std::string account = {});
    // The read in flight failed: its service's account is not trusted, and a
    // sign-in waiting on it pauses its polls.
    void ReadFailed();
    // Drops the requests waiting to be sent after the one in flight failed,
    // unless it came from an earlier journey: those are not its to drop.
    void ProfileRead(const std::string& bridge);
    void DropQueue();
    void Say(std::string text, bool error, double seconds = 6);
    void Wipe();
    void Refresh(const ShellView& view, const std::string& screen);
    void SelectBridge(const ShellView& view, const std::string& bridge);
    // Sends a waiting Play or Stop, or the assignment refresh the matches screen wants.
    void SendTournament(const ShellView& view, const std::string& screen, const Submit& submit);
    // Says so when the list a link's refresh brought back lacks its match.
    void CheckOpenedMatch(const netplay::tournament::AssignmentList& list);
    MenuEntry ServiceRow(const ShellView& view, const netplay::IdentityBridge& bridge) const;
    std::optional<MenuEntry> DiscordRow(const ShellView& view, bool busy) const;
    // Ember's own service, once trusted; null before.
    const netplay::IdentityBridge* EmberBridge(const ShellView& view) const;
    // The trusted service the Connect Discord screen is for: the one its link
    // named, or Ember's own; null until the player trusts it.
    const netplay::IdentityBridge* ConnectTarget(const ShellView& view) const;
    void ConnectRows(const ShellView& view, std::vector<MenuEntry>& rows, bool busy) const;
    // Connect Discord's next request once the service list is known.
    void ConnectNext(const ShellView& view);
    // The press of Connect Discord: runs the steps from wherever they stand.
    void ConnectGo(const ShellView& view);
    // Ember's own service, found by its look-up, may be trusted now; the
    // player has not removed it (the helper remembers removals).
    bool EmberOffered() const;
    bool Removed(const ShellView& view, const std::string& bridge) const;
    // Opens Discord's page for `bridge`, noting the account connected now.
    void OpenSignIn(const std::string& bridge);
    // Stops the attempt: what it queued and has not sent goes too, and an
    // answer already out no longer advances it.
    void CancelAttempt();
    // Connect Discord's own rows: true when `a` was one of them.
    bool ConnectActivate(const MenuAction& a, const ShellView& view);
    bool Active() const { return attempt_ == Attempt::Setup || attempt_ == Attempt::Opening || attempt_ == Attempt::Waiting; }
    // Connect Discord starts over: a sign-in an earlier visit opened is no
    // longer waited for, and its late answers start no wait.
    void NewJourney();
    // The answer in flight is a read or Connect from a journey since replaced.
    bool Superseded() const;

    std::deque<Queued> queue_;
    std::uint64_t nextTicket_ = 0, sent_ = 0;
    netplay::IdentityOp sentOp_ = netplay::IdentityOp::None;
    bool sentLookUp_ = false;
    std::string sentAccount_;
    // The Connect Discord journey, and the one the request in flight was
    // queued in. While its answer is handled, what it queues belongs to that
    // journey too: an earlier journey's follow-up reads are not queued.
    std::uint64_t journey_ = 1, sentJourney_ = 1;
    bool finishing_ = false;
    double sentAt_ = 0, now_ = 0;
    bool onScreens_ = false;
    std::string lastScreen_;
    std::string message_;
    bool messageError_ = false;
    double messageUntil_ = 0;
    // What the player typed. The passphrase fields are secrets.
    std::string newPassphrase_, newConfirm_, backupPassphrase_, backupConfirm_, restorePassphrase_;
    // The address starts as Ember's own tournament service, the one most
    // players trust; any other can be typed over it.
    std::string restorePath_, origin_ = EmberService, code_;
    // The selected service and site, and the service whose links the view lists.
    std::string bridge_, connection_, listedBridge_, sentBridge_, sentOrigin_;
    // The Discord account last read from each service: its user ID (empty
    // when none is connected) and name, whether it was ever read, and
    // whether the latest read of the service or its account failed (then
    // Connect Discord trusts none of it until Try again succeeds).
    struct DiscordAccount {
        std::string user, name;
        bool read = false, failed = false;
    };
    std::map<std::string, DiscordAccount> discord_;
    // The service a connect link named; empty for Ember's own. Whether this
    // visit already looked Ember's own service up.
    std::string connectBridge_;
    bool connectLookedUp_ = false;
    // The Connect Discord attempt on the journey's service: running its steps
    // by itself (Setup), opening Discord's page (Opening), waiting for the
    // account while the page is open (Waiting), or stopped by a failure,
    // Cancel or its deadline (Stopped), which only Connect or Try again
    // restarts. A link for the same service shows it rather than starting
    // another.
    enum class Attempt { None, Setup, Opening, Waiting, Stopped };
    Attempt attempt_ = Attempt::None;
    // The account connected when Discord's page was opened: only another one
    // (a first one, or a change of account) finishes the attempt.
    std::string attemptBefore_;
    // While Discord's page is open in the browser: the service the sign-in
    // is for, until when it can finish, and when its account is read again.
    // A failed read pauses the polls until Try again; the sign-in still
    // counts until its deadline.
    std::string discordWaitBridge_;
    double discordWaitUntil_ = 0, discordPollAt_ = 0;
    bool discordPaused_ = false;
    // The backup the view's preview describes, while the path still names it.
    std::string previewPath_;
    // A service the player looked up and may now trust.
    netplay::IdentityBridge found_;
    // Away from these screens, Home learns the Ember ID's state and the
    // services (again after a failure, quietly), then the matches every
    // minute: whether the services are known, when to read next, and the
    // matches told.
    bool servicesKnown_ = false;
    double backgroundAt_ = 0, assignmentsAt_ = 0;
    std::vector<std::string> told_;
    // The assignment list is wanted for the selected service; a Play or Stop
    // waiting to be sent; and what the last refresh said, to report a failure once.
    bool wantAssignments_ = false, loadingAssignments_ = false;
    std::optional<netplay::tournament::Command> play_;
    std::string assignmentsError_;
    netplay::tournament::Phase phase_ = netplay::tournament::Phase::Idle;
    // The match a link named, until its row is focused or found missing: its
    // service, its row, and the refreshes finished before its own was sent.
    struct OpenedMatch {
        std::string bridge, row;
        std::optional<std::uint64_t> finishedBefore;
    };
    std::optional<OpenedMatch> opened_;
};
} }
