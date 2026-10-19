#pragma once
// Where a player connects an account to their Ember ID through a browser
// sign-in on a tournament service: Discord, which tournament sites such as
// BluMint find players by, and the sign-in sites a service lists (Challonge,
// start.gg and later ones), whose brackets Ember runs. All work the same way:
// the helper opens the site's page, the player approves there, and the
// screens read the account on the service until it appears. This table holds
// what differs between Discord and a listed site; IdentitySignIn.cxx holds
// what they share. A listed site's words take its name as {0}, so a new site
// needs no new words.
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/IdentityView.hxx"
#include <cstdint>
#include <iterator>
#include <string>

namespace sf4e { namespace ui {
using netplay::SignInSource;
using netplay::SignInStep;
constexpr SignInSource SignInSources[] = {SignInSource::Discord, SignInSource::Site};
static_assert(std::size(SignInSources) == netplay::SignInSourceCount, "every source is listed");

// Discord, or one of a service's sign-in sites by its ID.
struct SignInTarget {
    SignInSource source = SignInSource::Discord;
    std::string site;
    static SignInTarget Discord() { return {}; }
    static SignInTarget Listed(std::string site) { return {SignInSource::Site, std::move(site)}; }
    bool operator==(const SignInTarget& o) const { return source == o.source && site == o.site; }
    bool operator!=(const SignInTarget& o) const { return !(*this == o); }
};

constexpr const char* DiscordConnectScreen = "discord-connect";
constexpr const char* SiteConnectScreen = "sign-in-site";
// The Ember ID screen's row for a listed site is this prefix and the site's ID.
constexpr const char* SiteRowPrefix = "site:";

struct SignInSiteInfo {
    // Its Connect screen, and another screen that shows (and so waits for)
    // its account, or null.
    const char* screen = nullptr;
    const char* alsoOn = nullptr;
    // It connects on Ember's own service, not the one the screens selected.
    bool embersService = false;
    // A failed read of its account is said only on its Connect screen.
    bool quietElsewhere = false;
    // Its sign-in belongs to the Connect Discord journey: a new journey no
    // longer waits for it. Otherwise it goes on until its own screen or
    // another sign-in ends it.
    bool followsJourney = false;
    // Its words take the site's name as {0}.
    bool named = false;
    // The view's fields for the account last read.
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
        const char* rowDetail = nullptr;        // the Ember ID screen's row
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
        const char* done = nullptr;             // the account's name: {0} for Discord, {1} for a listed site
        const char* removed = nullptr;
        const char* cancelled = nullptr;
        const char* busy = nullptr;             // over the site's request limits; null: the general words
        const char* gone = nullptr;             // the service no longer has the site; null: the general words
        const char* refusedOff = nullptr;       // its sign-in was switched off since the list was read; null: the general words
    } words;
};

inline const SignInSiteInfo& Site(SignInSource source) {
    using netplay::IdentityView;
    static const SignInSiteInfo discord = [] {
        SignInSiteInfo d;
        d.screen = DiscordConnectScreen; d.alsoOn = "linked-accounts"; d.followsJourney = true;
        d.user = &IdentityView::discordUser; d.name = &IdentityView::discordName;
        auto& r = d.rows;
        r.service = "dc-service"; r.waiting = "dc-waiting"; r.progress = "dc-progress"; r.connected = "dc-connected";
        r.connect = "id-discord-connect"; r.remove = "id-discord-remove"; r.off = "dc-off"; r.retry = "dc-retry"; r.cancel = "dc-cancel";
        auto& w = d.words;
        w.title = "screen.connect_discord"; w.rowDetail = "identity.connect_discord_detail";
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
    static const SignInSiteInfo listed = [] {
        SignInSiteInfo s;
        s.screen = SiteConnectScreen; s.embersService = s.quietElsewhere = s.named = true;
        s.user = &IdentityView::siteUser; s.name = &IdentityView::siteName;
        auto& r = s.rows;
        r.service = "st-service"; r.waiting = "st-waiting"; r.progress = "st-progress"; r.connected = "st-connected";
        r.connect = "st-connect"; r.remove = "st-remove"; r.off = "st-off"; r.retry = "st-retry"; r.cancel = "st-cancel";
        auto& w = s.words;
        w.title = "site.title"; w.rowDetail = "site.row_detail";
        w.none = "site.none"; w.connectDetail = "site.go_detail";
        w.settingUp = "site.setting_up"; w.settingUpDetail = "site.setting_up_detail";
        w.waiting = "site.waiting"; w.waitingLabel = "site.waiting_label"; w.waitingDetail = "site.waiting_detail";
        w.connected = "site.connected"; w.connectedDetail = "site.connected_detail";
        w.change = "site.change"; w.changeDetail = "site.change_detail";
        w.removeLabel = "identity.unlink"; w.removeDetail = "site.remove_detail";
        w.cancelDetail = "site.cancel_detail"; w.off = "site.off"; w.offDetail = "site.off_detail";
        w.retry = "site.retry"; w.retryDetail = "site.retry_detail";
        w.opened = "site.opened"; w.done = "site.done";
        w.removed = "site.removed"; w.cancelled = "site.cancelled"; w.busy = "site.busy";
        w.gone = "site.gone"; w.refusedOff = "site.off_detail";
        return s;
    }();
    switch (source) {
    case SignInSource::Site: return listed;
    case SignInSource::Discord: break;
    }
    return discord;
}
} }
