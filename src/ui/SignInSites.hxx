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
#include <iterator>

namespace sf4e { namespace ui {
using netplay::SignInSite;
using netplay::SignInStep;
constexpr SignInSite SignInSites[] = {SignInSite::Discord, SignInSite::Challonge};
static_assert(std::size(SignInSites) == netplay::SignInSiteCount, "every site is listed");
static_assert(static_cast<std::size_t>(SignInSite::Discord) == 0 && static_cast<std::size_t>(SignInSite::Challonge) == 1,
    "sites index their tables in this order");

// A set of sites, for the accounts a service's profile leads to reading.
using SignInSiteSet = std::uint8_t;
constexpr SignInSiteSet SiteBit(SignInSite site) { return static_cast<SignInSiteSet>(1u << static_cast<unsigned>(site)); }
constexpr SignInSiteSet AllSignInSites = static_cast<SignInSiteSet>((1u << netplay::SignInSiteCount) - 1);

constexpr const char* DiscordConnectScreen = "discord-connect";
constexpr const char* ChallongeConnectScreen = "challonge-connect";

struct SignInSiteInfo {
    // The site's Connect screen, and another screen that shows (and so waits
    // for) its account, or null.
    const char* screen = nullptr;
    const char* alsoOn = nullptr;
    // The site connects on Ember's own service, not the one the screens selected.
    bool embersService = false;
    // The Ember ID screen lists the site only where the service keeps its
    // accounts; otherwise always, as Connect Discord sets everything up.
    bool listedWhenKept = false;
    // A failed read of its account is said only on its Connect screen.
    bool quietElsewhere = false;
    // Its sign-in belongs to the Connect Discord journey: a new journey no
    // longer waits for it. Otherwise it goes on until the site's own screen
    // or another site's sign-in ends it.
    bool followsJourney = false;
    // The view's fields for the site: whether the service last inspected
    // offers its sign-in and keeps its accounts, and the account last read.
    bool netplay::IdentityView::* offered = nullptr;
    bool netplay::IdentityView::* kept = nullptr;
    std::string netplay::IdentityView::* user = nullptr;
    std::string netplay::IdentityView::* name = nullptr;
    // Its rows' ids, wherever they show.
    struct Rows {
        const char* service = nullptr;
        const char* waiting = nullptr;
        const char* progress = nullptr;
        const char* connected = nullptr;
        const char* connect = nullptr;
        const char* remove = nullptr;
        const char* off = nullptr;
        const char* retry = nullptr;
        const char* cancel = nullptr;
    } rows;
    // Catalog ids of its words.
    struct Words {
        const char* title = nullptr;            // the Connect screen, and its Connect row
        const char* label = nullptr;            // the Ember ID screen's row
        const char* rowDetail = nullptr;
        const char* none = nullptr;             // no account connected
        const char* connectDetail = nullptr;
        const char* settingUp = nullptr;
        const char* settingUpDetail = nullptr;
        const char* waiting = nullptr;          // the status line while the account is read again
        const char* waitingLabel = nullptr;
        const char* waitingDetail = nullptr;
        const char* connected = nullptr;
        const char* connectedDetail = nullptr;
        const char* change = nullptr;
        const char* changeDetail = nullptr;
        const char* removeLabel = nullptr;
        const char* removeDetail = nullptr;
        const char* cancelDetail = nullptr;
        const char* off = nullptr;
        const char* offDetail = nullptr;
        const char* retry = nullptr;
        const char* retryDetail = nullptr;
        const char* opened = nullptr;
        const char* done = nullptr;             // {0}: the account's name
        const char* removed = nullptr;
        const char* cancelled = nullptr;
        const char* busy = nullptr;             // over the site's request limits; null: the general words
    } words;
};

inline const SignInSiteInfo& Site(SignInSite site) {
    using netplay::IdentityView;
    static const SignInSiteInfo discord = [] {
        SignInSiteInfo d;
        d.screen = DiscordConnectScreen; d.alsoOn = "linked-accounts"; d.followsJourney = true;
        d.offered = &IdentityView::inspectedDiscord; d.kept = &IdentityView::inspectedDiscordAccounts;
        d.user = &IdentityView::discordUser; d.name = &IdentityView::discordName;
        auto& r = d.rows;
        r.service = "dc-service"; r.waiting = "dc-waiting"; r.progress = "dc-progress"; r.connected = "dc-connected";
        r.connect = "id-discord-connect"; r.remove = "id-discord-remove"; r.off = "dc-off"; r.retry = "dc-retry"; r.cancel = "dc-cancel";
        auto& w = d.words;
        w.title = "screen.connect_discord"; w.label = "identity.discord"; w.rowDetail = "identity.connect_discord_detail";
        w.none = "identity.discord_none"; w.connectDetail = "connect.go_detail";
        w.settingUp = "connect.setting_up"; w.settingUpDetail = "connect.setting_up_detail";
        w.waiting = "connect.waiting"; w.waitingLabel = "connect.waiting_label"; w.waitingDetail = "connect.waiting_detail";
        w.connected = "connect.connected"; w.connectedDetail = "connect.connected_detail";
        w.change = "connect.change"; w.changeDetail = "connect.change_detail";
        w.removeLabel = "identity.unlink"; w.removeDetail = "identity.discord_remove_detail";
        w.cancelDetail = "connect.cancel_detail"; w.off = "connect.off"; w.offDetail = "connect.off_detail";
        w.retry = "connect.retry"; w.retryDetail = "connect.retry_detail";
        w.opened = "identity.done.discord_opened"; w.done = "connect.done";
        w.removed = "identity.done.discord_removed"; w.cancelled = "connect.cancelled";
        return d;
    }();
    static const SignInSiteInfo challonge = [] {
        SignInSiteInfo c;
        c.screen = ChallongeConnectScreen; c.embersService = c.listedWhenKept = c.quietElsewhere = true;
        c.offered = &IdentityView::inspectedChallonge; c.kept = &IdentityView::inspectedChallongeAccounts;
        c.user = &IdentityView::challongeUser; c.name = &IdentityView::challongeName;
        auto& r = c.rows;
        r.service = "ch-service"; r.waiting = "ch-waiting"; r.progress = "ch-progress"; r.connected = "ch-connected";
        r.connect = "ch-connect"; r.remove = "ch-remove"; r.off = "ch-off"; r.retry = "ch-retry"; r.cancel = "ch-cancel";
        auto& w = c.words;
        w.title = "screen.connect_challonge"; w.label = "identity.challonge"; w.rowDetail = "identity.connect_challonge_detail";
        w.none = "challonge.none"; w.connectDetail = "challonge.go_detail";
        w.settingUp = "challonge.setting_up"; w.settingUpDetail = "challonge.setting_up_detail";
        w.waiting = "challonge.waiting"; w.waitingLabel = "challonge.waiting_label"; w.waitingDetail = "challonge.waiting_detail";
        w.connected = "challonge.connected"; w.connectedDetail = "challonge.connected_detail";
        w.change = "challonge.change"; w.changeDetail = "challonge.change_detail";
        w.removeLabel = "identity.unlink"; w.removeDetail = "challonge.remove_detail";
        w.cancelDetail = "challonge.cancel_detail"; w.off = "challonge.off"; w.offDetail = "challonge.off_detail";
        w.retry = "challonge.retry"; w.retryDetail = "challonge.retry_detail";
        w.opened = "challonge.opened"; w.done = "challonge.done";
        w.removed = "challonge.removed"; w.cancelled = "challonge.cancelled"; w.busy = "challonge.busy";
        return c;
    }();
    switch (site) {
    case SignInSite::Challonge: return challonge;
    case SignInSite::Discord: break;
    }
    return discord;
}
} }
