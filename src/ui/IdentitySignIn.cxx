// The browser sign-in Discord and Challonge share (SignInSites.hxx): the
// helper opens the site's page, the player approves there, and these screens
// read the account on the service every few seconds until a different one
// appears. One attempt runs at a time, for one site. Leaving the site's
// Connect screen while its page is being opened stops it; a wait for the
// browser goes on, and Cancel ends the sign-in on the service.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;
namespace {
// While a site's page is open in the browser, the account is read this
// often, for as long as the bridge keeps the sign-in.
constexpr double PollSeconds = 4, SignInSeconds = 600;
}

std::string IdentityPanel::SiteBridge(const ShellView& v, SignInSite site) const {
    if (site == SignInSite::Discord) return bridge_;
    const auto* ember = EmberBridge(v);
    return ember ? ember->id : std::string();
}

void IdentityPanel::ShowAccount(MenuEntry& row, SignInSite site, const std::string& bridge) const {
    const auto& accounts = Accounts(site);
    const auto account = accounts.find(bridge);
    if (account == accounts.end() || !account->second.read || account->second.failed) return;
    const bool named = !account->second.user.empty();
    row.value = named ? account->second.name : std::string(loc::T("identity.discord_none"));
    row.userText = named;
}

void IdentityPanel::OpenSignIn(SignInSite site, const std::string& bridge) {
    const auto& accounts = Accounts(site);
    const auto account = accounts.find(bridge);
    attemptBefore_ = account == accounts.end() ? std::string() : account->second.user;
    attempt_ = Attempt::Opening; site_ = site; waitBridge_ = bridge;
    IdentityRequest r; r.op = Site(site).connect; r.bridge = bridge; Queue(std::move(r));
}

void IdentityPanel::CancelAttempt() {
    // A sign-in already asked of the service ends there too, so a page left
    // open in the browser connects nothing.
    const auto connect = [](const Queued& q) { const auto site = SiteOf(q.request.op); return site && q.request.op == Site(*site).connect; };
    const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Queued& q) { return q.journey == journey_ && connect(q); });
    const bool asked = attempt_ == Attempt::Waiting || (attempt_ == Attempt::Opening && !queued);
    // The public rooms setup's own requests are not the attempt's to stop.
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Queued& q) {
        return q.owner == Owner::Screens && q.journey == journey_ &&
            (q.request.op == IdentityOp::Enable || q.request.op == IdentityOp::BridgeApprove || connect(q));
    }), queue_.end());
    if (asked && !waitBridge_.empty()) {
        IdentityRequest cancel; cancel.op = Site(site_).cancel; cancel.bridge = waitBridge_; cleanup_.push_back(std::move(cancel));
    }
    attempt_ = Attempt::Stopped; waitUntil_ = 0; paused_ = false;
}

void IdentityPanel::ReadSiteAccounts(const netplay::IdentityView& id, const std::string& bridge) {
    for (const auto site : SignInSites) {
        const auto& info = Site(site);
        auto& account = Accounts(site)[bridge];
        account.kept = id.*info.offered || id.*info.kept;
        if (account.kept) { IdentityRequest read; read.op = info.status; read.bridge = bridge; Queue(std::move(read)); }
        // Nothing to read: the profile was the whole read, and it worked.
        else account.failed = false;
    }
}

std::optional<MenuEntry> IdentityPanel::SiteRow(const ShellView& v, SignInSite site) const {
    const auto& info = Site(site);
    const auto* ember = EmberBridge(v);
    const auto& accounts = Accounts(site);
    const auto account = ember ? accounts.find(ember->id) : accounts.end();
    if (info.listedWhenKept && (account == accounts.end() || !account->second.kept)) return std::nullopt;
    auto row = Row(info.screen, loc::T(info.label), loc::T(info.rowDetail));
    if (ember) ShowAccount(row, site, ember->id);
    else row.value = loc::T("identity.discord_none");
    return row;
}

bool IdentityPanel::ShowsWait(const std::string& screen) const {
    const auto& info = Site(site_);
    return screen == info.screen || (info.alsoOn && screen == info.alsoOn);
}

void IdentityPanel::SignInRows(const ShellView& v, SignInSite site, const netplay::IdentityBridge& target, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& info = Site(site);
    const std::string prefix = info.prefix;
    auto service = InfoRow(prefix + "service", loc::T("identity.service"), target.name.empty() ? target.origin : target.name, ServiceDetail(target));
    service.userText = true; rows.push_back(std::move(service));
    // Read, and the latest read did not fail.
    const auto& accounts = Accounts(site);
    const auto account = accounts.find(target.id);
    const bool failed = account != accounts.end() && account->second.failed;
    const bool read = SiteBridge(v, site) == target.id && account != accounts.end() && account->second.read && !failed;
    const bool mine = site == site_;
    if (mine && waitUntil_ > 0 && !paused_ && waitBridge_ == target.id) {
        rows.push_back(InfoRow(prefix + "waiting", loc::T(info.waitingLabel), {}, loc::T(info.waitingDetail)));
    } else if (mine && (attempt_ == Attempt::Setup || attempt_ == Attempt::Opening) && busy) {
        rows.push_back(InfoRow(prefix + "progress", loc::T("connect.setting_up"), {}, loc::T(info.settingUpDetail)));
    } else if (read && !account->second.user.empty()) {
        auto connected = InfoRow(prefix + "connected", loc::T(info.connected), account->second.name, loc::T(info.connectedDetail));
        connected.userText = true; rows.push_back(std::move(connected));
        // Another account, or none: both here.
        rows.push_back(Row(info.connectRow, loc::T(info.change), loc::T(info.changeDetail), !busy));
        auto remove = ConfirmRow(info.removeRow, loc::T(info.removeLabel), loc::T(info.removeDetail), !busy);
        remove.hint = loc::T(info.removeLabel); rows.push_back(std::move(remove));
    } else if (v.identity.inspected.id == target.id && !(v.identity.*info.offered) && !failed) {
        // Off as the latest read said; a failed read offers Try again below.
        rows.push_back(InfoRow(prefix + "off", loc::T(info.off), {}, loc::T(info.offDetail)));
    } else {
        rows.push_back(Row(info.connectRow, loc::T(info.title), loc::T(info.connectDetail), !busy && read));
        // Nothing in flight and the account not read: reading it failed.
        if (!busy && !read) rows.push_back(Row(prefix + "retry", loc::T("connect.retry"), loc::T("connect.retry_detail")));
    }
}

