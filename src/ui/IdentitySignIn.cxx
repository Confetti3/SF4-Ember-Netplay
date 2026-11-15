// Discord's browser sign-in (SignInSites.hxx): the helper opens its page, the player approves
// there, and these screens read the account on the service every few seconds
// until a different one appears. One sign-in runs at a time: starting one
// ends any other on its service. Leaving the Connect screen while its page is
// being opened stops it; a wait for the browser goes on, and Cancel ends the
// sign-in on the service.
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

IdentityRequest Request(const SignInTarget& target, SignInStep step, const std::string& bridge) {
    IdentityRequest r; r.op = netplay::SignInRequest(target.source, step); r.bridge = bridge; return r;
}
}

IdentityPanel::SiteAccount& IdentityPanel::Account(const SignInTarget& target, const std::string& bridge) {
    return accounts_[{target.source, bridge}];
}

const IdentityPanel::SiteAccount* IdentityPanel::FindAccount(const SignInTarget& target, const std::string& bridge) const {
    const auto it = accounts_.find({target.source, bridge});
    return it == accounts_.end() ? nullptr : &it->second;
}

bool IdentityPanel::AtScreenOf(const SignInTarget& target, const std::string& screen) const {
    return screen == Site(target.source).screen;
}

void IdentityPanel::ShowAccount(MenuEntry& row, const SignInTarget& target, const std::string& bridge) const {
    const auto* account = FindAccount(target, bridge);
    if (!account || !account->read || account->failed) return;
    const bool named = !account->user.empty();
    row.value = named ? account->name : std::string(loc::T(Site(target.source).words.none));
    row.userText = named;
}

bool IdentityPanel::Starting() const {
    using Phase = SignInAttempt::Phase;
    return signIn_ && (signIn_->phase == Phase::Steps || signIn_->phase == Phase::Opening);
}

bool IdentityPanel::ActiveFor(const SignInTarget& target) const {
    return signIn_ && signIn_->target == target && signIn_->phase != SignInAttempt::Phase::Stopped;
}

void IdentityPanel::EndOtherSignIn(const SignInTarget& target) {
    using Phase = SignInAttempt::Phase;
    // Connect Discord's steps have nothing on a service yet: the new sign-in replaces them.
    if (signIn_ && signIn_->target != target && signIn_->phase != Phase::Stopped && signIn_->phase != Phase::Steps) CancelAttempt();
}

void IdentityPanel::RetireSignIn() {
    // The journey's own sign-in, its steps included, is no longer waited for.
    signIn_.reset();
}

bool IdentityPanel::WaitingOn(const SignInTarget& target, const std::string& bridge) const {
    return signIn_ && signIn_->target == target && signIn_->bridge == bridge && signIn_->until > 0 && !signIn_->paused;
}

void IdentityPanel::OpenSignIn(const SignInTarget& target, const std::string& bridge) {
    EndOtherSignIn(target);
    const auto* account = FindAccount(target, bridge);
    SignInAttempt next{target, bridge};
    if (account) next.before = *account;
    // The same site again on the same service (Discord's change of account):
    // the wait already running goes on as it always has, still shown and
    // still read every few seconds, until the new page's answer starts it again.
    if (signIn_ && signIn_->target == target && signIn_->bridge == bridge) {
        next.until = signIn_->until; next.pollAt = signIn_->pollAt; next.paused = signIn_->paused;
    }
    if (target.source == SignInSource::Discord) discordService_ = bridge;
    signIn_ = std::move(next);
    Queue(Request(target, SignInStep::Connect, bridge));
}

void IdentityPanel::CancelAttempt() {
    if (!signIn_) return;
    using Phase = SignInAttempt::Phase;
    // A sign-in already asked of the service ends there too, so a page left
    // open in the browser connects nothing.
    const auto connect = [](const Queued& q) { return netplay::EffectOf(q.request.op) == netplay::IdentityEffect::Open; };
    const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Queued& q) { return q.journey == journey_ && connect(q); });
    // Connect Discord's steps take the trust and the ID creation they queued
    // with them; a sign-in leaves those to finish. The public rooms setup's
    // own requests are not the attempt's to stop.
    const bool steps = signIn_->phase == Phase::Steps;
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Queued& q) {
        return q.owner == Owner::Screens && q.journey == journey_ &&
            (connect(q) || (steps && (q.request.op == IdentityOp::Enable || q.request.op == IdentityOp::BridgeApprove)));
    }), queue_.end());
    const bool asked = signIn_->phase == Phase::Waiting || (signIn_->phase == Phase::Opening && !queued);
    if (asked) cleanup_.push_back(Request(signIn_->target, SignInStep::Cancel, signIn_->bridge));
    signIn_ = SignInAttempt{signIn_->target, signIn_->bridge, signIn_->before, Phase::Stopped};
}

void IdentityPanel::ReadSiteAccounts(const netplay::IdentityView& id, const std::string& bridge, const AccountReads& reads) {
    if (reads.ReadsDiscord()) {
        // A service without Discord accounts: its profile was the whole read, and it worked.
        if (id.inspectedDiscord || id.inspectedDiscordAccounts) Queue(Request(SignInTarget::Discord(), SignInStep::Status, bridge));
        else Account(SignInTarget::Discord(), bridge).failed = false;
    }
    // The tournament desk, on the bracket site the same service names; another
    // service's profile leaves it as it was.
    if (id.inspectedDesk) deskBridge_ = bridge;
    else if (deskBridge_ == bridge) deskBridge_.clear();
}

