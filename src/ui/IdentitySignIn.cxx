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

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;
namespace {
// While a site's page is open in the browser, the account is read this
// often, for as long as the bridge keeps the sign-in.
constexpr double PollSeconds = 4, SignInSeconds = 600, OrganizerSeconds = 180;

IdentityRequest Request(const SignInTarget& target, SignInStep step, const std::string& bridge) {
    IdentityRequest r; r.op = netplay::SignInRequest(target.source, step); r.bridge = bridge; r.site = target.site; return r;
}

// The organizer sign-in of a listed site: a status, a sign-in, or dropping the grant.
IdentityRequest Organizer(const std::string& site, SignInStep step, const std::string& bridge) {
    auto r = Request(SignInTarget::Listed(site), step, bridge); r.organizer = true; return r;
}
}

const netplay::IdentitySignInSite* IdentityPanel::ListedSite(const std::string& bridge, const std::string& site) const {
    if (bridge != sitesBridge_) return nullptr;
    const auto it = std::find_if(sites_.begin(), sites_.end(), [&](const netplay::IdentitySignInSite& s) { return s.id == site; });
    return it == sites_.end() ? nullptr : &*it;
}

std::string IdentityPanel::SiteName(const std::string& site) const {
    const auto it = siteNames_.find(site);
    return it == siteNames_.end() ? site : it->second;
}

std::string IdentityPanel::Word(const SignInTarget& target, const char* id) const {
    return Site(target.source).listed ? loc::Tf(id, SiteName(target.site)) : std::string(loc::T(id));
}

std::string IdentityPanel::Word(const SignInTarget& target, const char* id, const std::string& value) const {
    return Site(target.source).listed ? loc::Tf(id, SiteName(target.site), value) : loc::Tf(id, value);
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
    if (!Site(target.source).listed) return bridge_;
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
    row.value = named ? account->name : Word(target, Site(target.source).words.none);
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
    if (!signIn_) return;
    // The journey's own sign-in, its steps included, is no longer waited for, as it always was.
    if (!Site(signIn_->target.source).listed) { signIn_.reset(); return; }
    // A site's waiting for the browser goes on: Discord's steps replace it
    // only once they start (StartSteps). One whose page is still being opened
    // ends on its service, since the new journey retires the answer it waits
    // for. A page the helper is already opening still opens, harmlessly: the
    // cancel that follows means it connects nothing.
    if (signIn_->phase == SignInAttempt::Phase::Opening) CancelAttempt();
}

std::optional<std::string> IdentityPanel::SiteFailureText(const std::string& code) const {
    const auto signIn = netplay::SignInOf(sentOp_);
    if (!signIn || signIn->source != SignInSource::Site) return std::nullopt;
    const char* said = code == "sign_in_busy" ? "site.busy" : code == "not_found" ? "site.gone"
        : code == "sign_in_off" ? Site(SignInSource::Site).words.offDetail : nullptr;
    return said ? std::optional<std::string>(Word(SentTarget(), said)) : std::nullopt;
}

void IdentityPanel::SiteRefused(const std::string& code) {
    const auto signIn = netplay::SignInOf(sentOp_);
    if (!signIn || signIn->source != SignInSource::Site || sentBridge_ != sitesBridge_) return;
    if (code == "sign_in_off") {
        for (auto& site : sites_)
            if (site.id == sentSite_) site.signIn = false;
    } else if (code == "not_found" && signIn->step == SignInStep::Connect) {
        DropSite(sentSite_);
    }
}

