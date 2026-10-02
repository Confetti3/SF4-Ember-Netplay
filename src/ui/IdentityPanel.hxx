#pragma once
#include "MenuNavigation.hxx"
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/IdentityView.hxx"
#include "../netplay/TournamentStatus.hxx"
#include <optional>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
struct ShellAction;
enum class Tone;

// The Ember ID screens: the identity itself, its backup and restore, the
// accounts linked through tournament services, and the matches those
// services assigned, which the player can play from here. It holds the passphrases the
// player types until they are sent, and wipes them when the player leaves
// these screens. One request is in flight at a time; the rest wait in order.
class IdentityPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static bool Owns(const std::string& screen);
    // Why a tournament match stopped, from its stable code, in words.
    static std::string TournamentFailure(const std::string& code);
    // A match link named this match: the matches screen selects its service,
    // refreshes the list and focuses the match's row, for the player to press
    // Play. A service the player does not trust, or a match not in their
    // list, is said instead.
    void OpenMatch(const std::string& bridge, const std::string& match);
    // The row the matches screen should focus, once it is among `rows`.
    std::string TakeFocus(const std::vector<MenuEntry>& rows);
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
    void Conceal() { Wipe(); queue_.clear(); play_.reset(); onScreens_ = false; lastScreen_.clear(); }
private:
    bool Busy(const ShellView& view) const;
    bool Answered(const ShellView& view) const;
    void Finish(const ShellView& view);
    void Queue(netplay::IdentityRequest request);
    void Say(std::string text, bool error, double seconds = 6);
    void Wipe();
    void Refresh(const ShellView& view, const std::string& screen);
    void SelectBridge(const ShellView& view, const std::string& bridge);
    // Sends a waiting Play or Stop, or the assignment refresh the matches screen wants.
    void SendTournament(const ShellView& view, const std::string& screen, const Submit& submit);
    // Says so when the list a link's refresh brought back lacks its match.
    void CheckOpenedMatch(const netplay::tournament::AssignmentList& list);
    MenuEntry ServiceRow(const ShellView& view, const netplay::IdentityBridge& bridge) const;
    MenuEntry DiscordRow(const ShellView& view, bool busy) const;

    std::deque<netplay::IdentityRequest> queue_;
    std::uint64_t nextTicket_ = 0, sent_ = 0;
    netplay::IdentityOp sentOp_ = netplay::IdentityOp::None;
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
    std::string restorePath_, origin_ = "https://bridge.embernetplay.link", code_;
    // The selected service and site, and the service whose links the view lists.
    std::string bridge_, connection_, listedBridge_, sentBridge_;
    // The service the view's Discord account was last read from.
    std::string discordBridge_;
    // The backup the view's preview describes, while the path still names it.
    std::string previewPath_;
    // A service the player looked up and may now trust.
    bool lookingUp_ = false;
    netplay::IdentityBridge found_;
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
