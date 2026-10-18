#pragma once
#include "MenuNavigation.hxx"
#include "MatchAnnouncements.hxx"
#include "PublicRoomsSetup.hxx"
#include "SignInSites.hxx"
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/IdentityView.hxx"
#include "../netplay/TournamentStatus.hxx"
#include <optional>
#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
struct ShellAction;
enum class Tone;

// The Ember ID screens: the identity itself, its backup and restore, the
// accounts linked through tournament services, the matches those services
// assigned, which the player can play from here, Connect Discord, which
// takes a player from no Ember ID to a connected Discord account step by
// step, and Connect Challonge. It holds the passphrases the
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
    // The service the Ember ID screens have selected, when the ID is ready and
    // the service is one the player trusts; empty otherwise. Public rooms use it.
    std::string UsableBridge(const ShellView& view) const;
    // Asks for the ID's status and the trusted services, for a screen outside
    // these that needs to know them.
    void Probe();
    // The panel is waiting for an answer from the helper.
    bool Waiting() const { return sent_ != 0 || !queue_.empty(); }
    // Public rooms, in one press: reads the Ember ID, creates it when nothing must
    // be typed, finds Ember's own service and trusts it. Where the setup stands is
    // PublicSetup(); the player's screens say it, since this panel's own status
    // line is silent off the Ember ID screens. The setup's requests are its own,
    // and so are their answers (see Owner).
    void SetUpPublicRooms(const ShellView& view);
    PublicSetupView PublicSetup() const { return setup_.View(); }
    // The setup's end (Done, NeedsId or Failed) has been shown.
    void ClearPublicSetup() { setup_.Clear(); }
    // The row the matches screen should focus, once it is among `rows`.
    std::string TakeFocus(const std::vector<MenuEntry>& rows);
    // The Ember ID row's line (on Online play): where to start, how many
    // matches are ready to play, or what the screens hold.
    std::string EntryDetail(const ShellView& view) const;
    // Settings > Discord's Tournament account row: the Discord account on the
    // selected service, read again each time that screen opens, or Not
    // connected without a ready Ember ID or a service. It opens Connect
    // Discord for that service, or for Ember's own without one.
    MenuEntry TournamentAccountRow(const ShellView& view) const;
    void OpenTournamentAccount(const ShellView& view, MenuNavigation& navigation);
    // Tournament matches ready to play on the service the matches are read
    // from; 0 without a ready ID and a service. Home shows a row while any are.
    int ReadyMatches(const ShellView& view) const;
    // A playable match the player has not been told about yet: true once for
    // each new one, except while the matches screen already shows them, unless
    // a match that ended for a lost connection is being set up again (see
    // MatchAnnouncements, which remembers them across restarts).
    bool TakeAssigned(const ShellView& view);
    // Where the matches told are kept; the platform store unless a test supplies its own.
    using AnnouncedLoader = MatchAnnouncements::Loader;
    using AnnouncedSaver = MatchAnnouncements::Saver;
    void SetAnnouncedStore(AnnouncedLoader loader, AnnouncedSaver saver) { announced_.SetStore(std::move(loader), std::move(saver)); }
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
    // Hiding Ember stops a Connect Discord or Connect Challonge setup, as
    // leaving its screen does; a wait for the browser goes on.
    // Ember is hidden (after Conceal): the answer in flight is still taken,
    // and a cancelled sign-in is still ended on its service.
    void Hidden(const ShellView& view, const Submit& submit, double now);
    void Conceal() {
        if (Starting()) CancelAttempt();
        Wipe(); queue_.clear(); play_.reset(); onScreens_ = false; lastScreen_.clear(); setup_.Stop();
    }
