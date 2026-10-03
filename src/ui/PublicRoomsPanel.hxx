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
// selects, so it works from the service it is given.
class PublicRoomsPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static bool Owns(const std::string& screen) { return screen == "public-rooms"; }
    // A refused request or a failed refresh, by its stable code, in words.
    static std::string FailureText(const std::string& code);
    // Every frame, before the rows: asks for the list on entering the screen,
    // sends what is waiting, and joins with an admission that arrives.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge, const Submit& submit, double now);
    // `bridge` is the usable service, empty without one; `identityPending`
    // says the Ember ID is still being asked about.
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
    // The shell is hidden: an admission that arrives meanwhile must not join later.
    void Conceal();
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
    void Joined(const ShellView& view, const Submit& submit);
    void Say(std::string text) { said_ = std::move(text); }

    std::string bridge_, lastScreen_, said_, listError_;
    bool wantList_ = false;
    std::optional<Pending> pending_;
    // The newest identity given to a request; it only grows.
    std::uint64_t lastRequest_ = 0;
};
} }
