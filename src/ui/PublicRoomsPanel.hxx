#pragma once
#include "IdentityPanel.hxx"
#include "MenuNavigation.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/TournamentStatus.hxx"
#include "../common/RoomRules.hxx"
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
struct ShellAction;
enum class Tone;

// The bridge's public rooms: the list to browse, and the one step from a
// create or a pressed room to joining it. A request for a ticket or a new room
// is answered with an admission, which the panel joins with at once. It needs
// the Ember ID and a service the player trusts, which the Ember ID panel
// selects, so it works from the service it is given. A room link names its own
// service and room: the panel asks that service for that room's ticket, like a
// pressed room, once the Ember ID has answered and the service is one the
// player trusts, and shows that service's list until the player leaves.
// The list is shown sorted (rooms that can be joined first, near ones first)
// and filtered, and refreshes itself while the screen is open and idle.
class PublicRoomsPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    // Which rooms are listed; kept for the session, not saved.
    enum class Filter { All, Near, Seats };
    // What the room being opened is: one joined from the list or a link, or one just created.
    enum class OpenKind { Join, Create };
    // What a room's card shows besides the room: nothing, its ticket being asked
    // for, or the room being opened (the pr-opening card).
    enum class CardPhase { Idle, Joining, Opening };
    // The list is asked for again this long after the last answer, and this long
    // after one that failed.
    static constexpr double AutoRefreshSeconds = 15, ErrorBackoffSeconds = 30;
    static bool Owns(const std::string& screen) { return screen == "public-rooms"; }
    // A refused request or a failed refresh, by its stable code, in words.
    static std::string FailureText(const std::string& code);
    // The region of this PC's home relay, whose rooms are near; empty while it is unknown.
    static std::string KnownRegion(const ShellView& view);
    // A region by its name, or its code in capitals when the name is not known.
    static std::string RegionLabel(const std::string& code);
    // The setup card's line: the step while the setup runs, why it stopped once
    // it failed, empty otherwise. The row's value and the painted card both use it.
    static std::string SetupLine(const PublicSetupView& setup);
    // Every frame, before the rows: asks for the list on entering the screen
    // and now and then while it stays, sends what is waiting, and joins with an
    // admission that arrives. `bridge` is the usable service, empty without
    // one; `identityPending` says the Ember ID is still being asked about.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge, bool identityPending,
                const Submit& submit, double now);
    // A room link opened from the browser, or pasted. `direct`: the player was
    // free when it arrived, so it asks for the room by itself once on this
    // screen; otherwise it waits as a row until the player chooses it.
    void OpenLink(const std::string& bridge, const std::string& room, bool direct);
    // `bridge` and `identityPending` as for Update; `setup` is where the Ember ID
    // panel's one-press setup stands, which the setup card shows; `listWidth` is
    // the width of the list in pixels, which decides how tall a room's card is.
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& bridge, bool identityPending, const PublicSetupView& setup,
                                float listWidth) const;
    // True when the row was the panel's own.
    bool Activate(const MenuAction& action, MenuNavigation& navigation);
    // The option chosen on the filter row; true when it was the panel's own.
    bool Choose(const MenuAction& action);
    // The Options button or key (Y, T) asks for the list again; true when it was the panel's own.
    bool Shortcut(const MenuAction& action, const ShellView& view);
    // The screen shows a list that Options can refresh.
    bool Refreshable(const ShellView& view) const;
    // The room Quick join would ask for: the first listed one with a free seat that
    // is not locked and was not refused this visit, in `region` when it is known.
    std::optional<std::string> QuickPick(const ShellView& view, const std::string& region) const;
    // Asks for a room's ticket, as pressing its row does; false while a request is waiting.
    bool Join(const std::string& roomId);
    // Gives up waiting for the ticket or room in flight: its answer joins nothing.
    void Abandon();
    // Opens a public room: the new room's admission is joined when it arrives.
    // `rules` go with the new room's join, for the runtime to set on its tables.
    void Create(const std::string& name, int capacity, const room::Rules& rules);
    // A create or ticket request is waiting to be sent or answered.
    bool Busy() const { return pending_.has_value(); }
    // What a pending request or refresh says on `screen`, and the list's own
    // state once nothing else has anything to say: `status` is what other
    // reports left, and only an empty one takes the list's text.
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone, double now) const;
    // What the panel has to say, once, wherever the player is.
    bool TakeSaid(std::string& text);
    // The shell is hidden: an admission that arrives meanwhile must not join
    // later, so the request in flight is abandoned. A room link nobody has
    // followed yet is kept: it is the player's, until followed or replaced.
    // The room the player is in or opening is kept too.
    void Conceal();
    // A link that goes on by itself is waiting to be asked for.
    bool FollowingLink() const { return link_ && link_->direct; }
    // That link waits for the player's choice instead: they are busy now.
    void HoldLink() { if (link_) link_->direct = false; }
    // The public room this PC is opening or in, from the admission it joined
    // with until the room ends; empty kind when it is not in one.
    std::optional<OpenKind> OpeningKind() const { return current_ ? std::optional<OpenKind>(current_->kind) : std::nullopt; }
    // That room's page link, for sharing; empty when there is none.
    std::string RoomLink() const;
    // The room being opened or in, as its card shows it; null when there is none.
    const netplay::publicrooms::Room* CurrentRoom() const { return current_ ? &current_->room : nullptr; }
    // The phase of the card for row `id`: Opening for pr-opening, Joining for the
    // room whose ticket is pending, Idle for any other.
    CardPhase PhaseOf(const std::string& id) const;
    // How long `room` has been open as of `now` (the interface clock), from the
    // bridge's own times; negative when it sent none.
    double OpenSeconds(const ShellView& view, const netplay::publicrooms::Room& room, double now) const;
    // The shell opened some other room itself: whatever this panel joined is not it.
    void ForgetCurrent() { current_.reset(); }
    Filter filter() const { return filter_; }