private:
    // Whose answer a request's is. The Ember ID screens, Connect Discord and
    // Home's reads in the background are one flow, whose answer is handled for
    // whichever of them is open when it comes (Finish). The one-press setup of
    // public rooms owns its requests: only it moves on their answers, which no
    // screen sees, and it sees no other answer (SetupFinish).
    enum class Owner { Screens, PublicSetup };
    // A request waiting to be sent. A look-up's answer is a service the
    // player may trust, not the selected one's profile. Every request names
    // the run of its owner it was queued in: for the screens, the Connect
    // Discord journey; once a new journey starts, an earlier read (or
    // Connect) is retired without effect, and an earlier change still
    // finishes but leaves the new journey's requests. For the setup, its own
    // run, which a restart or hiding Ember retires.
    struct Queued {
        netplay::IdentityRequest request;
        bool lookUp = false;
        std::uint64_t journey = 0;
        // An inspection only to read these sites' accounts on this service,
        // which leaves the selected service as it is.
        std::string account;
        SignInSiteSet sites = 0;
        Owner owner = Owner::Screens;
    };
    bool Busy(const ShellView& view) const;
    bool Answered(const ShellView& view) const;
    void Finish(const ShellView& view);
    // The answer to the setup's own request in flight.
    void SetupFinish(const ShellView& view);
    void Queue(netplay::IdentityRequest request, bool lookUp = false, std::string account = {}, SignInSiteSet sites = 0);
    void QueueSetup(netplay::IdentityRequest request);
    // A request of the setup's is waiting or out.
    bool SetupPending() const;
    // The match `id` in the selected service's list is one the player can play now.
    bool CanPlay(const ShellView& view, const std::string& id) const;
    // The read in flight failed: its service's accounts are not trusted, and a
    // sign-in waiting on it pauses its polls.
    void ReadFailed();
    // Reads the accounts of `sites` on `bridge` (its profile first), leaving
    // the selected service as it is.
    void ReadAccount(const netplay::IdentityBridge& bridge, SignInSiteSet sites);
    // A service's profile was read: each of `sites` it keeps accounts for is
    // asked for the account.
    void ReadSiteAccounts(const netplay::IdentityView& id, const std::string& bridge, SignInSiteSet sites);
    // The site's account last read from `bridge` as the row's value: its
    // name, or Not connected; nothing while it is unread or its latest read failed.
    void ShowAccount(MenuEntry& row, SignInSite site, const std::string& bridge) const;
    // Drops the requests waiting to be sent after the one in flight failed,
    // unless it came from an earlier journey: those are not its to drop.
    void DropQueue();
    void Collect(const ShellView& view, double now);
    void Send(const ShellView& view, const Submit& submit, double now);
    void Say(std::string text, bool error, double seconds = 6);
    void Wipe();
    void Refresh(const ShellView& view, const std::string& screen);
    void SelectBridge(const ShellView& view, const std::string& bridge);
    // Sends a waiting Play or Stop, or the assignment refresh the matches screen wants.
    void SendTournament(const ShellView& view, const std::string& screen, const Submit& submit);
    // The tournament matches screen's rows, and its own rows' presses (IdentityMatches.cxx).
    void MatchesRows(const ShellView& view, std::vector<MenuEntry>& rows) const;
    bool MatchesActivate(const MenuAction& a, const ShellView& view);
    // Says so when the list a link's refresh brought back lacks its match.
    void CheckOpenedMatch(const netplay::tournament::AssignmentList& list);
    // Reads the service list again after the helper answered for a dormant
    // built-in service, which that use made one of the player's.
    void ReadIfFirstUse(const ShellView& view, const std::string& bridge);
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
    // Connect Discord's own rows: true when `a` was one of them.
    bool ConnectActivate(const MenuAction& a, const ShellView& view);
    // Connect Discord's steps start, or start again, from wherever they stand,
    // ending another site's sign-in first.
    void StartSteps();
    // Connect Discord's steps run, which only Discord has.
    bool StepsRunFor(SignInSite site) const;
    // A Discord account was read: running steps open Discord's page for a
    // service with none connected, and stop for one already connected.
    void ConnectAccountRead(const ShellView& view);

    // What Discord and Challonge share (IdentitySignIn.cxx).
    // The service the site's rows connect on: Ember's own, or the selected one.
    std::string SiteBridge(const ShellView& view, SignInSite site) const;
    // Opens the site's page for `bridge`, noting the account connected now.
    // Another site's sign-in under way ends on its service first.
    void OpenSignIn(SignInSite site, const std::string& bridge);
    // Stops the attempt: what it queued and has not sent goes too, and an
    // answer already out no longer advances it.
    void CancelAttempt();
    // Ends another site's sign-in under way on its service, before `site` starts one.
    void EndOtherSignIn(SignInSite site);
    // Connect Discord's steps run, or a site's page is being opened.
    bool Starting() const;
    // The site has steps or a sign-in under way, which Cancel stops.
    bool ActiveFor(SignInSite site) const;
    // The site's sign-in on `bridge` waits for the browser, and its last read worked.
    bool WaitingOn(SignInSite site, const std::string& bridge) const;
    // The request in flight read a site's account that is said only on its
    // own Connect screen, and that screen is not showing.
    bool QuietRead() const;
    // The site's account read in flight failed: its account is not trusted,
    // and a sign-in waiting on it pauses its polls.
    void SiteReadFailed();
    // The Ember ID screen's row for the site, on Ember's own service.
    std::optional<MenuEntry> SiteRow(const ShellView& view, SignInSite site) const;
    // The site on `target`: waiting for the browser, connected, off, or Connect.
    void SignInRows(const ShellView& view, SignInSite site, const netplay::IdentityBridge& target, std::vector<MenuEntry>& rows, bool busy) const;
    // Connect Challonge's screen, for Ember's own service.
    void ChallongeRows(const ShellView& view, std::vector<MenuEntry>& rows, bool busy) const;
    // Connect, Disconnect, Cancel and Try again for either site: true when `a` was one of them.
    bool SignInActivate(const MenuAction& a, const ShellView& view);
    // The answer to a site's read, Connect or Disconnect.
    void SignInFinish(const ShellView& view);
    // While the site's page is open, its account is read again every few
    // seconds on the screens that show it.
    void SignInPoll(const std::string& screen, double now);
    // `screen` shows the account the attempt waits for.
    bool ShowsWait(const std::string& screen) const;
    // Connect Discord starts over: a sign-in an earlier visit opened is no
    // longer waited for, and its late answers start no wait.
    void NewJourney();
    // The answer in flight is a read or Connect from a journey since replaced.
    bool Superseded() const;

    std::deque<Queued> queue_;
    // Cancelled sign-ins to end on their services, sent before anything
    // else. Held apart from the queue, which failures and hiding clear.
    std::deque<netplay::IdentityRequest> cleanup_;
    std::uint64_t nextTicket_ = 0, sent_ = 0;
    netplay::IdentityOp sentOp_ = netplay::IdentityOp::None;
    Owner sentOwner_ = Owner::Screens;
    bool sentLookUp_ = false;
    std::string sentAccount_;
    SignInSiteSet sentSites_ = 0;
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
    // Each site's account last read from each service: its user ID (empty
    // when none is connected) and name, whether it was ever read, whether
    // the latest read of the service or its account failed (then the Connect
    // screen trusts none of it until Try again succeeds), and whether the
    // service keeps the site's accounts, as its profile last said.
    struct SiteAccount {
        std::string user, name;
        bool read = false, failed = false, kept = false;
    };
    std::map<std::string, SiteAccount> accounts_[std::size(SignInSites)];
    std::map<std::string, SiteAccount>& Accounts(SignInSite site) { return accounts_[static_cast<std::size_t>(site)]; }
    const std::map<std::string, SiteAccount>& Accounts(SignInSite site) const { return accounts_[static_cast<std::size_t>(site)]; }
    // The service a connect link named; empty for Ember's own. Whether this
    // visit already looked Ember's own service up.
    std::string connectBridge_;
    bool connectLookedUp_ = false;
    // Connect Discord's own steps up to Discord's page (IdentityConnect.cxx):
    // running by themselves, or stopped by a failure or Cancel, which only
    // Connect or Try again restarts. A link for the same service shows them
    // rather than starting again.
    // Invariant: steps_ and signIn_ never both hold an attempt. The steps
    // start with no sign-in (StartSteps), and a sign-in starts with no steps
    // (OpenSignIn); Starting() asserts it.
    enum class Steps { None, Running, Stopped };
    Steps steps_ = Steps::None;
    // A site's sign-in in the browser, one at a time (IdentitySignIn.cxx):
    // the site and service it is for, the account connected when its page
    // was opened (only another one finishes it), and whether the page is
    // being opened, is open and waited for, or the sign-in stopped by a
    // failure, Cancel or its deadline. Its wait for the browser: until when
    // it can finish (0 when none runs), when its account is read next, and
    // whether a failed read paused the reads until Try again; a new page for
    // the same site and service keeps the wait running. Replaced as a whole,
    // never edited field by field into another site's or service's sign-in.
    struct SignInAttempt {
        enum class Phase { Opening, Waiting, Stopped };
        SignInSite site = SignInSite::Discord;
        std::string bridge, before;
        Phase phase = Phase::Opening;
        double until = 0, pollAt = 0;
        bool paused = false;
    };
    std::optional<SignInAttempt> signIn_;
    // The backup the view's preview describes, while the path still names it.
    std::string previewPath_;
    // A service the player looked up and may now trust.
    netplay::IdentityBridge found_;
    PublicRoomsSetup setup_;
    // Away from these screens, Home learns the Ember ID's state and the
    // services (again after a failure, quietly), then the matches every
    // minute: whether the services are known, when to read next, and the
    // matches told.
    bool servicesKnown_ = false;
    double backgroundAt_ = 0, assignmentsAt_ = 0;
    // Until when the matches are watched for one the bridge sets up again
    // after a lost connection (RestartWatchSeconds).
    double restartWatchUntil_ = 0;
    MatchAnnouncements announced_;
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
