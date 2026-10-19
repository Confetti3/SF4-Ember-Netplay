// The browser sign-in Discord and the listed sign-in sites share
// (SignInSites.hxx): the helper opens the site's page, the player approves
// there, and these screens read the account on the service every few seconds
// until a different one appears. One sign-in runs at a time: starting one
// ends any other on its service. Leaving the Connect screen while its page is
// being opened stops it; a wait for the browser goes on, and Cancel ends the
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

IdentityRequest Request(const SignInTarget& target, SignInStep step, const std::string& bridge) {
    IdentityRequest r; r.op = netplay::SignInRequest(target.source, step); r.bridge = bridge; r.site = target.site; return r;
}
}

const netplay::IdentitySignInSite* IdentityPanel::ListedSite(const std::string& bridge, const std::string& site) const {
    const auto listed = sites_.find(bridge);
    if (listed == sites_.end()) return nullptr;
    const auto it = std::find_if(listed->second.begin(), listed->second.end(), [&](const netplay::IdentitySignInSite& s) { return s.id == site; });
    return it == listed->second.end() ? nullptr : &*it;
}

std::string IdentityPanel::SiteName(const std::string& bridge, const std::string& site) const {
    if (const auto it = siteNames_.find({bridge, site}); it != siteNames_.end()) return it->second;
    if (bridge.empty())
        for (const auto& named : siteNames_)
            if (named.first.second == site) return named.second;
    return site;
}

std::string IdentityPanel::Word(const SignInTarget& target, const std::string& bridge, const char* id) const {
    return Site(target.source).named ? loc::Tf(id, SiteName(bridge, target.site)) : std::string(loc::T(id));
}

std::string IdentityPanel::Word(const SignInTarget& target, const std::string& bridge, const char* id, const std::string& value) const {
    return Site(target.source).named ? loc::Tf(id, SiteName(bridge, target.site), value) : loc::Tf(id, value);
}

SignInTarget IdentityPanel::SentTarget() const {
    const auto signIn = netplay::SignInOf(sentOp_);
    return signIn && signIn->source == SignInSource::Site ? SignInTarget::Listed(sentSite_) : SignInTarget::Discord();
}

IdentityPanel::SiteAccount& IdentityPanel::Account(const SignInTarget& target, const std::string& bridge) {
    return accounts_[{target.source, target.site, bridge}];
}

const IdentityPanel::SiteAccount* IdentityPanel::FindAccount(const SignInTarget& target, const std::string& bridge) const {
    const auto it = accounts_.find({target.source, target.site, bridge});
    return it == accounts_.end() ? nullptr : &it->second;
}

std::string IdentityPanel::SiteBridge(const ShellView& v, const SignInTarget& target) const {
    if (!Site(target.source).embersService) return bridge_;
    const auto* ember = EmberBridge(v);
    return ember ? ember->id : std::string();
}

bool IdentityPanel::AtScreenOf(const SignInTarget& target, const std::string& screen) const {
    if (screen != Site(target.source).screen) return false;
    return target.source == SignInSource::Discord || siteScreen_ == target.site;
}

void IdentityPanel::ShowAccount(MenuEntry& row, const SignInTarget& target, const std::string& bridge) const {
    const auto* account = FindAccount(target, bridge);
    if (!account || !account->read || account->failed) return;
    const bool named = !account->user.empty();
    row.value = named ? account->name : Word(target, bridge, Site(target.source).words.none);
    row.userText = named;
}

bool IdentityPanel::Starting() const {
    assert(steps_ == Steps::None || !signIn_);
    return steps_ == Steps::Running || (signIn_ && signIn_->phase == SignInAttempt::Phase::Opening);
}

bool IdentityPanel::ActiveFor(const SignInTarget& target) const {
    return StepsRunFor(target) || (signIn_ && signIn_->target == target && signIn_->phase != SignInAttempt::Phase::Stopped);
}