private:
    // The one create or ticket request the panel will act on. It is identified
    // by Command::request, which the panel picks when it sends it; the runtime
    // answers under that identity and a newer request supersedes an older one.
    // The panel joins only an answer to the request it holds, so dropping it
    // (hiding the shell, leaving the screen) abandons whatever was in flight.
    struct Pending {
        netplay::tournament::Command command;
        bool sent = false;
        double sentAt = 0;
        // A create's chosen table rules.
        std::optional<room::Rules> createdRules;
    };
    // The room joined with an admission, kept while it is being opened and while
    // the player is in it. `seen`: the session has left Idle for it.
    struct Current {
        std::string bridge, roomId;
        OpenKind kind = OpenKind::Join;
        netplay::publicrooms::Room room;
        bool seen = false;
        double at = 0;
    };
    void Begin(netplay::tournament::Command command);
    void AskForLink(const ShellView& view, bool identityPending);
    void Joined(const ShellView& view, const Submit& submit, const Pending& done, double now);
    void Say(std::string text) { said_ = std::move(text); }
    // Asks for the list again for the player: Updating... shows while it is in flight.
    void Refresh() { wantList_ = true; manualRefresh_ = true; said_.clear(); }

    std::string bridge_, lastScreen_, said_, listError_;
    // The service the last finished refresh listed; the list on screen is for it only when it is the current one.
    std::string loadedFor_;
    // The service a trusted room link named, in place of the selected one on
    // these screens until the player leaves them.
    std::string service_;
    struct Link {
        std::string bridge, room;
        bool direct = false;
    };
    std::optional<Link> link_;
    bool wantList_ = false;
    std::optional<Pending> pending_;
    // The newest identity given to a request; it only grows.
    std::uint64_t lastRequest_ = 0;
    Filter filter_ = Filter::All;
    // Rooms that refused this PC while it was on the screen (full, locked, banned):
    // Quick join skips them until the player leaves.
    std::set<std::string> refused_;
    // The refresh count last seen, and the interface time of the list that advanced
    // it without error; the next refresh the panel asks for by itself is due at nextAuto_.
    std::uint64_t listedSeen_ = 0;
    double listedAt_ = 0, nextAuto_ = 0;
    // The service was usable on the last frame the screen was open: the list is asked for as soon as it becomes so.
    bool hadService_ = false;
    // The player asked for the refresh in flight (not the panel by itself).
    bool manualRefresh_ = false;
    std::optional<Current> current_;
};
} }