void IdentityPanel::ChallongeRows(const ShellView& v, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& info = Site(SignInSite::Challonge);
    if (v.identity.state != "ready") {
        rows.push_back(InfoRow("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
        return;
    }
    // Challonge runs on Ember's own service; without it there is nothing to connect.
    if (const auto* ember = EmberBridge(v)) SignInRows(v, SignInSite::Challonge, *ember, rows, busy);
    else rows.push_back(InfoRow("ch-off", loc::T(info.off), {}, loc::T(info.offDetail)));
    if (Active() && site_ == SignInSite::Challonge) rows.push_back(Row("ch-cancel", loc::T("common.cancel"), loc::T(info.cancelDetail)));
}

bool IdentityPanel::SignInActivate(const MenuAction& a, const ShellView& v) {
    for (const auto site : SignInSites) {
        const auto& info = Site(site);
        const std::string prefix = info.prefix;
        if (a.id == info.connectRow) {
            message_.clear();
            const auto bridge = SiteBridge(v, site);
            if (!bridge.empty()) OpenSignIn(site, bridge);
        } else if (a.id == info.removeRow) {
            IdentityRequest r; r.op = info.remove; r.bridge = SiteBridge(v, site);
            message_.clear(); Queue(std::move(r));
        } else if (a.id == prefix + "cancel") {
            CancelAttempt(); Say(loc::T(info.cancelled), false);
        } else if (a.id == prefix + "retry") {
            // Try again restarts a stopped Connect Discord; a paused wait resumes its polls.
            if (site == SignInSite::Discord && attempt_ == Attempt::Stopped) { attempt_ = Attempt::Setup; site_ = site; }
            message_.clear(); Refresh(v, info.screen);
        } else continue;
        return true;
    }
    return false;
}

void IdentityPanel::SignInFinish(const ShellView& v, SignInSite site) {
    const auto& id = v.identity;
    const auto& info = Site(site);
    const auto& user = id.*info.user;
    const bool waitedFor = site == site_ && sentBridge_ == waitBridge_;
    if (sentOp_ == info.status) {
        auto& account = Accounts(site)[sentBridge_];
        account.user = user; account.name = id.*info.name; account.read = true; account.failed = false;
        // Only the service and site the sign-in is for end the wait, and only
        // with an account other than the one connected when the page opened.
        if (waitUntil_ > 0 && waitedFor && !user.empty() && user != attemptBefore_) {
            waitUntil_ = 0; attempt_ = Attempt::None;
            Say(loc::Tf(info.done, id.*info.name), false, 20);
        }
        // A running Connect Discord opens Discord once the account is read:
        // none yet, on a service that offers sign-in. A connected account is shown.
        if (site == SignInSite::Discord && attempt_ == Attempt::Setup && lastScreen_ == info.screen) {
            const auto* target = ConnectTarget(v);
            if (target && target->id == sentBridge_) {
                if (user.empty() && id.inspected.id == target->id && id.*info.offered) OpenSignIn(site, sentBridge_);
                else attempt_ = Attempt::None;
            }
        }
    } else if (sentOp_ == info.connect) {
        // A cancelled attempt does not wait, though its page may have opened.
        if (attempt_ != Attempt::Opening || !waitedFor) return;
        Say(loc::T(info.opened), false, 30);
        attempt_ = Attempt::Waiting;
        waitUntil_ = now_ + SignInSeconds; pollAt_ = now_ + PollSeconds; paused_ = false;
    } else if (sentOp_ == info.remove) {
        auto& account = Accounts(site)[sentBridge_];
        account.user.clear(); account.name.clear(); account.read = true; account.failed = false;
        if (waitedFor) { waitUntil_ = 0; attempt_ = Attempt::None; }
        Say(loc::T(info.removed), false);
    }
}

void IdentityPanel::SignInPoll(const std::string& screen, double now) {
    if (waitUntil_ <= 0 || paused_ || sent_ || !queue_.empty() || now < pollAt_ || !ShowsWait(screen)) return;
    if (now > waitUntil_) { waitUntil_ = 0; attempt_ = Attempt::Stopped; return; }
    pollAt_ = now + PollSeconds;
    IdentityRequest poll; poll.op = Site(site_).status; poll.bridge = waitBridge_; Queue(std::move(poll));
}
} }