void IdentityPanel::AccountReadsFailed(const std::string& bridge, const AccountReads& reads) {
    if (reads.ReadsDiscord()) Account(SignInTarget::Discord(), bridge).failed = true;
}

void IdentityPanel::SiteReadFailed() {
    if (!netplay::SignInOf(sentOp_)) return;
    const auto target = SignInTarget::Discord();
    Account(target, sentBridge_).failed = true;
    if (signIn_ && signIn_->target == target && signIn_->bridge == sentBridge_) signIn_->paused = true;
}

MenuEntry IdentityPanel::DiscordIdentityRow(const ShellView& v) const {
    const auto discord = SignInTarget::Discord();
    auto row = Row(DiscordConnectScreen, loc::T("identity.discord"), loc::T(Site(SignInSource::Discord).words.rowDetail));
    if (const auto* ember = EmberBridge(v)) ShowAccount(row, discord, ember->id);
    else row.value = loc::T("identity.discord_none");
    return row;
}

bool IdentityPanel::ShowsWait(const std::string& screen) const {
    if (!signIn_) return false;
    const auto& info = Site(signIn_->target.source);
    return AtScreenOf(signIn_->target, screen) || (info.alsoOn && screen == info.alsoOn);
}

void IdentityPanel::SignInRows(const ShellView& v, const SignInTarget& site, const netplay::IdentityBridge& target, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& info = Site(site.source);
    const auto& ids = info.rows;
    const auto& words = info.words;
    auto service = InfoRow(ids.service, loc::T("identity.service"), target.name.empty() ? target.origin : target.name, ServiceDetail(target));
    service.userText = true; rows.push_back(std::move(service));
    // Read, and the latest read did not fail.
    const auto* account = FindAccount(site, target.id);
    const bool failed = account && account->failed;
    const bool read = bridge_ == target.id && account && account->read && !failed;
    const bool starting = signIn_ && signIn_->target == site && Starting();
    // Whether the service offers its sign-in, as its profile last said; none while unknown.
    const bool offKnown = v.identity.inspected.id == target.id;
    const bool offered = v.identity.inspectedDiscord;
    const auto word = [](const char* id) { return std::string(loc::T(id)); };
    if (WaitingOn(site, target.id)) {
        rows.push_back(InfoRow(ids.waiting, word(words.waitingLabel), {}, word(words.waitingDetail)));
    } else if (starting && busy) {
        rows.push_back(InfoRow(ids.progress, word(words.settingUp), {}, word(words.settingUpDetail)));
    } else if (read && !account->user.empty()) {
        auto connected = InfoRow(ids.connected, word(words.connected), account->name, word(words.connectedDetail));
        connected.userText = true; rows.push_back(std::move(connected));
        // Another account, or none: both here. Another needs the sign-in on.
        rows.push_back(Row(ids.connect, word(words.change), word(words.changeDetail), !busy && (offered || !offKnown)));
        auto remove = ConfirmRow(ids.remove, loc::T(words.removeLabel), word(words.removeDetail), !busy);
        remove.hint = loc::T(words.removeLabel); rows.push_back(std::move(remove));
    } else if (offKnown && !offered && !failed) {
        // Off as the latest read said; a failed read offers Try again below.
        rows.push_back(InfoRow(ids.off, word(words.off), {}, word(words.offDetail)));
    } else {
        rows.push_back(Row(ids.connect, word(words.title), word(words.connectDetail), !busy && read));
        // Nothing in flight and the account not read: reading it failed.
        if (!busy && !read) rows.push_back(Row(ids.retry, word(words.retry), word(words.retryDetail)));
    }
}

bool IdentityPanel::SignInActivate(const MenuAction& a, const ShellView& v) {
    for (const auto source : SignInSources) {
        const auto& info = Site(source);
        const SignInTarget target{source};
        if (a.id == info.rows.connect) {
            message_.clear();
            if (!bridge_.empty()) OpenSignIn(target, bridge_);
        } else if (a.id == info.rows.remove) {
            message_.clear(); Queue(Request(target, SignInStep::Remove, bridge_));
        } else if (a.id == info.rows.cancel) {
            CancelAttempt(); Say(loc::T(info.words.cancelled), Tone::Success);
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
    const SignInTarget target{signIn->source};
    const auto& info = Site(target.source);
    const auto read = (id.*info.account)();
    using Phase = SignInAttempt::Phase;
    const bool waitedFor = signIn_ && signIn_->target == target && signIn_->bridge == sentBridge_;
    switch (signIn->step) {
    case SignInStep::Status: {
        Account(target, sentBridge_).Set(read);
        // Only the service and site the sign-in is for end the wait, and only
        // with an account connected since the page opened.
        if (waitedFor && signIn_->until > 0 && read.ConnectedSince(signIn_->before)) {
            signIn_.reset();
            Say(loc::Tf(info.words.done, read.name), Tone::Success, 20);
        }
        break;
    }
    case SignInStep::Connect:
        // A cancelled attempt does not wait, though its page may have opened.
        if (!waitedFor || signIn_->phase != Phase::Opening) break;
        Say(loc::T(info.words.opened), Tone::Success, 30);
        signIn_->phase = Phase::Waiting;
        signIn_->until = now_ + SignInSeconds; signIn_->pollAt = now_ + PollSeconds; signIn_->paused = false;
        break;
    case SignInStep::Remove: {
        Account(target, sentBridge_).Set({});
        if (waitedFor) signIn_.reset();
        Say(loc::T(info.words.removed), Tone::Success);
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
    Queue(Request(signIn_->target, SignInStep::Status, signIn_->bridge));
}
} }