void IdentityPanel::EndOtherSignIn(const SignInTarget& target) {
    if (signIn_ && signIn_->target != target && signIn_->phase != SignInAttempt::Phase::Stopped) CancelAttempt();
}

void IdentityPanel::RetireSignIn() {
    if (!signIn_) return;
    // The journey's own sign-in is no longer waited for, as it always was.
    if (Site(signIn_->target.source).followsJourney) { signIn_.reset(); return; }
    // Another site's waiting for the browser goes on: Discord's steps or page
    // replace it only once they start (EndOtherSignIn). One whose page is
    // still being opened ends on its service, since the new journey retires
    // the answer it waits for. A page the helper is already opening still
    // opens, harmlessly: the cancel that follows means it connects nothing.
    if (signIn_->phase == SignInAttempt::Phase::Opening) CancelAttempt();
}

std::optional<std::string> IdentityPanel::SiteFailureText(const std::string& code) const {
    if (!netplay::SignInOf(sentOp_)) return std::nullopt;
    const auto target = SentTarget();
    const auto& words = Site(target.source).words;
    const char* said = code == "sign_in_busy" ? words.busy : code == "not_found" ? words.gone
        : code == "sign_in_off" ? words.refusedOff : nullptr;
    return said ? std::optional<std::string>(Word(target, sentBridge_, said)) : std::nullopt;
}

void IdentityPanel::SiteRefused(const std::string& code) {
    const auto signIn = netplay::SignInOf(sentOp_);
    if (code != "sign_in_off" || !signIn || signIn->source != SignInSource::Site) return;
    const auto listed = sites_.find(sentBridge_);
    if (listed == sites_.end()) return;
    for (auto& site : listed->second)
        if (site.id == sentSite_) site.signIn = false;
}

bool IdentityPanel::WaitingOn(const SignInTarget& target, const std::string& bridge) const {
    return signIn_ && signIn_->target == target && signIn_->bridge == bridge && signIn_->until > 0 && !signIn_->paused;
}

void IdentityPanel::OpenSignIn(const SignInTarget& target, const std::string& bridge) {
    EndOtherSignIn(target);
    const auto* account = FindAccount(target, bridge);
    steps_ = Steps::None;
    SignInAttempt next{target, bridge, account ? account->user : std::string()};
    // The same site again on the same service (Discord's change of account):
    // the wait already running goes on as it always has, still shown and
    // still read every few seconds, until the new page's answer starts it again.
    if (signIn_ && signIn_->target == target && signIn_->bridge == bridge) {
        next.until = signIn_->until; next.pollAt = signIn_->pollAt; next.paused = signIn_->paused;
    }
    signIn_ = std::move(next);
    Queue(Request(target, SignInStep::Connect, bridge));
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
    if (asked) cleanup_.push_back(Request(signIn_->target, SignInStep::Cancel, signIn_->bridge));
    signIn_ = SignInAttempt{signIn_->target, signIn_->bridge, signIn_->before, Phase::Stopped};
}

void IdentityPanel::ReadSiteAccounts(const netplay::IdentityView& id, const std::string& bridge, const AccountReads& reads) {
    if (reads.ReadsDiscord()) {
        // A service without Discord accounts: its profile was the whole read, and it worked.
        if (id.inspectedDiscord || id.inspectedDiscordAccounts) Queue(Request(SignInTarget::Discord(), SignInStep::Status, bridge));
        else Account(SignInTarget::Discord(), bridge).failed = false;
    }
    if (!reads.ReadsSites()) return;
    // The sites it lists, as its profile says now. One no longer listed loses
    // its row and what was read of it, and a sign-in on it ends: its screen,
    // if open, says its sign-in is off.
    auto& listed = sites_[bridge];
    for (const auto& was : listed) {
        const bool kept = std::any_of(id.signInSites.begin(), id.signInSites.end(),
            [&](const netplay::IdentitySignInSite& site) { return site.id == was.id; });
        if (kept) continue;
        const auto target = SignInTarget::Listed(was.id);
        accounts_.erase({target.source, target.site, bridge});
        if (signIn_ && signIn_->target == target && signIn_->bridge == bridge && signIn_->phase != SignInAttempt::Phase::Stopped)
            CancelAttempt();
    }
    listed = id.signInSites;
    for (const auto& site : listed) {
        if (!site.name.empty()) siteNames_[{bridge, site.id}] = site.name;
        if (reads.Reads(site.id)) Queue(Request(SignInTarget::Listed(site.id), SignInStep::Status, bridge));
    }
}