void IdentityPanel::DropSite(const std::string& site) {
    const auto target = SignInTarget::Listed(site);
    sites_.erase(std::remove_if(sites_.begin(), sites_.end(), [&](const netplay::IdentitySignInSite& s) { return s.id == site; }), sites_.end());
    accounts_.erase({target.source, target.site, sitesBridge_});
    if (signIn_ && signIn_->target == target && signIn_->bridge == sitesBridge_ && signIn_->phase != SignInAttempt::Phase::Stopped)
        CancelAttempt();
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
    if (!reads.ReadsSites()) return;
    // The sites Ember's own service lists, as its profile says now. One no
    // longer listed goes (DropSite); another service's list replaces the
    // last one whole.
    // A sign-in on the old service's sites is not ended here: only Ember's
    // own service lists sites, so the list only changes service when Ember's
    // own service itself is replaced.
    if (bridge != sitesBridge_) { sitesBridge_ = bridge; sites_.clear(); siteNames_.clear(); }
    std::vector<std::string> gone;
    for (const auto& was : sites_) {
        const bool kept = std::any_of(id.signInSites.begin(), id.signInSites.end(),
            [&](const netplay::IdentitySignInSite& site) { return site.id == was.id; });
        if (!kept) gone.push_back(was.id);
    }
    for (const auto& site : gone) DropSite(site);
    sites_ = id.signInSites;
    for (const auto& site : sites_) {
        if (!site.name.empty()) siteNames_[site.id] = site.name;
        if (!reads.Reads(site.id)) continue;
        Queue(Request(SignInTarget::Listed(site.id), SignInStep::Status, bridge));
        if (site.organizer) Queue(Organizer(site.id, SignInStep::Status, bridge));
    }
}

void IdentityPanel::AccountReadsFailed(const std::string& bridge, const AccountReads& reads) {
    if (reads.ReadsDiscord()) Account(SignInTarget::Discord(), bridge).failed = true;
    if (bridge != sitesBridge_) return;
    for (const auto& site : sites_)
        if (reads.Reads(site.id)) Account(SignInTarget::Listed(site.id), bridge).failed = true;
}

bool IdentityPanel::QuietRead() const {
    const auto signIn = netplay::SignInOf(sentOp_);
    if (!signIn || signIn->step != SignInStep::Status) return false;
    return Site(signIn->source).listed && !AtScreenOf(SentTarget(), lastScreen_);
}

