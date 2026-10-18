#pragma once
// The sites a player connects to their Ember ID through a browser sign-in on
// a tournament service: Discord, which tournament sites such as BluMint find
// players by, and Challonge, whose brackets Ember runs. Both work the same
// way: the helper opens the site's page, the player approves there, and the
// screens read the account on the service until it appears. This table holds
// what differs between them; IdentitySignIn.cxx holds what they share.
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/IdentityView.hxx"
#include <cstdint>
#include <optional>

namespace sf4e { namespace ui {
enum class SignInSite : std::uint8_t { Discord, Challonge };
constexpr SignInSite SignInSites[] = {SignInSite::Discord, SignInSite::Challonge};

struct SignInSiteInfo {
    // The helper requests: read the account, start a sign-in, disconnect, end a sign-in.
    netplay::IdentityOp status, connect, remove, cancel;
    // The site's Connect screen, and another screen that shows (and so
    // waits for) its account, or null.
    const char* screen;
    const char* alsoOn;
    // The rows that connect and disconnect the account, wherever they show,
    // and the prefix of the Connect screen's other rows.
    const char* connectRow;
    const char* removeRow;
    const char* prefix;
    // The Ember ID screen lists the site only where the service keeps its
    // accounts; Discord shows always, since Connect Discord sets everything up.
    bool listedWhenKept;
    // The view's fields for the site: whether the service last inspected
    // offers its sign-in and keeps its accounts, and the account last read.
    bool netplay::IdentityView::* offered;
    bool netplay::IdentityView::* kept;
    std::string netplay::IdentityView::* user;
    std::string netplay::IdentityView::* name;
    // Catalog ids of the words that name the site.
    const char* title;          // the Connect screen, and its Connect row
    const char* label;          // the Ember ID screen's row
    const char* rowDetail;
    const char* connectDetail;
    const char* settingUpDetail;
    const char* waiting;        // the status line while the account is read again
    const char* waitingLabel;
    const char* waitingDetail;
    const char* connected;
    const char* connectedDetail;
    const char* change;
    const char* changeDetail;
    const char* removeLabel;
    const char* removeDetail;
    const char* cancelDetail;
    const char* off;
    const char* offDetail;
    const char* opened;
    const char* done;           // {0}: the account's name
    const char* removed;
    const char* cancelled;
};

inline const SignInSiteInfo& Site(SignInSite site) {
    using Op = netplay::IdentityOp;
    static const SignInSiteInfo sites[] = {
        {Op::DiscordStatus, Op::DiscordConnect, Op::DiscordRemove, Op::DiscordCancel,
         "discord-connect", "linked-accounts", "id-discord-connect", "id-discord-remove", "dc-", false,
         &netplay::IdentityView::inspectedDiscord, &netplay::IdentityView::inspectedDiscordAccounts,
         &netplay::IdentityView::discordUser, &netplay::IdentityView::discordName,
         "screen.connect_discord", "identity.discord", "identity.connect_discord_detail", "connect.go_detail",
         "connect.setting_up_detail", "connect.waiting", "connect.waiting_label", "connect.waiting_detail",
         "connect.connected", "connect.connected_detail", "connect.change", "connect.change_detail",
         "identity.unlink", "identity.discord_remove_detail", "connect.cancel_detail", "connect.off", "connect.off_detail",
         "identity.done.discord_opened", "connect.done", "identity.done.discord_removed", "connect.cancelled"},
        {Op::ChallongeStatus, Op::ChallongeConnect, Op::ChallongeRemove, Op::ChallongeCancel,
         "challonge-connect", nullptr, "ch-connect", "ch-remove", "ch-", true,
         &netplay::IdentityView::inspectedChallonge, &netplay::IdentityView::inspectedChallongeAccounts,
         &netplay::IdentityView::challongeUser, &netplay::IdentityView::challongeName,
         "screen.connect_challonge", "identity.challonge", "identity.connect_challonge_detail", "challonge.go_detail",
         "challonge.setting_up_detail", "challonge.waiting", "challonge.waiting_label", "challonge.waiting_detail",
         "challonge.connected", "challonge.connected_detail", "challonge.change", "challonge.change_detail",
         "challonge.disconnect", "challonge.remove_detail", "challonge.cancel_detail", "challonge.off", "challonge.off_detail",
         "challonge.opened", "challonge.done", "challonge.removed", "challonge.cancelled"},
    };
    return sites[static_cast<std::size_t>(site)];
}

// The site a sign-in request is for; none for any other request.
inline std::optional<SignInSite> SiteOf(netplay::IdentityOp op) {
    for (const auto site : SignInSites) {
        const auto& info = Site(site);
        if (op == info.status || op == info.connect || op == info.remove || op == info.cancel) return site;
    }
    return std::nullopt;
}
} }
