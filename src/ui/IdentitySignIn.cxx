// The browser sign-in Discord and Challonge share (SignInSites.hxx): the
// helper opens the site's page, the player approves there, and these screens
// read the account on the service every few seconds until a different one
// appears. One sign-in runs at a time: starting one site's ends the other's
// on its service. Leaving the site's Connect screen while its page is being
// opened stops it; a wait for the browser goes on, and Cancel ends the
// sign-in on the service.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cassert>

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;
namespace {
// While a site's page is open in the browser, the account is read this
// often, for as long as the bridge keeps the sign-in.
constexpr double PollSeconds = 4, SignInSeconds = 600;

IdentityRequest Request(SignInSite site, SignInStep step, const std::string& bridge) {
    IdentityRequest r; r.op = netplay::SignInRequest(site, step); r.bridge = bridge; return r;
}
}

std::string IdentityPanel::SiteBridge(const ShellView& v, SignInSite site) const {
    if (!Site(site).embersService) return bridge_;
    const auto* ember = EmberBridge(v);
    return ember ? ember->id : std::string();
}

void IdentityPanel::ShowAccount(MenuEntry& row, SignInSite site, const std::string& bridge) const {
    const auto& accounts = Accounts(site);
    const auto account = accounts.find(bridge);
    if (account == accounts.end() || !account->second.read || account->second.failed) return;
    const bool named = !account->second.user.empty();
    row.value = named ? account->second.name : std::string(loc::T(Site(site).words.none));
    row.userText = named;
}

bool IdentityPanel::Starting() const {
    assert(steps_ == Steps::None || !signIn_);
    return steps_ == Steps::Running || (signIn_ && signIn_->phase == SignInAttempt::Phase::Opening);
}

bool IdentityPanel::ActiveFor(SignInSite site) const {
    return StepsRunFor(site) || (signIn_ && signIn_->site == site && signIn_->phase != SignInAttempt::Phase::Stopped);
}

void IdentityPanel::EndOtherSignIn(SignInSite site) {
    if (signIn_ && signIn_->site != site && signIn_->phase != SignInAttempt::Phase::Stopped) CancelAttempt();
}

void IdentityPanel::RetireSignIn() {
    if (!signIn_) return;
    // The journey's own sign-in is no longer waited for, as it always was.
    if (Site(signIn_->site).followsJourney) { signIn_.reset(); return; }
    // Another site's waiting for the browser goes on: Discord's steps or page
    // replace it only once they start (EndOtherSignIn). One whose page is
    // still being opened ends on its service, since the new journey retires
    // the answer it waits for. A page the helper is already opening still
    // opens, harmlessly: the cancel that follows means it connects nothing.
    if (signIn_->phase == SignInAttempt::Phase::Opening) CancelAttempt();
}

std::optional<std::string> IdentityPanel::SiteFailureText(IdentityOp op, const std::string& code) {
    const auto signIn = netplay::SignInOf(op);
    if (!signIn || code != "sign_in_busy") return std::nullopt;
    const char* busy = Site(signIn->site).words.busy;
    return busy ? std::optional<std::string>(loc::T(busy)) : std::nullopt;
}

bool IdentityPanel::WaitingOn(SignInSite site, const std::string& bridge) const {
    return signIn_ && signIn_->site == site && signIn_->bridge == bridge && signIn_->until > 0 && !signIn_->paused;
}

void IdentityPanel::OpenSignIn(SignInSite site, const std::string& bridge) {
    EndOtherSignIn(site);
    const auto& accounts = Accounts(site);
    const auto account = accounts.find(bridge);
    steps_ = Steps::None;
    SignInAttempt next{site, bridge, account == accounts.end() ? std::string() : account->second.user};
    // The same site again on the same service (Discord's change of account):
    // the wait already running goes on as it always has, still shown and
    // still read every few seconds, until the new page's answer starts it again.
    if (signIn_ && signIn_->site == site && signIn_->bridge == bridge) {
        next.until = signIn_->until; next.pollAt = signIn_->pollAt; next.paused = signIn_->paused;
    }
    signIn_ = std::move(next);
    Queue(Request(site, SignInStep::Connect, bridge));
}