void IdentityPanel::SiteReadFailed() {
    // An organizer read that failed leaves the account as it was read.
    if (!netplay::SignInOf(sentOp_) || sentOrganizer_) return;
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
    if (!ember || ember->id != sitesBridge_) return;
    for (const auto& site : sites_) {
        const auto target = SignInTarget::Listed(site.id);
        auto row = Row(SiteRowPrefix + site.id, SiteName(site.id), Word(target, Site(SignInSource::Site).words.rowDetail));
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
    const bool starting = signIn_ && signIn_->target == site && Starting();
    // Whether the service offers its sign-in, as its profile last said; none while unknown.
    const auto* listed = info.listed ? ListedSite(target.id, site.site) : nullptr;
    const bool offKnown = info.listed || v.identity.inspected.id == target.id;
    const bool offered = info.listed ? listed && listed->signIn : v.identity.inspectedDiscord;
    const auto word = [&](const char* id) { return Word(site, id); };
    // A label carrying a listed site's name, which its service gives: elided
    // when long, as a player's own text is, rather than checked to fit.
    const auto named = [&](MenuEntry row) { row.userText = info.listed; return row; };
    if (WaitingOn(site, target.id)) {
        rows.push_back(named(InfoRow(ids.waiting, word(words.waitingLabel), {}, word(words.waitingDetail))));
    } else if (starting && busy) {
        rows.push_back(InfoRow(ids.progress, word(words.settingUp), {}, word(words.settingUpDetail)));
    } else if (read && !account->user.empty()) {
        auto connected = InfoRow(ids.connected, word(words.connected), account->name, word(words.connectedDetail));
        connected.userText = true; rows.push_back(std::move(connected));
        // Another account, or none: both here. Another needs the sign-in on.
        rows.push_back(named(Row(ids.connect, word(words.change), word(words.changeDetail), !busy && (offered || !offKnown))));
        auto remove = ConfirmRow(ids.remove, loc::T(words.removeLabel), word(words.removeDetail), !busy);
        remove.hint = loc::T(words.removeLabel); rows.push_back(std::move(remove));
        // A site that also runs tournaments through Ember: organizer access
        // is one place, Ember ID › Tournament organizer.
        if (listed && listed->organizer)
            rows.push_back(InfoRow("st-organizer-hint", loc::T("site.organizer"), {}, loc::T("site.organizer_where")));
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
    if (v.identity.state != "ready") {
        rows.push_back(InfoRow("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
    } else if (const auto* ember = EmberBridge(v)) {
        SignInRows(v, target, *ember, rows, busy);
    } else {
        // The sites connect through Ember's own service, which the player has not trusted.
        rows.push_back(InfoRow("st-no-service", loc::T("site.no_service"), {}, Word(target, "site.no_service_detail")));
        rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
    }
    // A sign-in under way can always be stopped, even with the ID not ready.
    if (ActiveFor(target)) rows.push_back(Row(Site(SignInSource::Site).rows.cancel, loc::T("common.cancel"), Word(target, "site.cancel_detail")));
}

void IdentityPanel::OrganizerAccess(const ShellView& v, const std::string& site, bool allow) {
    message_.clear();
    const auto bridge = SiteBridge(v, SignInTarget::Listed(site));
    if (!bridge.empty()) Queue(Organizer(site, allow ? SignInStep::Connect : SignInStep::Remove, bridge));
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
            CancelAttempt(); Say(Word(target, info.words.cancelled), Tone::Success);
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
    const auto read = (id.*info.account)();
    using Phase = SignInAttempt::Phase;
    const bool waitedFor = signIn_ && signIn_->target == target && signIn_->bridge == sentBridge_;
    // The organizer sign-in: once its page opened, the grant is read again
    // every few seconds (SignInPoll) until it shows or three minutes pass.
    if (sentOrganizer_) {
        if (signIn->step == SignInStep::Connect) {
            Say(loc::T("site.organizer_opened"), false, 30);
            organizerWait_ = {target.site, sentBridge_, now_ + OrganizerSeconds, now_ + PollSeconds};
        } else if (ListedSite(sentBridge_, target.site)) {
            Account(target, sentBridge_).organizer = id.siteOrganizer;
            if (signIn->step == SignInStep::Remove || id.siteOrganizer) {
                if (id.siteOrganizer && organizerWait_ && organizerWait_->until > 0) Say(loc::T("site.organizer_now"), false, 20);
                deskReadAt_ = 0;
                organizerWait_.reset();
            }
        }
        return;
    }
    switch (signIn->step) {
    case SignInStep::Status: {
        // A site its service stopped listing since the read was asked keeps nothing.
        if (info.listed && !ListedSite(sentBridge_, target.site)) break;
        Account(target, sentBridge_).Set(read);
        // Only the service and site the sign-in is for end the wait, and only
        // with an account connected since the page opened.
        if (waitedFor && signIn_->until > 0 && read.ConnectedSince(signIn_->before)) {
            signIn_.reset();
            Say(Word(target, info.words.done, read.name), Tone::Success, 20);
        }
        break;
    }
    case SignInStep::Connect:
        // A cancelled attempt does not wait, though its page may have opened.
        if (!waitedFor || signIn_->phase != Phase::Opening) break;
        Say(Word(target, info.words.opened), Tone::Success, 30);
        signIn_->phase = Phase::Waiting;
        signIn_->until = now_ + SignInSeconds; signIn_->pollAt = now_ + PollSeconds; signIn_->paused = false;
        break;
    case SignInStep::Remove: {
        Account(target, sentBridge_).Set({});
        if (waitedFor) signIn_.reset();
        Say(Word(target, info.words.removed), Tone::Success);
        break;
    }
    case SignInStep::Cancel: break;
    }
}

void IdentityPanel::SignInPoll(const std::string& screen, double now) {
    using Phase = SignInAttempt::Phase;
    if (organizerWait_ && !sent_ && queue_.empty() && now >= organizerWait_->pollAt
        && (screen == "tournament-desk" || AtScreenOf(SignInTarget::Listed(organizerWait_->site), screen))) {
        if (now > organizerWait_->until) {
            organizerWait_.reset();
        } else {
            organizerWait_->pollAt = now + PollSeconds;
            Queue(Organizer(organizerWait_->site, SignInStep::Status, organizerWait_->bridge));
        }
        return;
    }
    if (!signIn_ || signIn_->until <= 0 || signIn_->paused || sent_ || !queue_.empty() || now < signIn_->pollAt || !ShowsWait(screen)) return;
    if (now > signIn_->until) { signIn_->phase = Phase::Stopped; signIn_->until = 0; return; }
    signIn_->pollAt = now + PollSeconds;
    Queue(Request(signIn_->target, SignInStep::Status, signIn_->bridge));
}
} }
