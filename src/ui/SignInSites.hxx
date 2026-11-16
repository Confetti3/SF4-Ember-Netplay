#pragma once
// Where a player connects an account to their Ember ID through a browser
// sign-in on a tournament service: Discord, which tournament sites such as
// BluMint and Ember's bracket site find players by. The helper opens the
// site's page, the player approves there, and the screens read the account
// on the service until it appears. This table holds the sign-in's screens,
// rows and words; IdentitySignIn.cxx holds how it runs.
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/IdentityView.hxx"
#include <cstdint>
#include <iterator>
#include <string>

namespace sf4e { namespace ui {
using netplay::SignInSource;
using netplay::SignInStep;
constexpr SignInSource SignInSources[] = {SignInSource::Discord};
static_assert(std::size(SignInSources) == netplay::SignInSourceCount, "every source is listed");

// The sign-in a screen runs: Discord's.
struct SignInTarget {
    SignInSource source = SignInSource::Discord;
    static SignInTarget Discord() { return {}; }
    bool operator==(const SignInTarget& o) const { return source == o.source; }
    bool operator!=(const SignInTarget& o) const { return !(*this == o); }
};

constexpr const char* DiscordConnectScreen = "discord-connect";

struct SignInSiteInfo {
    // Its Connect screen, and another screen that shows (and so waits for)
    // its account, or null.
    const char* screen = nullptr;
    const char* alsoOn = nullptr;
    // The view's account last read.
    netplay::IdentityAccount (netplay::IdentityView::* account)() const = nullptr;
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
        const char* off = nullptr;
        const char* offDetail = nullptr;
        const char* retry = nullptr;
        const char* retryDetail = nullptr;
        const char* opened = nullptr;
        const char* done = nullptr;             // the account's name as {0}
        const char* removed = nullptr;
        const char* cancelled = nullptr;
    } words;
};

inline const SignInSiteInfo& Site(SignInSource source) {
    using netplay::IdentityView;
    static const SignInSiteInfo discord = [] {
        SignInSiteInfo d;
        d.screen = DiscordConnectScreen; d.alsoOn = "linked-accounts";
        d.account = &IdentityView::Discord;
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
        w.off = "connect.off"; w.offDetail = "connect.off_detail";
        w.retry = "connect.retry"; w.retryDetail = "connect.retry_detail";
        w.opened = "identity.done.discord_opened"; w.done = "connect.done";
        w.removed = "identity.done.discord_removed"; w.cancelled = "connect.cancelled";
        return d;
    }();
    switch (source) {
    case SignInSource::Discord: break;
    }
    return discord;
}
} }