void IdentityPanel::CancelAttempt() {
    // A sign-in already asked of the service ends there too, so a page left
    // open in the browser connects nothing.
    const auto connect = [](const Queued& q) { return netplay::EffectOf(q.request.op) == netplay::IdentityEffect::Open; };
    const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Queued& q) { return q.journey == journey_ && connect(q); });
    // The public rooms setup's own requests are not the attempt's to stop.
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Queued& q) {
        return q.owner == Owner::Screens && q.journey == journey_ &&
            (q.request.op == IdentityOp::Enable || q.request.op == IdentityOp::BridgeApprove || connect(q));
    }), queue_.end());
    if (!signIn_) { steps_ = Steps::Stopped; return; }
    using Phase = SignInAttempt::Phase;
    const bool asked = signIn_->phase == Phase::Waiting || (signIn_->phase == Phase::Opening && !queued);
    if (asked) cleanup_.push_back(Request(signIn_->site, SignInStep::Cancel, signIn_->bridge));
    signIn_ = SignInAttempt{signIn_->site, signIn_->bridge, signIn_->before, Phase::Stopped};
}

void IdentityPanel::ReadSiteAccounts(const netplay::IdentityView& id, const std::string& bridge, SignInSiteSet sites) {
    for (const auto site : SignInSites) {
        if (!(sites & SiteBit(site))) continue;
        const auto& info = Site(site);
        auto& account = Accounts(site)[bridge];
        account.kept = id.*info.offered || id.*info.kept;
        if (account.kept) Queue(Request(site, SignInStep::Status, bridge));
        // Nothing to read: the profile was the whole read, and it worked.
        else account.failed = false;
    }
}

bool IdentityPanel::QuietRead() const {
    const auto signIn = netplay::SignInOf(sentOp_);
    if (!signIn || signIn->step != SignInStep::Status) return false;
    const auto& info = Site(signIn->site);
    return info.quietElsewhere && lastScreen_ != info.screen;
}

void IdentityPanel::SiteReadFailed() {
    const auto signIn = netplay::SignInOf(sentOp_);
    if (!signIn) return;
    Accounts(signIn->site)[sentBridge_].failed = true;
    if (signIn_ && signIn_->site == signIn->site && signIn_->bridge == sentBridge_) signIn_->paused = true;
}

std::optional<MenuEntry> IdentityPanel::SiteRow(const ShellView& v, SignInSite site) const {
    const auto& info = Site(site);
    const auto* ember = EmberBridge(v);
    const auto& accounts = Accounts(site);
    const auto account = ember ? accounts.find(ember->id) : accounts.end();
    if (info.listedWhenKept && (account == accounts.end() || !account->second.kept)) return std::nullopt;
    auto row = Row(info.screen, loc::T(info.words.label), loc::T(info.words.rowDetail));
    if (ember) ShowAccount(row, site, ember->id);
    else row.value = loc::T(info.words.none);
    return row;
}

bool IdentityPanel::ShowsWait(const std::string& screen) const {
    if (!signIn_) return false;
    const auto& info = Site(signIn_->site);
    return screen == info.screen || (info.alsoOn && screen == info.alsoOn);
}

void IdentityPanel::SignInRows(const ShellView& v, SignInSite site, const netplay::IdentityBridge& target, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& info = Site(site);
    const auto& ids = info.rows;
    const auto& words = info.words;
    auto service = InfoRow(ids.service, loc::T("identity.service"), target.name.empty() ? target.origin : target.name, ServiceDetail(target));
    service.userText = true; rows.push_back(std::move(service));
    // Read, and the latest read did not fail.
    const auto& accounts = Accounts(site);
    const auto account = accounts.find(target.id);
    const bool failed = account != accounts.end() && account->second.failed;
    const bool read = SiteBridge(v, site) == target.id && account != accounts.end() && account->second.read && !failed;
    const bool starting = StepsRunFor(site) || (signIn_ && signIn_->site == site && signIn_->phase == SignInAttempt::Phase::Opening);
    if (WaitingOn(site, target.id)) {
        rows.push_back(InfoRow(ids.waiting, loc::T(words.waitingLabel), {}, loc::T(words.waitingDetail)));
    } else if (starting && busy) {
        rows.push_back(InfoRow(ids.progress, loc::T(words.settingUp), {}, loc::T(words.settingUpDetail)));
    } else if (read && !account->second.user.empty()) {
        auto connected = InfoRow(ids.connected, loc::T(words.connected), account->second.name, loc::T(words.connectedDetail));
        connected.userText = true; rows.push_back(std::move(connected));
        // Another account, or none: both here.
        rows.push_back(Row(ids.connect, loc::T(words.change), loc::T(words.changeDetail), !busy));
        auto remove = ConfirmRow(ids.remove, loc::T(words.removeLabel), loc::T(words.removeDetail), !busy);
        remove.hint = loc::T(words.removeLabel); rows.push_back(std::move(remove));
    } else if (v.identity.inspected.id == target.id && !(v.identity.*info.offered) && !failed) {
        // Off as the latest read said; a failed read offers Try again below.
        rows.push_back(InfoRow(ids.off, loc::T(words.off), {}, loc::T(words.offDetail)));
    } else {
        rows.push_back(Row(ids.connect, loc::T(words.title), loc::T(words.connectDetail), !busy && read));
        // Nothing in flight and the account not read: reading it failed.
        if (!busy && !read) rows.push_back(Row(ids.retry, loc::T(words.retry), loc::T(words.retryDetail)));
    }
}

