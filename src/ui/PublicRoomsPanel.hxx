#pragma once
#include "MenuNavigation.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/TournamentStatus.hxx"
#include <functional>
#include <optional>
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
class PublicRoomsPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static bool Owns(const std::string& screen) { return screen == "public-rooms"; }
    // A refused request or a failed refresh, by its stable code, in words.
    static std::string FailureText(const std::string& code);
    // Every frame, before the rows: asks for the list on entering the screen,
    // sends what is waiting, and joins with an admission that arrives.
    // `bridge` is the usable service, empty without one; `identityPending`
    // says the Ember ID is still being asked about.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge, bool identityPending,
                const Submit& submit, double now);
    // A room link opened from the browser, or pasted. `direct`: the player was
    // free when it arrived, so it asks for the room by itself once on this
    // screen; otherwise it waits as a row until the player chooses it.
    void OpenLink(const std::string& bridge, const std::string& room, bool direct);
    // `bridge` and `identityPending` as for Update.
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& bridge, bool identityPending) const;
    // True when the row was the panel's own.
    bool Activate(const MenuAction& action, MenuNavigation& navigation);
    // Opens a public room: the new room's admission is joined when it arrives.
    void Create(const std::string& name, int capacity);
    // A create or ticket request is waiting to be sent or answered.
    bool Busy() const { return pending_.has_value(); }
    // What a pending request or refresh says on `screen`.
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const;
    // What the panel has to say, once, wherever the player is.
    bool TakeSaid(std::string& text);
    // The shell is hidden: an admission that arrives meanwhile must not join
    // later, so the request in flight is abandoned. A room link nobody has
    // followed yet is kept: it is the player's, until followed or replaced.
    void Conceal();
    // A link that goes on by itself is waiting to be asked for.
    bool FollowingLink() const { return link_ && link_->direct; }
    // That link waits for the player's choice instead: they are busy now.
    void HoldLink() { if (link_) link_->direct = false; }
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
    };
    void Begin(netplay::tournament::Command command);
    void AskForLink(const ShellView& view, bool identityPending);
    void Joined(const ShellView& view, const Submit& submit);
    void Say(std::string text) { said_ = std::move(text); }

    std::string bridge_, lastScreen_, said_, listError_;
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
};
} }