void IdentityPanel::AccountReadsFailed(const std::string& bridge, const AccountReads& reads) {
    if (reads.ReadsDiscord()) Account(SignInTarget::Discord(), bridge).failed = true;
    const auto listed = sites_.find(bridge);
    if (listed == sites_.end()) return;
    for (const auto& site : listed->second)
        if (reads.Reads(site.id)) Account(SignInTarget::Listed(site.id), bridge).failed = true;
}

bool IdentityPanel::QuietRead() const {
    const auto signIn = netplay::SignInOf(sentOp_);
    if (!signIn || signIn->step != SignInStep::Status) return false;
    return Site(signIn->source).quietElsewhere && !AtScreenOf(SentTarget(), lastScreen_);
}

void IdentityPanel::SiteReadFailed() {
    if (!netplay::SignInOf(sentOp_)) return;
    const auto target = SentTarget();
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

void IdentityPanel::ListedSiteIdentityRows(const ShellView& v, std::vector<MenuEntry>& rows) const {
    const auto* ember = EmberBridge(v);
    const auto listed = ember ? sites_.find(ember->id) : sites_.end();
    if (listed == sites_.end()) return;
    for (const auto& site : listed->second) {
        const auto target = SignInTarget::Listed(site.id);
        auto row = Row(SiteRowPrefix + site.id, SiteName(ember->id, site.id), Word(target, ember->id, Site(SignInSource::Site).words.rowDetail));
        ShowAccount(row, target, ember->id);
        // The site's name comes from the service.
        row.userText = true;
        rows.push_back(std::move(row));
    }
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
    const bool read = SiteBridge(v, site) == target.id && account && account->read && !failed;
    const bool starting = StepsRunFor(site) || (signIn_ && signIn_->target == site && signIn_->phase == SignInAttempt::Phase::Opening);
    // Whether the service offers its sign-in, as its profile last said; none while unknown.
    const auto* listed = site.source == SignInSource::Site ? ListedSite(target.id, site.site) : nullptr;
    const bool offKnown = site.source == SignInSource::Discord ? v.identity.inspected.id == target.id : true;
    const bool offered = site.source == SignInSource::Discord ? v.identity.inspectedDiscord : listed && listed->signIn;
    const auto word = [&](const char* id) { return Word(site, target.id, id); };
    // A label carrying a listed site's name, which its service gives: elided
    // when long, as a player's own text is, rather than checked to fit.
    const auto named = [&](MenuEntry row) { row.userText = info.named; return row; };
    if (WaitingOn(site, target.id)) {
        rows.push_back(named(InfoRow(ids.waiting, word(words.waitingLabel), {}, word(words.waitingDetail))));
    } else if (starting && busy) {
        rows.push_back(InfoRow(ids.progress, word(words.settingUp), {}, word(words.settingUpDetail)));
    } else if (read && !account->user.empty()) {
        auto connected = InfoRow(ids.connected, word(words.connected), account->name, word(words.connectedDetail));
        connected.userText = true; rows.push_back(std::move(connected));
        // Another account, or none: both here.
        rows.push_back(named(Row(ids.connect, word(words.change), word(words.changeDetail), !busy)));
        auto remove = ConfirmRow(ids.remove, loc::T(words.removeLabel), word(words.removeDetail), !busy);
        remove.hint = loc::T(words.removeLabel); rows.push_back(std::move(remove));
    } else if (offKnown && !offered && !failed) {
        // Off as the latest read said; a failed read offers Try again below.
        rows.push_back(named(InfoRow(ids.off, word(words.off), {}, word(words.offDetail))));
    } else {
        rows.push_back(named(Row(ids.connect, word(words.title), word(words.connectDetail), !busy && read)));
        // Nothing in flight and the account not read: reading it failed.
        if (!busy && !read) rows.push_back(Row(ids.retry, word(words.retry), word(words.retryDetail)));
    }
}

void IdentityPanel::ListedSiteRows(const ShellView& v, std::vector<MenuEntry>& rows, bool busy) const {
    const auto target = SignInTarget::Listed(siteScreen_);
    const auto& info = Site(SignInSource::Site);
    if (v.identity.state != "ready") {
        rows.push_back(InfoRow("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
        return;
    }
    const auto* ember = EmberBridge(v);
    const auto bridge = ember ? ember->id : std::string();
    if (ember) SignInRows(v, target, *ember, rows, busy);
    else {
        // The sites connect through Ember's own service, which the player has not trusted.
        rows.push_back(InfoRow("st-no-service", loc::T("site.no_service"), {}, Word(target, bridge, "site.no_service_detail")));
        rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
    }
    if (ActiveFor(target)) rows.push_back(Row(info.rows.cancel, loc::T("common.cancel"), Word(target, bridge, info.words.cancelDetail)));
}

bool IdentityPanel::SignInActivate(const MenuAction& a, const ShellView& v) {
    for (const auto source : SignInSources) {
        const auto& info = Site(source);
        const auto target = source == SignInSource::Discord ? SignInTarget::Discord() : SignInTarget::Listed(siteScreen_);
        if (a.id == info.rows.connect) {
            message_.clear();
            const auto bridge = SiteBridge(v, target);
            if (!bridge.empty()) OpenSignIn(target, bridge);
        } else if (a.id == info.rows.remove) {
            message_.clear(); Queue(Request(target, SignInStep::Remove, SiteBridge(v, target)));
        } else if (a.id == info.rows.cancel) {
            CancelAttempt(); Say(Word(target, SiteBridge(v, target), info.words.cancelled), false);
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
    const auto target = SentTarget();
    const auto& info = Site(target.source);
    const auto& user = id.*info.user;
    const auto& name = id.*info.name;
    using Phase = SignInAttempt::Phase;
    const bool waitedFor = signIn_ && signIn_->target == target && signIn_->bridge == sentBridge_;
    switch (signIn->step) {
    case SignInStep::Status: {
        // A site its service stopped listing since the read was asked keeps nothing.
        if (target.source == SignInSource::Site && !ListedSite(sentBridge_, target.site)) break;
        auto& account = Account(target, sentBridge_);
        account.user = user; account.name = name; account.read = true; account.failed = false;
        // Only the service and site the sign-in is for end the wait, and only
        // with an account other than the one connected when the page opened.
        if (waitedFor && signIn_->until > 0 && !user.empty() && user != signIn_->before) {
            signIn_.reset();
            Say(Word(target, sentBridge_, info.words.done, name), false, 20);
        }
        break;
    }
    case SignInStep::Connect:
        // A cancelled attempt does not wait, though its page may have opened.
        if (!waitedFor || signIn_->phase != Phase::Opening) break;
        Say(Word(target, sentBridge_, info.words.opened), false, 30);
        signIn_->phase = Phase::Waiting;
        signIn_->until = now_ + SignInSeconds; signIn_->pollAt = now_ + PollSeconds; signIn_->paused = false;
        break;
    case SignInStep::Remove: {
        auto& account = Account(target, sentBridge_);
        account.user.clear(); account.name.clear(); account.read = true; account.failed = false;
        if (waitedFor) signIn_.reset();
        Say(Word(target, sentBridge_, info.words.removed), false);
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
