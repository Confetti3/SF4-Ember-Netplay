#pragma once
#ifndef SF4E_EMBER_SERVICE
#define SF4E_EMBER_SERVICE "https://bridge.embernetplay.link"
#endif
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
#include <tuple>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
struct ShellAction;
enum class Tone;

// The Ember ID screens: the identity itself, its backup and restore, the
// accounts linked through tournament services, the matches those services
// assigned, which the player can play from here, Connect Discord, which
// takes a player from no Ember ID to a connected Discord account step by
// step, and the Connect screens of the sign-in sites Ember's own service
// lists (Challonge, start.gg). It holds the passphrases the
// player types until they are sent, and wipes them when the player leaves
// these screens. One request is in flight at a time; the rest wait in order.
class IdentityPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    // Ember's own tournament service, the one most players trust.
    // Ember's own service, whose sign-in sites Linked accounts shows. A test
    // build may name another (CMake SF4E_EMBER_SERVICE), such as the
    // tournament desk's nightly bridge.
    static constexpr const char* EmberService = SF4E_EMBER_SERVICE;
    static bool Owns(const std::string& screen);
    // Why a tournament match stopped, from its stable code, in words.
    static std::string TournamentFailure(const std::string& code);
    // A match link named this match: the matches screen selects its service,
    // refreshes the list and focuses the match's Play row. `playNow`: the
    // player just clicked or pasted the link while free, so the match plays by
    // itself once the list shows it playable, while the matches screen is
    // shown: for a browser link (`linkSequence`, its OpenedLink::sequence) as
    // long as the runtime says it is direct, for a pasted one LinkPlaySeconds.
    // A service the player does not trust, or a match not in their list, is
    // said instead.
    void OpenMatch(const std::string& bridge, const std::string& match, bool playNow = false, std::uint64_t linkSequence = 0);
    // Whether a match link arriving now may play its match by itself: no room,
    // no game, and no tournament match played or watched (OpenMatch's playNow).
    static bool LinkMayPlay(const ShellView& v);
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
    // Hiding Ember stops a Connect Discord or a sign-in site's setup, as
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
    // Which accounts a service's profile leads to reading: none, Discord's,
    // Discord's and every sign-in site's it lists, or one site's.
    struct AccountReads {
        enum class Of { None, Discord, Everything, OneSite };
        Of of = Of::None;
        std::string site;
        static AccountReads Discord() { return {Of::Discord, {}}; }
        static AccountReads Everything() { return {Of::Everything, {}}; }
        static AccountReads OneSite(std::string site) { return {Of::OneSite, std::move(site)}; }
        bool ReadsDiscord() const { return of == Of::Discord || of == Of::Everything; }
        bool ReadsSites() const { return of == Of::Everything || of == Of::OneSite; }
        bool Reads(const std::string& id) const { return of == Of::Everything || (of == Of::OneSite && site == id); }
        bool operator==(const AccountReads& o) const { return of == o.of && site == o.site; }
    };
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
        // An inspection only to read these accounts on this service, which
        // leaves the selected service as it is.
        std::string account;
        AccountReads reads;
        Owner owner = Owner::Screens;
    };
    bool Busy(const ShellView& view) const;
    bool Answered(const ShellView& view) const;
    void Finish(const ShellView& view);
    // The answer to the setup's own request in flight.
    void SetupFinish(const ShellView& view);
    void Queue(netplay::IdentityRequest request, bool lookUp = false, std::string account = {}, AccountReads reads = {});
    void QueueSetup(netplay::IdentityRequest request);
    // A request of the setup's is waiting or out.
    bool SetupPending() const;
    // The match `id` in the selected service's list is one the player can play now.
    bool CanPlay(const ShellView& view, const std::string& id) const;
    // The read in flight failed: its service's accounts are not trusted, and a
    // sign-in waiting on it pauses its polls.
    void ReadFailed();
    // Reads the accounts `reads` names on `bridge` (its profile first),
    // leaving the selected service as it is.
    void ReadAccount(const netplay::IdentityBridge& bridge, AccountReads reads);
    // A service's profile was read: each account `reads` names that it keeps
    // is asked for, and the sign-in sites it lists are noted.
    void ReadSiteAccounts(const netplay::IdentityView& id, const std::string& bridge, const AccountReads& reads);
    // Reading those accounts failed: none of them is trusted.
    void AccountReadsFailed(const std::string& bridge, const AccountReads& reads);
    // The account last read from `bridge` as the row's value: its name, or
    // Not connected; nothing while it is unread or its latest read failed.
    void ShowAccount(MenuEntry& row, const SignInTarget& target, const std::string& bridge) const;
    // Drops the requests waiting to be sent after the one in flight failed,
    // unless it came from an earlier journey: those are not its to drop.
    void DropQueue();
    void Collect(const ShellView& view, double now);
    void Send(const ShellView& view, const Submit& submit, double now);
    void Say(std::string text, Tone tone, double seconds = 6);
    void Wipe();
    void Refresh(const ShellView& view, const std::string& screen);
    void SelectBridge(const ShellView& view, const std::string& bridge);
    // Sends a waiting Play or Stop, or the assignment refresh the matches screen wants.
    void SendTournament(const ShellView& view, const std::string& screen, const Submit& submit);
    // The tournament matches screen's rows, and its own rows' presses (IdentityMatches.cxx).
    void MatchesRows(const ShellView& view, std::vector<MenuEntry>& rows) const;
    bool MatchesActivate(const MenuAction& a, const ShellView& view);
    // The tournament desk's screens (IdentityDesk.cxx): shown while the Ember
    // ID holds organizer access on Ember's own service; their rows, presses
    // and Left/Right, and the read every few seconds while one is open.
    static bool DeskScreen(const std::string& screen);
    bool DeskHeld(const ShellView& view) const;
    // Whether Ember's own service offers organizer access on any site.
    bool DeskOffered(const ShellView& view) const;
    // Asks a listed site for organizer access, or drops it (IdentitySignIn.cxx).
    void OrganizerAccess(const ShellView& view, const std::string& site, bool allow);
    const netplay::DeskSet* DeskSelected(const ShellView& view) const;
    void DeskPoll(const ShellView& view, const std::string& screen);
    // The open event's state on its site, read on opening it and after a move.
    void DeskSiteRead(const ShellView& view);
    void DeskRows(const ShellView& view, const std::string& screen, std::vector<MenuEntry>& rows, std::string& title) const;
    bool DeskActivate(const MenuAction& a, const ShellView& view, MenuNavigation& navigation);
    bool DeskAccept(const MenuAction& a, const ShellView& view);
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
    // ending a sign-in under way on its service first.
    void StartSteps();
    // A Discord account was read: running steps open Discord's page for a
    // service with none connected, and stop for one already connected.
    void ConnectAccountRead(const ShellView& view);

    // What Discord and the listed sign-in sites share (IdentitySignIn.cxx).
    // A listed site by its ID on `bridge`, as the service's profile last listed it; null when not.
    const netplay::IdentitySignInSite* ListedSite(const std::string& bridge, const std::string& site) const;
    // A listed site's name to show: as its service last named it, even once
    // it no longer lists it, or its ID.
    std::string SiteName(const std::string& site) const;
    // One of the target's words: Discord's as they are, a listed site's with
    // its name as {0}; `value` follows as the next placeholder.
    std::string Word(const SignInTarget& target, const char* id) const;
    std::string Word(const SignInTarget& target, const char* id, const std::string& value) const;
    // The target of the sign-in request in flight.
    SignInTarget SentTarget() const;
    // The service the target's rows connect on: Ember's own, or the selected one.
    std::string SiteBridge(const ShellView& view, const SignInTarget& target) const;
    // `screen` is the target's Connect screen.
    bool AtScreenOf(const SignInTarget& target, const std::string& screen) const;
    // Opens the target's page for `bridge`, noting the account connected now.
    // Another sign-in under way ends on its service first.
    void OpenSignIn(const SignInTarget& target, const std::string& bridge);
    // Stops the attempt: what it queued and has not sent goes too, and an
    // answer already out no longer advances it.
    void CancelAttempt();
    // Ends another sign-in under way on its service, before `target` starts one.
    void EndOtherSignIn(const SignInTarget& target);
    // A new Connect Discord journey starts: what becomes of the sign-in under way.
    void RetireSignIn();
    // A site's own words for a refusal of the sign-in request in flight, where
    // it has them (over its request limits, or no longer on the service);
    // none otherwise.
    std::optional<std::string> SiteFailureText(const std::string& code) const;
    // Connect Discord's steps run, or a sign-in's page is being opened.
    bool Starting() const;
    // The target has steps or a sign-in under way, which Cancel stops.
    bool ActiveFor(const SignInTarget& target) const;
    // The target's sign-in on `bridge` waits for the browser, and its last read worked.
    bool WaitingOn(const SignInTarget& target, const std::string& bridge) const;
    // The request in flight read a site's account that is said only on its
    // own Connect screen, and that screen is not showing.
    bool QuietRead() const;
    // The site's account read in flight failed: its account is not trusted,
    // and a sign-in waiting on it pauses its polls.
    void SiteReadFailed();
    // The service refused a site's request: its sign-in is off now, or (on
    // Connect) it no longer has the site. The site's screen shows it off
    // without reading the list again.
    void SiteRefused(const std::string& code);
    // A listed site its service no longer has: its row and what was read of
    // it go, and a sign-in on it ends. Its name is kept for its screen.
    void DropSite(const std::string& site);
    // The Ember ID screen's rows on Ember's own service: Discord's, and one
    // per sign-in site it lists.
    MenuEntry DiscordIdentityRow(const ShellView& view) const;
    void ListedSiteIdentityRows(const ShellView& view, std::vector<MenuEntry>& rows) const;
    // The site on `target`: waiting for the browser, connected, off, or Connect.
    void SignInRows(const ShellView& view, const SignInTarget& site, const netplay::IdentityBridge& target, std::vector<MenuEntry>& rows, bool busy) const;
    // A listed site's Connect screen (siteScreen_), for Ember's own service.
    void ListedSiteRows(const ShellView& view, std::vector<MenuEntry>& rows, bool busy) const;
    // Connect, Unlink, Cancel and Try again for Discord or a listed site: true when `a` was one of them.
    bool SignInActivate(const MenuAction& a, const ShellView& view);
    // The answer to a site's read, Connect or Disconnect.
    void SignInFinish(const ShellView& view);
    // While the site's page is open, its account is read again every few
    // seconds on the screens that show it.
    void SignInPoll(const std::string& screen, double now);
    // `screen` shows the account the attempt waits for.
    bool ShowsWait(const std::string& screen) const;
    // Connect Discord starts over: Discord's sign-in from an earlier visit is
    // no longer waited for, and its late answers start no wait. A site's
    // sign-in waiting for the browser goes on (RetireSignIn).
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
    std::string sentAccount_, sentSite_;
    bool sentOrganizer_ = false;
    AccountReads sentReads_;
    // The Connect Discord journey, and the one the request in flight was
    // queued in. While its answer is handled, what it queues belongs to that
    // journey too: an earlier journey's follow-up reads are not queued.
    std::uint64_t journey_ = 1, sentJourney_ = 1;
    bool finishing_ = false;
    double sentAt_ = 0, now_ = 0;
    bool onScreens_ = false;
    std::string lastScreen_;
    std::string message_;
    Tone messageTone_{};
    double messageUntil_ = 0;
    // What the player typed. The passphrase fields are secrets.
    std::string newPassphrase_, newConfirm_, backupPassphrase_, backupConfirm_, restorePassphrase_;
    // The address starts as Ember's own tournament service, the one most
    // players trust; any other can be typed over it.
    std::string restorePath_, origin_ = EmberService, code_;
    // The selected service and site, and the service whose links the view lists.
    std::string bridge_, connection_, listedBridge_, sentBridge_, sentOrigin_;
    // Each account last read, by Discord or listed site and service: its
    // user ID (empty when none is connected), name and when it connected,
    // whether it was ever read, and whether the latest read of the service
    // or the account failed (then the Connect screen trusts none of it until
    // Try again succeeds).
    struct SiteAccount : netplay::IdentityAccount {
        bool read = false, failed = false;
        // Whether the Ember ID holds the site's organizer grant, as last read.
        bool organizer = false;
        void Set(const netplay::IdentityAccount& now) { IdentityAccount::operator=(now); read = true; failed = false; }
    };
    std::map<std::tuple<SignInSource, std::string, std::string>, SiteAccount> accounts_;
    // The desk's event and set on screen, the queue's filter (IdentityDesk.cxx
    // Filters) or the bracket, and when the desk is read next.
    std::string deskEvent_, deskSet_;
    // The other device's code typed on desk-device, until it is approved.
    std::string deskCode_;
    int deskFilter_ = 0;
    bool deskBracket_ = false;
    double deskReadAt_ = 0;
    // A move was sent: its answer reads the event's site state again.
    bool deskMoved_ = false;
    SiteAccount& Account(const SignInTarget& target, const std::string& bridge);
    const SiteAccount* FindAccount(const SignInTarget& target, const std::string& bridge) const;
    // The sign-in sites Ember's own service (sitesBridge_) lists, as its
    // profile last said; the name it gave each, kept after it stops listing
    // one so the site's screen still names it; and the site the listed
    // site's Connect screen shows.
    std::string sitesBridge_;
    std::vector<netplay::IdentitySignInSite> sites_;
    std::map<std::string, std::string> siteNames_;
    std::string siteScreen_;
    // The service a connect link named, empty for Ember's own, and the one
    // Discord's page last opened for in this journey: a link for either
    // shows the journey rather than starting it again. Whether this visit
    // already looked Ember's own service up.
    std::string connectBridge_, discordService_;
    bool connectLookedUp_ = false;
    // The sign-in under way, Discord's or a listed site's, one at a time
    // (IdentitySignIn.cxx): the site and service it is for, the account
    // connected when its page was opened (only a connection since then
    // finishes it, another account's or the same one's again), and its phase.
    // Steps: Connect Discord's own steps up to Discord's page
    // (IdentityConnect.cxx), Discord only and with no service yet. Opening:
    // its page is being opened. Waiting: the page is open and waited for.
    // Stopped: by a failure, Cancel or its deadline; only Connect or Try
    // again starts it again. Its wait for the browser: until when it can
    // finish (0 when none runs), when its account is read next, and whether
    // a failed read paused the reads until Try again; a new page for the same
    // site and service keeps the wait running. Replaced as a whole, never
    // edited field by field into another site's or service's sign-in.
    struct SignInAttempt {
        enum class Phase { Steps, Opening, Waiting, Stopped };
        SignInTarget target;
        std::string bridge;
        netplay::IdentityAccount before;
        Phase phase = Phase::Opening;
        double until = 0, pollAt = 0;
        bool paused = false;
    };
    std::optional<SignInAttempt> signIn_;
    // An organizer sign-in whose page opened: its site and service, and until
    // when (and next when) its grant is read again while its screen is open.
    struct OrganizerWait { std::string site, bridge; double until = 0, pollAt = 0; };
    std::optional<OrganizerWait> organizerWait_;
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
        std::string match;
    };
    std::optional<OpenedMatch> opened_;
    // The match a fresh link plays by itself, and until when (OpenMatch).
    struct LinkedPlay { std::string match; double until = 0; std::uint64_t linkSequence = 0; };
    std::optional<LinkedPlay> linkedPlay_;
    // The match a link named that the player may only watch: it is not in their
    // list, or its service is not trusted. Its row stays until another link opens.
    std::string watchBridge_, watchMatch_;
    // The player can start watching a match now (CanWatch).
    bool CanWatch(const ShellView& v) const;
    // The row for the match a link named that the player may only watch: Watch itself, or why
    // it cannot be used yet. None when no link named one, or it names another service than the one shown.
    std::optional<MenuEntry> WatchEntry(const ShellView& v) const;
    // How the watch is going, and Stop while it is under way (IdentityPanel__Watch.cxx).
    void AppendWatchState(const ShellView& v, std::vector<MenuEntry>& rows) const;
    // True when `action` is Watch on the Watch row, which then starts the watch if it still can.
    bool ActivateWatch(const MenuAction& action, const ShellView& v);
};
} }