void IdentityPanel::ChallongeRows(const ShellView& v, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& info = Site(SignInSite::Challonge);
    if (v.identity.state != "ready") {
        rows.push_back(InfoRow("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
        return;
    }
    if (const auto* ember = EmberBridge(v)) SignInRows(v, SignInSite::Challonge, *ember, rows, busy);
    else {
        // Challonge connects through Ember's own service, which the player has not trusted.
        rows.push_back(InfoRow("ch-no-service", loc::T("challonge.no_service"), {}, loc::T("challonge.no_service_detail")));
        rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
    }
    if (ActiveFor(SignInSite::Challonge)) rows.push_back(Row(info.rows.cancel, loc::T("common.cancel"), loc::T(info.words.cancelDetail)));
}

bool IdentityPanel::SignInActivate(const MenuAction& a, const ShellView& v) {
    for (const auto site : SignInSites) {
        const auto& info = Site(site);
        if (a.id == info.rows.connect) {
            message_.clear();
            const auto bridge = SiteBridge(v, site);
            if (!bridge.empty()) OpenSignIn(site, bridge);
        } else if (a.id == info.rows.remove) {
            message_.clear(); Queue(Request(site, SignInStep::Remove, SiteBridge(v, site)));
        } else if (a.id == info.rows.cancel) {
            CancelAttempt(); Say(loc::T(info.words.cancelled), false);
        } else if (a.id == info.rows.retry) {
            // Reading again also resumes a paused wait's polls.
            message_.clear(); Refresh(v, info.screen);
        } else continue;
        return true;
    }
    return false;
}

void IdentityPanel::SignInFinish(const ShellView& v) {
    const auto& id = v.identity;
    const auto signIn = netplay::SignInOf(sentOp_);
    if (!signIn) return;
    const auto& info = Site(signIn->site);
    const auto& user = id.*info.user;
    using Phase = SignInAttempt::Phase;
    const bool waitedFor = signIn_ && signIn_->site == signIn->site && signIn_->bridge == sentBridge_;
    switch (signIn->step) {
    case SignInStep::Status: {
        auto& account = Accounts(signIn->site)[sentBridge_];
        account.user = user; account.name = id.*info.name; account.read = true; account.failed = false;
        // Only the service and site the sign-in is for end the wait, and only
        // with an account other than the one connected when the page opened.
        if (waitedFor && signIn_->until > 0 && !user.empty() && user != signIn_->before) {
            signIn_.reset();
            Say(loc::Tf(info.words.done, id.*info.name), false, 20);
        }
        break;
    }
    case SignInStep::Connect:
        // A cancelled attempt does not wait, though its page may have opened.
        if (!waitedFor || signIn_->phase != Phase::Opening) break;
        Say(loc::T(info.words.opened), false, 30);
        signIn_->phase = Phase::Waiting;
        signIn_->until = now_ + SignInSeconds; signIn_->pollAt = now_ + PollSeconds; signIn_->paused = false;
        break;
    case SignInStep::Remove: {
        auto& account = Accounts(signIn->site)[sentBridge_];
        account.user.clear(); account.name.clear(); account.read = true; account.failed = false;
        if (waitedFor) signIn_.reset();
        Say(loc::T(info.words.removed), false);
        break;
    }
    case SignInStep::Cancel: break;
    }
}

void IdentityPanel::SignInPoll(const std::string& screen, double now) {
    using Phase = SignInAttempt::Phase;
    if (!signIn_ || signIn_->until <= 0 || signIn_->paused || sent_ || !queue_.empty() || now < signIn_->pollAt || !ShowsWait(screen)) return;
    if (now > signIn_->until) { signIn_->phase = Phase::Stopped; signIn_->until = 0; return; }
    signIn_->pollAt = now + PollSeconds;
    Queue(Request(signIn_->site, SignInStep::Status, signIn_->bridge));
}
} }
