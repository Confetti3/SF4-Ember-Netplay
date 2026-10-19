#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>
#include <algorithm>
#include <cstring>

namespace sf4e { namespace ui {
namespace {
using netplay::IdentityOp;
using netplay::IdentityRequest;
// An answer gets this long before the panel stops waiting for it. Key work
// takes seconds; a bridge request is bounded by the helper's own timeouts.
constexpr double AnswerSeconds = 120;
// A failed background read of the state or services is tried again after this.
constexpr double BackgroundRetrySeconds = 30;
// Settings > Discord is none of these screens, but its Tournament account row
// reads the account on the selected service each time it opens, as they do.
constexpr const char* AccountScreen = "discord";
// Requests waiting to be sent: room for the screens' usual few, and a read
// of every sign-in site a service may list (the Ember ID screen reads them all).
constexpr std::size_t MaxQueued = 8 + netplay::MaxSignInSites;

// The helper's and the bridge's failure codes, in words. Codes that mean the
// same thing to the player share a sentence.
std::string FailureText(const std::string& code) {
    static const std::pair<const char*, const char*> table[] = {
        {"wrong_passphrase", "identity.failure.wrong_passphrase"}, {"decrypt_failed", "identity.failure.wrong_passphrase"},
        {"invalid_passphrase", "identity.failure.invalid_passphrase"},
        {"passphrase_required", "identity.failure.passphrase_required"},
        {"corrupt", "identity.failure.corrupt"}, {"unsupported_envelope", "identity.failure.corrupt"},
        {"file_unreadable", "identity.failure.file"}, {"invalid_path", "identity.failure.file"},
        {"identity_busy", "identity.failure.busy"},
        {"identity_unavailable", "identity.failure.state"}, {"wrong_state", "identity.failure.state"},
        {"key_missing", "identity.failure.state"},
        {"confirmation_mismatch", "identity.failure.changed"}, {"replacement_not_confirmed", "identity.failure.changed"},
        {"permission_denied", "identity.failure.storage"}, {"io", "identity.failure.storage"},
        {"random_unavailable", "identity.failure.storage"}, {"metadata_mismatch", "identity.failure.storage"},
        {"bridge_unreachable", "identity.failure.unreachable"}, {"service_unavailable", "identity.failure.unreachable"},
        {"not_a_bridge", "identity.failure.not_bridge"}, {"invalid_origin", "identity.failure.not_bridge"},
        {"unsupported_bridge", "identity.failure.not_bridge"}, {"invalid_bridge", "identity.failure.not_bridge"},
        {"bridge_invalid_response", "identity.failure.not_bridge"}, {"bridge_response_too_large", "identity.failure.not_bridge"},
        {"unsupported_version", "identity.failure.not_bridge"},
        {"bridge_changed", "identity.failure.bridge_changed"}, {"bridge_mismatch", "identity.failure.bridge_changed"},
        {"pinned_bridge_mismatch", "identity.failure.pin_mismatch"},
        {"bridge_not_approved", "identity.failure.not_approved"},
        {"too_many_bridges", "identity.failure.too_many"},
        {"link_expired", "identity.failure.code"}, {"not_found", "identity.failure.code"},
        {"link_conflict", "identity.failure.link_conflict"},
        {"rate_limited", "identity.failure.rate_limited"},
        {"challenge_mismatch", "identity.failure.sign_in"}, {"invalid_signature", "identity.failure.sign_in"},
        {"challenge_expired", "identity.failure.sign_in"}, {"challenge_used", "identity.failure.sign_in"},
        {"unauthenticated", "identity.failure.sign_in"},
        {"forbidden", "identity.failure.forbidden"}, {"invalid_request", "identity.failure.refused"},
        {"browser_unavailable", "identity.failure.browser"},
    };
    for (const auto& entry : table) if (code == entry.first) return loc::T(entry.second);
    return loc::Tf("identity.failure.other", code.empty() ? std::string("?") : code);
}

const char* SecretValue(const std::string& secret) {
    return secret.empty() ? loc::T("identity.secret_empty") : loc::T("identity.secret_set");
}
MenuEntry Secret(std::string id, std::string label, const std::string& secret, std::string detail, bool enabled) {
    auto e = TextRow(std::move(id), std::move(label), SecretValue(secret), 256, enabled);
    e.secret = true; e.detail = std::move(detail); return e;
}
// The two passphrase fields agree and are not empty.
bool Matching(const std::string& a, const std::string& b) { return !a.empty() && a == b; }
std::string PairDetail(const std::string& a, const std::string& b, const char* ready) {
    return !a.empty() && !b.empty() && a != b ? loc::T("identity.passphrases_differ") : loc::T(ready);
}
void Assign(std::string& target, const std::string& text) { WipeText(target); target = text; }
}

bool IdentityPanel::Owns(const std::string& screen) {
    return screen == "identity" || screen == "identity-backup" || screen == "linked-accounts" || screen == "tournament-matches" ||
        screen == DiscordConnectScreen || screen == SiteConnectScreen;
}

// Requests whose answers only show something or start a wait for a site's
// sign-in: a replaced journey's are retired. Changes the player asked for are not.
static bool Retirable(IdentityOp op) {
    using netplay::IdentityEffect;
    const auto effect = netplay::EffectOf(op);
    return effect == IdentityEffect::Read || effect == IdentityEffect::Preview || effect == IdentityEffect::Open;
}
// Ending a sign-in the player cancelled or left: tidying up nobody waits for.
static bool IsCleanup(IdentityOp op) { return netplay::EffectOf(op) == netplay::IdentityEffect::Cleanup; }

bool IdentityPanel::Superseded() const {
    if (sentOwner_ == Owner::PublicSetup) return sentJourney_ != setup_.Run();
    return sentJourney_ != journey_ && Retirable(sentOp_);
}

void IdentityPanel::ReadFailed() {
    // A background read on Home is tried again a little later.
    if (!Owns(lastScreen_) && Retirable(sentOp_)) backgroundAt_ = now_ + BackgroundRetrySeconds;
    if (sentJourney_ == journey_ && Starting()) signIn_->phase = SignInAttempt::Phase::Stopped;
    if (const auto signIn = netplay::SignInOf(sentOp_); signIn && signIn->step == SignInStep::Status) {
        SiteReadFailed();
    } else if (sentOp_ == IdentityOp::BridgeInspect && !sentLookUp_) {
        // The service the inspection was for, and whose accounts it was to
        // read: an account read's own, or the selected one.
        // Linked accounts and the matches screen read only Discord's account on the selected one.
        const std::string& target = sentAccount_.empty() ? bridge_ : sentAccount_;
        if (!target.empty()) AccountReadsFailed(target, sentAccount_.empty() ? AccountReads::Discord() : sentReads_);
    }
}

void IdentityPanel::ReadAccount(const netplay::IdentityBridge& bridge, AccountReads reads) {
    IdentityRequest inspect; inspect.op = IdentityOp::BridgeInspect; inspect.origin = bridge.origin;
    Queue(std::move(inspect), false, bridge.id, std::move(reads));
}

void IdentityPanel::DropQueue() {
    if (sentJourney_ != journey_) return;
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Queued& q) { return q.owner == Owner::Screens; }), queue_.end());
}

void IdentityPanel::NewJourney() {
    RetireSignIn();
    ++journey_;
    discordService_.clear();
    // The public rooms setup's requests are not the journey's.
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Queued& q) {
        return q.owner == Owner::Screens && Retirable(q.request.op);
    }), queue_.end());
}

std::string IdentityPanel::EntryDetail(const ShellView& v) const {
    const auto& id = v.identity;
    if (id.known && id.state == "disabled") return loc::T("home.identity_start");
    if (id.known && id.state == "ready" && servicesKnown_ && !FirstService(v)) return loc::T("home.identity_start");
    if (const int ready = ReadyMatches(v)) return loc::Tf("home.identity_matches", ready);
    return loc::T("home.identity_detail");
}

std::string IdentityPanel::TournamentFailure(const std::string& code) {
    if (code == "incompatible_build") return loc::T("tournament.failure.build");
    if (code == "unsupported_rules") return loc::T("tournament.failure.rules");
    if (code == "helper_lost" || code == "helper_unavailable") return loc::T("tournament.failure.helper");
    if (code == "report_not_saved") return loc::T("tournament.failure.report");
    if (code == "not_found") return loc::T("tournament.failure.not_found");
    return FailureText(code);
}

std::string IdentityPanel::UsableBridge(const ShellView& v) const {
    return v.identity.state == "ready" && FindBridge(v, bridge_) ? bridge_ : std::string();
}

void IdentityPanel::Probe() {
    IdentityRequest status; status.op = IdentityOp::Status; Queue(std::move(status));
    IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
}

void IdentityPanel::SetUpPublicRooms(const ShellView&) {
    if (!setup_.Running()) QueueSetup(setup_.Start());
}

void IdentityPanel::QueueSetup(IdentityRequest request) {
    if (queue_.size() < MaxQueued) queue_.push_back(Queued{std::move(request), false, setup_.Run(), {}, {}, Owner::PublicSetup});
}

bool IdentityPanel::SetupPending() const {
    return (sent_ && sentOwner_ == Owner::PublicSetup) ||
        std::any_of(queue_.begin(), queue_.end(), [](const Queued& q) { return q.owner == Owner::PublicSetup; });
}

// Only the setup moves on its answers; the services it reads are fresh for Home too, and the one it trusts is selected.
void IdentityPanel::SetupFinish(const ShellView& v) {
    const auto& id = v.identity;
    if (!v.identityRequest) { setup_.Fail(RefusalText(v)); return; }
    if (!id.ok) { setup_.Failed(id.failure, FailureText(id.failure)); return; }
    if (sentOp_ == IdentityOp::BridgeList) servicesKnown_ = true;
    if (auto next = setup_.Answered(v, sentOp_)) QueueSetup(std::move(*next));
    if (setup_.View().step == PublicSetupStep::Done) bridge_ = setup_.Service();
}

bool IdentityPanel::Answered(const ShellView& v) const {
    return v.identityTicket == sent_ && (!v.identityRequest || v.identity.requestId == v.identityRequest);
}
bool IdentityPanel::Busy(const ShellView&) const { return sent_ != 0 || !queue_.empty(); }

void IdentityPanel::Say(std::string text, bool error, double seconds) {
    message_ = std::move(text); messageError_ = error; messageUntil_ = now_ + seconds;
}

void IdentityPanel::Queue(IdentityRequest request, bool lookUp, std::string account, AccountReads reads) {
    const auto owner = finishing_ && sentOwner_ == Owner::Screens ? sentJourney_ : journey_;
    // The new journey reads everything again itself.
    if (owner != journey_ && Retirable(request.op)) return;
    // A read the same as one of the screens' waiting or out for this journey
    // adds nothing: its answer is handled for whichever screen is open when it comes.
    if (netplay::EffectOf(request.op) == netplay::IdentityEffect::Read) {
        if (sent_ && sentOwner_ == Owner::Screens && sentJourney_ == owner && sentOp_ == request.op && sentBridge_ == request.bridge &&
            sentOrigin_ == request.origin && sentSite_ == request.site && sentLookUp_ == lookUp && sentAccount_ == account && sentReads_ == reads) return;
        const bool waiting = std::any_of(queue_.begin(), queue_.end(), [&](const Queued& q) {
            return q.owner == Owner::Screens && q.journey == owner && q.request.op == request.op && q.request.bridge == request.bridge &&
                q.request.origin == request.origin && q.request.site == request.site && q.lookUp == lookUp && q.account == account && q.reads == reads;
        });
        if (waiting) return;
    }
    if (queue_.size() < MaxQueued) queue_.push_back(Queued{std::move(request), lookUp, owner, std::move(account), std::move(reads)});
}

void IdentityPanel::Wipe() {
    for (auto* secret : {&newPassphrase_, &newConfirm_, &backupPassphrase_, &backupConfirm_, &restorePassphrase_}) WipeText(*secret);
}

void IdentityPanel::Refresh(const ShellView&, const std::string& screen) {
    // Reading everything again also resumes a paused sign-in's polls.
    if (signIn_) signIn_->paused = false;
    IdentityRequest status; status.op = IdentityOp::Status; Queue(std::move(status));
    if (screen == DiscordConnectScreen) connectLookedUp_ = false;
    if (screen == "linked-accounts" || screen == "tournament-matches" || screen == DiscordConnectScreen || screen == SiteConnectScreen) {
        IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
    }
    if (screen == "tournament-matches") wantAssignments_ = true;
}

void IdentityPanel::SelectBridge(const ShellView& v, const std::string& id) {
    bridge_ = id;
    const auto* bridge = FindBridge(v, id);
    if (!bridge) return;
    // The service's connections come from its profile; its links from the list.
    IdentityRequest inspect; inspect.op = IdentityOp::BridgeInspect; inspect.origin = bridge->origin; Queue(std::move(inspect));
    IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = id; Queue(std::move(list));
}

void IdentityPanel::Update(const ShellView& v, const std::string& screen, const Submit& submit, double now) {
    now_ = now;
    announced_.Save(now);
    const bool owned = Owns(screen);
    if (!owned && onScreens_) Wipe();
    const bool entered = (owned || screen == AccountScreen) && screen != lastScreen_;
    // Leaving the Connect screen while it sets up stops it: nothing opens a
    // browser page behind the player's back.
    if (Starting() && AtScreenOf(signIn_->target, lastScreen_) && !AtScreenOf(signIn_->target, screen)) CancelAttempt();
    onScreens_ = owned; lastScreen_ = screen;
    if (screen != "public-rooms") setup_.Unseen();
    if (entered) Refresh(v, screen);
    Collect(v, now);
    // Away from these screens, Home learns the state, then the services.
    if (!owned && !sent_ && queue_.empty() && now >= backgroundAt_) {
        if (!v.identity.known) { IdentityRequest status; status.op = IdentityOp::Status; Queue(std::move(status)); }
        else if (!servicesKnown_ && v.identity.state == "ready") { IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list)); }
    }
    SendTournament(v, screen, submit);
    // While a site's page is open in the browser, the account is read again
    // every few seconds, so the screen shows it connected by itself.
    SignInPoll(screen, now);
    Send(v, submit, now);
    // Whatever dropped the setup's next request (a full queue, hiding Ember) must not leave it waiting for good.
    if (setup_.Running() && !SetupPending()) setup_.Fail(loc::T("identity.failure.timeout"));
}

void IdentityPanel::Hidden(const ShellView& v, const Submit& submit, double now) {
    now_ = now;
    Collect(v, now);
    // Only a cancelled sign-in's cleanup goes out while Ember is hidden.
    queue_.clear();
    Send(v, submit, now);
}

// The answer to the request in flight, or its timeout, for its owner. An
// answer from a replaced journey or setup run is retired without touching the
// new one's status or requests.
void IdentityPanel::Collect(const ShellView& v, double now) {
    if (sent_ && Answered(v)) {
        if (!Superseded()) {
            finishing_ = true;
            if (sentOwner_ == Owner::PublicSetup) SetupFinish(v); else Finish(v);
            finishing_ = false;
        }
        sent_ = 0;
    }
    else if (sent_ && now - sentAt_ > AnswerSeconds) {
        const bool current = !Superseded();
        if (current && sentOwner_ == Owner::PublicSetup) setup_.Fail(loc::T("identity.failure.timeout"));
        else if (current && QuietRead()) SiteReadFailed();
        else if (current && !IsCleanup(sentOp_)) {
            if (Owns(lastScreen_) || !Retirable(sentOp_)) Say(loc::T("identity.failure.timeout"), true);
            ReadFailed(); DropQueue();
        }
        sent_ = 0;
    }
}

void IdentityPanel::Send(const ShellView& v, const Submit& submit, double now) {
    if (sent_) return;
    // Ending a cancelled sign-in on its service goes first, whatever cleared
    // the queue since, so it also comes before a new Connect.
    if (!cleanup_.empty()) {
        queue_.push_front(Queued{std::move(cleanup_.front()), false, journey_, {}, {}, Owner::Screens});
        cleanup_.pop_front();
    }
    if (queue_.empty()) return;
    ShellAction action;
    action.command.generation = v.session.generation;
    sentLookUp_ = queue_.front().lookUp; sentJourney_ = queue_.front().journey; sentAccount_ = queue_.front().account;
    sentOwner_ = queue_.front().owner; sentReads_ = queue_.front().reads;
    action.identity = std::move(queue_.front().request); queue_.pop_front();
    action.identity.ticket = ++nextTicket_;
    const auto ticket = action.identity.ticket;
    sentOp_ = action.identity.op; sentBridge_ = action.identity.bridge; sentOrigin_ = action.identity.origin; sentSite_ = action.identity.site;
    if (submit(std::move(action))) { sent_ = ticket; sentAt_ = now; }
    else if (sentOwner_ == Owner::PublicSetup) setup_.Fail(loc::T("error.queue_failed"));
    else if (QuietRead()) SiteReadFailed();
    else if (!IsCleanup(sentOp_)) { Say(loc::T("error.queue_failed"), true); ReadFailed(); DropQueue(); }
}

MenuEntry IdentityPanel::ServiceRow(const ShellView& v, const netplay::IdentityBridge& bridge) const {
    auto service = Row("id-bridge", loc::T("identity.service"), ServiceDetail(bridge));
    service.value = bridge.name.empty() ? bridge.origin : bridge.name; service.userText = true;
    for (const auto& choice : v.identity.bridges)
        if (CountsAsService(choice) || choice.id == bridge_)
            service.choices.push_back({choice.id, choice.name.empty() ? choice.origin : choice.name, choice.origin});
    service.chosen = bridge_;
    if (service.choices.size() < 2) service.info = true;
    return service;
}

// Discord on the selected service: connect it where sign-in is offered
// (Ember opens Discord's page in the browser) or, once connected, disconnect
// it, also where sign-in is off now. Optional for the player.
std::optional<MenuEntry> IdentityPanel::DiscordRow(const ShellView& v, bool busy) const {
    const auto& id = v.identity;
    const auto* account = FindAccount(SignInTarget::Discord(), bridge_);
    const bool read = account && account->read;
    if (read && !account->user.empty()) {
        auto row = ConfirmRow("id-discord-remove", loc::T("identity.discord"), loc::T("identity.discord_remove_detail"), !busy);
        row.value = account->name; row.userText = true; row.hint = loc::T("identity.unlink");
        return row;
    }
    if (!id.inspectedDiscord) return std::nullopt;
    auto row = Row("id-discord-connect", loc::T("identity.discord"), loc::T("identity.discord_detail"), !busy && read);
    row.value = loc::T("identity.discord_none");
    return row;
}

void IdentityPanel::Finish(const ShellView& v) {
    const auto& id = v.identity;
    // Ending a sign-in on the service is tidying up after Cancel, which the
    // player already saw: a service that cannot does not hold anything back.
    if (IsCleanup(sentOp_)) return;
    // A site's account that only its own screen talks about fails quietly
    // elsewhere: its row shows no value, and nothing else stops.
    if ((!v.identityRequest || !id.ok) && QuietRead()) { SiteReadFailed(); return; }
    if (!v.identityRequest) {
        const std::string refusal = RefusalText(v);
        if (Owns(lastScreen_) || !Retirable(sentOp_)) Say(refusal, true);
        ReadFailed(); DropQueue(); return;
    }
    if (!id.ok) {
        // A link list on a service the helper no longer trusts just ends the selection.
        if (sentOp_ == IdentityOp::LinkList && id.failure == "bridge_not_approved") { bridge_.clear(); listedBridge_.clear(); return; }
        // The site approved or refused the request before the cancellation
        // arrived: say so and show what it decided.
        if (sentOp_ == IdentityOp::LinkCancel && id.failure == "link_conflict") {
            Say(loc::T("identity.failure.already_answered"), true);
            DropQueue();
            IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = bridge_; Queue(std::move(list));
            return;
        }
        ReadFailed();
        SiteRefused(id.failure);
        // A background read on Home fails quietly; it is tried again.
        if (Owns(lastScreen_) || !Retirable(sentOp_)) Say(SiteFailureText(id.failure).value_or(FailureText(id.failure)), true);
        DropQueue(); return;
    }
    switch (sentOp_) {
    case IdentityOp::Status:
        // A ready Ember ID screen shows the Discord and sign-in site accounts on
        // Ember's own service, and Settings > Discord the one on the selected service;
        // without an ID nothing holds Create back.
        if ((lastScreen_ == "identity" || lastScreen_ == AccountScreen) && id.state == "ready") {
            IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
        }
        // A running Connect creates the Ember ID itself when nothing must be typed.
        // A passphrase (Wine and Proton) waits for the player's Connect.
        if (signIn_ && signIn_->phase == SignInAttempt::Phase::Steps && lastScreen_ == DiscordConnectScreen && id.state == "disabled") {
            if (id.passphraseRequired || !v.canEditPreferences) signIn_.reset();
            else { IdentityRequest r; r.op = IdentityOp::Enable; Queue(std::move(r)); }
        }
        break;
    case IdentityOp::Enable: case IdentityOp::Unlock:
        if (sentOp_ == IdentityOp::Enable) { WipeText(newPassphrase_); WipeText(newConfirm_); }
        Say(loc::T(sentOp_ == IdentityOp::Enable ? "identity.done.enabled" : "identity.done.unlocked"), false);
        // Connect Discord goes on to the next step with the ID ready.
        if (lastScreen_ == DiscordConnectScreen) { IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list)); }
        break;
    case IdentityOp::Export:
        WipeText(backupPassphrase_); WipeText(backupConfirm_);
        Say(loc::Tf("identity.done.exported", id.exportPath), false, 10); break;
    case IdentityOp::PreviewImport: previewPath_ = restorePath_; break;
    case IdentityOp::Import:
        WipeText(restorePassphrase_); WipeText(newPassphrase_); WipeText(newConfirm_); previewPath_.clear();
        Say(loc::T("identity.done.imported"), false); break;
    case IdentityOp::Reset: previewPath_.clear(); Say(loc::T("identity.done.reset"), false); break;
    case IdentityOp::BridgeList:
        // Without an Ember ID the matches screen already says to create one first.
        if (opened_ && !FindBridge(v, opened_->bridge)) {
            if (id.state == "ready") Say(loc::T("tournament.failure.link_service"), true, 12);
            opened_.reset();
        }
        servicesKnown_ = true;
        if (lastScreen_ == DiscordConnectScreen) { ConnectNext(v); break; }
        // Away from these screens only the service is chosen: Ember's own, or the first.
        if (!Owns(lastScreen_)) {
            if (!FindBridge(v, bridge_)) {
                const auto* ember = EmberBridge(v);
                const auto* first = FirstService(v);
                bridge_ = ember ? ember->id : first ? first->id : std::string();
            }
            // Settings > Discord shows the Discord account on the one selected.
            if (const auto* selected = FindBridge(v, bridge_); selected && lastScreen_ == AccountScreen && id.state == "ready")
                ReadAccount(*selected, AccountReads::Discord());
            break;
        }
        // The Ember ID screen shows the Discord and sign-in site accounts on
        // Ember's own service, and a site's Connect screen that site's: its
        // profile, whose answer asks for the accounts.
        if (lastScreen_ == "identity" || lastScreen_ == SiteConnectScreen) {
            const auto reads = lastScreen_ == SiteConnectScreen ? AccountReads::OneSite(siteScreen_) : AccountReads::Everything();
            if (const auto* ember = EmberBridge(v); ember && id.state == "ready") ReadAccount(*ember, reads);
            break;
        }
        if (!FindBridge(v, bridge_)) {
            const auto* first = FirstService(v);
            bridge_ = first ? first->id : std::string();
        }
        if (!bridge_.empty()) SelectBridge(v, bridge_);
        break;
    case IdentityOp::BridgeInspect:
        if (sentLookUp_) found_ = id.inspected;
        // A running Connect trusts Ember's own service, unless the player removed it.
        if (sentLookUp_ && signIn_ && signIn_->phase == SignInAttempt::Phase::Steps && lastScreen_ == DiscordConnectScreen) {
            if (EmberOffered() && !Removed(v, found_.id)) {
                IdentityRequest r; r.op = IdentityOp::BridgeApprove; r.origin = found_.origin; r.bridge = found_.id; Queue(std::move(r));
            } else {
                // Not Ember's own, or removed by the player: theirs to trust.
                signIn_.reset();
            }
        }
        // Only an account to read: the selection stays.
        if (!sentAccount_.empty()) {
            if (id.inspected.id == sentAccount_) ReadSiteAccounts(id, sentAccount_, sentReads_);
            break;
        }
        // The selected service's profile, or a look-up of it once trusted.
        if (id.inspected.id == bridge_ && FindBridge(v, bridge_)) {
            const bool known = std::any_of(id.connections.begin(), id.connections.end(),
                [&](const netplay::IdentityConnection& c) { return c.id == connection_; });
            if (!known) connection_ = id.connections.empty() ? std::string() : id.connections.front().id;
            ReadSiteAccounts(id, bridge_, AccountReads::Discord());
        }
        break;
    case IdentityOp::DiscordStatus:
        SignInFinish(v);
        // Connect Discord's steps go on from the account read.
        ConnectAccountRead(v);
        break;
    case IdentityOp::DiscordConnect: case IdentityOp::DiscordRemove:
    case IdentityOp::SiteStatus: case IdentityOp::SiteConnect: case IdentityOp::SiteRemove:
        SignInFinish(v);
        break;
    case IdentityOp::BridgeApprove: {
        Say(loc::Tf("identity.done.trusted", found_.name), false);
        bridge_ = found_.id; found_ = {}; WipeText(origin_);
        IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
        break;
    }
    case IdentityOp::BridgeForget: {
        Say(loc::T("identity.done.forgotten"), false); bridge_.clear(); listedBridge_.clear();
        IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
        break;
    }
    case IdentityOp::LinkList: listedBridge_ = sentBridge_; break;
    case IdentityOp::LinkClaim: {
        code_.clear();
        Say(loc::Tf("identity.done.claimed", id.claimFingerprint), false, 20);
        IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = bridge_; Queue(std::move(list));
        break;
    }
    case IdentityOp::LinkCancel: case IdentityOp::LinkRemove: {
        Say(loc::T(sentOp_ == IdentityOp::LinkRemove ? "identity.done.unlinked" : "identity.done.cancelled"), false);
        IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = bridge_; Queue(std::move(list));
        break;
    }
    default: break;
    }
    ReadIfFirstUse(v, sentBridge_);
}

void IdentityPanel::ReadIfFirstUse(const ShellView& v, const std::string& bridge) {
    if (const auto* used = FindBridge(v, bridge); used && !CountsAsService(*used)) {
        IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
    }
}

bool IdentityPanel::Status(std::string& status, Tone& tone, double now) const {
    if (!message_.empty() && now < messageUntil_) { status = message_; tone = messageError_ ? Tone::Error : Tone::Success; return true; }
    // The account is read again while the player approves in the browser.
    if (signIn_ && WaitingOn(signIn_->target, signIn_->bridge) && ShowsWait(lastScreen_) && queue_.empty() &&
        (!sent_ || sentOp_ == netplay::SignInRequest(signIn_->target.source, SignInStep::Status))) {
        status = Word(signIn_->target, Site(signIn_->target.source).words.waiting); tone = Tone::Pending; return true;
    }
    if (sent_ || !queue_.empty()) {
        const bool slow = sentOp_ == IdentityOp::Enable || sentOp_ == IdentityOp::Unlock || sentOp_ == IdentityOp::Export ||
            sentOp_ == IdentityOp::PreviewImport || sentOp_ == IdentityOp::Import;
        status = loc::T(slow ? "identity.working_key" : "identity.working"); tone = Tone::Pending; return true;
    }
    if (lastScreen_ == "tournament-matches" && loadingAssignments_) { status = loc::T("tournament.loading"); tone = Tone::Pending; return true; }
    return false;
}

std::vector<MenuEntry> IdentityPanel::Rows(const ShellView& v, const std::string& screen, std::string& title) const {
    std::vector<MenuEntry> rows;
    const auto& id = v.identity;
    const bool busy = sent_ != 0 || !queue_.empty();
    const bool ready = id.state == "ready";
    const bool canEdit = v.canEditPreferences;
    const auto local = [&](std::vector<MenuEntry>& out) {
        // Under Wine and Proton the key on this PC needs a passphrase of its own.
        out.push_back(Secret("id-new-passphrase", loc::T("identity.local_passphrase"), newPassphrase_,
            loc::T("identity.local_passphrase_detail"), true));
        out.push_back(Secret("id-new-confirm", loc::T("identity.repeat_passphrase"), newConfirm_,
            PairDetail(newPassphrase_, newConfirm_, "identity.repeat_passphrase_detail"), true));
    };
    if (screen == "identity") {
        title = loc::T("identity.title");
        std::string value, detail;
        if (!id.known) { value = loc::T("identity.state.checking"); detail = loc::T("identity.state.checking_detail"); }
        else if (id.state == "disabled") { value = loc::T("common.off"); detail = loc::T("identity.state.disabled_detail"); }
        else if (id.state == "creating") { value = loc::T("identity.state.creating"); detail = loc::T("identity.state.creating_detail"); }
        else if (id.state == "locked") { value = loc::T("identity.state.locked"); detail = loc::T("identity.state.locked_detail"); }
        else if (ready) { value = id.fingerprint; detail = loc::Tf("identity.state.ready_detail", id.emberId, id.fingerprint); }
        else if (id.state == "recovery_required") { value = loc::T("identity.state.recovery"); detail = loc::T("identity.state.recovery_detail"); }
        else { value = loc::T("identity.state.unavailable"); detail = loc::Tf("identity.state.unavailable_detail", FailureText(id.reason)); }
        rows.push_back(InfoRow("id-status", loc::T("identity.status"), value, detail));
        if (id.state == "disabled") {
            // Most players come for Discord: Connect Discord creates the ID on the way.
            auto discord = Row(DiscordConnectScreen, loc::T("identity.discord"), loc::T("identity.connect_discord_detail"));
            discord.value = loc::T("identity.discord_none"); rows.push_back(std::move(discord));
            if (id.passphraseRequired) local(rows);
            const bool secretOk = !id.passphraseRequired || Matching(newPassphrase_, newConfirm_);
            rows.push_back(ConfirmRow("id-enable", loc::T("identity.enable"),
                canEdit ? loc::T("identity.enable_detail") : loc::T("identity.leave_room"), canEdit && !busy && secretOk));
        }
        if (id.state == "locked") {
            rows.push_back(Secret("id-unlock", loc::T("identity.unlock"), {}, loc::T("identity.unlock_detail"), true));
            rows.back().hint = loc::T("identity.unlock");
        }
        if (ready) {
            // What players come here for first: their matches, then Discord and the sign-in sites.
            rows.push_back(Row("tournament-matches", loc::T("screen.tournament_matches"), loc::T("identity.matches_detail")));
            rows.push_back(DiscordIdentityRow(v));
            ListedSiteIdentityRows(v, rows);
            rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
            rows.push_back(Row("id-copy", loc::T("identity.copy"), loc::T("identity.copy_detail")));
        }
        rows.push_back(Row("identity-backup", loc::T("screen.identity_backup"),
            ready ? loc::T("identity.backup_detail") : loc::T("identity.restore_detail"), id.known && id.state != "creating"));
        if (id.state == "locked" || id.state == "unavailable" || id.state == "recovery_required")
            rows.push_back(ConfirmRow("id-reset", loc::T("identity.reset"),
                canEdit ? loc::T("identity.reset_detail") : loc::T("identity.leave_room"), canEdit && !busy));
    } else if (screen == "identity-backup") {
        title = loc::T("identity.backup_title");
        if (ready) {
            rows.push_back(Secret("id-backup-passphrase", loc::T("identity.backup_passphrase"), backupPassphrase_,
                loc::T("identity.backup_passphrase_detail"), true));
            rows.push_back(Secret("id-backup-confirm", loc::T("identity.repeat_passphrase"), backupConfirm_,
                PairDetail(backupPassphrase_, backupConfirm_, "identity.repeat_passphrase_detail"), true));
            rows.push_back(ConfirmRow("id-export", loc::T("identity.export"),
                PairDetail(backupPassphrase_, backupConfirm_, "identity.export_detail"), !busy && Matching(backupPassphrase_, backupConfirm_)));
            if (!id.exportPath.empty()) rows.push_back(InfoRow("id-export-result", loc::T("identity.export_saved"), {}, id.exportPath));
        }
        rows.push_back(Row("id-restore-paste", loc::T("identity.paste_path"), loc::T("identity.paste_path_detail")));
        rows.back().hint = loc::T("menu.hint.paste");
        auto path = TextRow("id-restore-path", loc::T("identity.backup_file"), restorePath_, 1024);
        path.detail = loc::T("identity.backup_file_detail");
        rows.push_back(std::move(path));
        rows.push_back(Secret("id-restore-passphrase", loc::T("identity.backup_passphrase"), restorePassphrase_,
            loc::T("identity.restore_passphrase_detail"), true));
        rows.push_back(Row("id-preview", loc::T("identity.check_backup"), loc::T("identity.check_backup_detail"),
            !busy && !restorePath_.empty() && !restorePassphrase_.empty()));
        if (!previewPath_.empty() && previewPath_ == restorePath_ && !id.previewEmberId.empty()) {
            // A locked ID restored from its own backup gets a new passphrase on
            // this PC, which is how a forgotten one is recovered.
            const bool alreadyHere = id.previewSame && ready;
            std::string holds = loc::Tf("identity.backup_holds_detail", id.previewEmberId);
            holds += "\n\n";
            holds += alreadyHere ? loc::T("identity.backup_same") : id.previewSame ? loc::T("identity.backup_same_locked") :
                id.previewReplaces ? loc::T("identity.backup_replaces") : loc::T("identity.backup_new");
            rows.push_back(InfoRow("id-preview-result", loc::T("identity.backup_holds"), id.previewFingerprint, holds));
            if (!alreadyHere) {
                if (id.passphraseRequired) local(rows);
                const bool secretOk = !id.passphraseRequired || Matching(newPassphrase_, newConfirm_);
                rows.push_back(ConfirmRow("id-import", loc::T("identity.import"),
                    !canEdit ? loc::T("identity.leave_room") : id.previewReplaces ? loc::T("identity.import_replaces_detail") : loc::T("identity.import_detail"),
                    canEdit && !busy && secretOk && !restorePassphrase_.empty()));
            }
        }
    } else if (screen == "linked-accounts") {
        title = loc::T("identity.linked_title");
        if (!ready) {
            rows.push_back(InfoRow("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
            return rows;
        }
        if (const auto* bridge = FindBridge(v, bridge_)) {
            rows.push_back(ServiceRow(v, *bridge));
            if (listedBridge_ == bridge_) {
                for (const auto& link : id.links) {
                    auto row = ConfirmRow("id-unlink:" + link.id, link.account.empty() ? link.provider : link.provider + ": " + link.account,
                        loc::T("identity.unlink_detail"), !busy);
                    row.userText = true; row.hint = loc::T("identity.unlink"); rows.push_back(std::move(row));
                }
                for (const auto& claim : id.pending) {
                    auto row = ConfirmRow("id-cancel:" + claim.id, loc::Tf("identity.pending_label", claim.provider),
                        loc::T("identity.pending_detail"), !busy);
                    // The provider's name comes from the service and may be elided.
                    row.userText = true; row.hint = loc::T("identity.cancel_claim"); rows.push_back(std::move(row));
                }
                if (id.links.empty() && id.pending.empty())
                    rows.push_back(InfoRow("id-no-links", loc::T("identity.no_links"), {}, loc::T("identity.no_links_detail")));
            }
            if (id.inspected.id == bridge_ && (id.inspectedDiscord || id.inspectedDiscordAccounts))
                if (auto discord = DiscordRow(v, busy)) rows.push_back(std::move(*discord));
            if (id.inspected.id == bridge_ && id.connections.size() > 1) {
                auto connection = Row("id-connection", loc::T("identity.site"), loc::T("identity.site_detail"));
                for (const auto& choice : id.connections) {
                    connection.choices.push_back({choice.id, choice.name.empty() ? choice.id : choice.name, {}});
                    if (choice.id == connection_) connection.value = connection.choices.back().label;
                }
                connection.chosen = connection_; connection.userText = true;
                rows.push_back(std::move(connection));
            }
            auto code = TextRow("id-code", loc::T("identity.link_code"), code_, 16);
            code.detail = loc::T("identity.link_code_detail");
            rows.push_back(std::move(code));
            rows.push_back(Row("id-claim", loc::T("identity.link_account"), loc::T("identity.link_account_detail"),
                !busy && !code_.empty() && !connection_.empty()));
            rows.push_back(Row("id-refresh", loc::T("identity.refresh"), loc::T("identity.refresh_detail"), !busy));
            rows.push_back(ConfirmRow("id-forget", loc::T("identity.forget"), loc::T("identity.forget_detail"), !busy));
        }
        auto address = TextRow("id-origin", loc::T("identity.service_address"), origin_, 256);
        address.detail = loc::T("identity.service_address_detail");
        rows.push_back(std::move(address));
        rows.push_back(Row("id-lookup", loc::T("identity.look_up"), loc::T("identity.look_up_detail"), !busy && !origin_.empty()));
        if (!found_.id.empty() && !FindBridge(v, found_.id)) {
            auto trust = ConfirmRow("id-approve", loc::Tf("identity.trust", found_.name.empty() ? found_.origin : found_.name),
                loc::Tf("identity.trust_detail", found_.origin), !busy);
            trust.userText = true; rows.push_back(std::move(trust));
        }
    } else if (screen == DiscordConnectScreen) {
        const auto& discord = Site(SignInSource::Discord);
        title = loc::T("screen.connect_discord");
        // Setting up shows while its work is out; a step that needs the
        // player (unlock, recovery, a passphrase) shows its own row instead.
        const bool working = Starting() && busy;
        if (!id.known && !busy) {
            // The first read of the state failed: nothing is out to wait for.
            rows.push_back(Row(discord.rows.retry, loc::T("connect.retry"), loc::T("connect.retry_detail")));
        } else if (!id.known || working && !ready) {
            rows.push_back(InfoRow(discord.rows.progress, loc::T("connect.setting_up"), {}, loc::T("connect.setting_up_detail")));
        } else if (id.state == "disabled") {
            // Connect creates the Ember ID first; only Wine and Proton need a passphrase for it.
            if (id.passphraseRequired) local(rows);
            const bool secretOk = !id.passphraseRequired || Matching(newPassphrase_, newConfirm_);
            rows.push_back(Row("dc-go", loc::T("screen.connect_discord"),
                canEdit ? loc::T("connect.go_detail") : loc::T("identity.leave_room"), canEdit && !busy && secretOk));
        } else if (id.state == "locked") {
            // Unlocked here, so the journey goes on with its service.
            rows.push_back(Secret("id-unlock", loc::T("identity.unlock"), {}, loc::T("identity.unlock_detail"), true));
            rows.back().hint = loc::T("identity.unlock");
        } else if (!ready) {
            // Being created or needing recovery: the Ember ID screen says what to do.
            rows.push_back(Row("identity", loc::T("screen.identity"), loc::T("connect.identity_detail")));
        } else {
            ConnectRows(v, rows, busy);
        }
        // Every running attempt can be stopped, whatever its step.
        if (ActiveFor(SignInTarget::Discord())) rows.push_back(Row(discord.rows.cancel, loc::T("common.cancel"), loc::T("connect.cancel_detail")));
        // Where Open in Ember cannot open the game, the page's link is pasted.
        rows.push_back(Row("dc-paste", loc::T("connect.paste"), loc::T("connect.paste_detail")));
        rows.back().hint = loc::T("menu.hint.paste");
    } else if (screen == SiteConnectScreen) {
        const auto site = SignInTarget::Listed(siteScreen_);
        title = Word(site, Site(SignInSource::Site).words.title);
        ListedSiteRows(v, rows, busy);
    } else if (screen == "tournament-matches") {
        title = loc::T("screen.tournament_matches");
        MatchesRows(v, rows);
    }
    return rows;
}

void IdentityPanel::Activate(const MenuAction& a, const ShellView& v, MenuNavigation& nav) {
    const auto& id = v.identity;
    IdentityRequest r;
    // Connect Discord opened from the menu is for Ember's own service, unless
    // the player came to the Ember ID screen from a journey: it goes on.
    if (a.id == DiscordConnectScreen && nav.Parent() == DiscordConnectScreen) { nav.Return(); return; }
    if (a.id == DiscordConnectScreen) { NewJourney(); connectBridge_.clear(); }
    if (a.id == "identity" || a.id == "linked-accounts" || a.id == "identity-backup" || a.id == "tournament-matches" ||
        a.id == DiscordConnectScreen) { nav.Push(a.id); return; }
    if (a.id.compare(0, std::strlen(SiteRowPrefix), SiteRowPrefix) == 0) { siteScreen_ = a.id.substr(std::strlen(SiteRowPrefix)); nav.Push(SiteConnectScreen); return; }
    if (ConnectActivate(a, v) || SignInActivate(a, v)) return;
    if (MatchesActivate(a, v)) return;
    if (a.id == "id-copy") {
        if (!id.emberId.empty()) { ImGui::SetClipboardText(id.emberId.c_str()); Say(loc::T("identity.done.copied"), false, 3); }
        return;
    }
    if (a.id == "id-restore-paste") {
        const char* text = ImGui::GetClipboardText();
        std::string path = text ? text : "";
        // Explorer's "Copy as path" wraps the path in quotes.
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
        if (path.empty() || path.size() > IdentityRequest::MaxPath) Say(loc::T("identity.failure.paste"), true);
        else restorePath_ = path;
        return;
    }
    if (a.id == "id-enable") {
        r.op = IdentityOp::Enable;
        if (id.passphraseRequired) r.passphrase = newPassphrase_;
    } else if (a.id == "id-reset") { r.op = IdentityOp::Reset; r.emberId = id.emberId; }
    else if (a.id == "id-export") { r.op = IdentityOp::Export; r.passphrase = backupPassphrase_; }
    else if (a.id == "id-preview") { r.op = IdentityOp::PreviewImport; r.path = restorePath_; r.passphrase = restorePassphrase_; }
    else if (a.id == "id-import") {
        r.op = IdentityOp::Import; r.path = restorePath_; r.passphrase = restorePassphrase_;
        r.emberId = id.previewEmberId; r.replace = id.previewReplaces;
        if (id.passphraseRequired) r.localPassphrase = newPassphrase_;
    } else if (a.id == "id-claim") { r.op = IdentityOp::LinkClaim; r.bridge = bridge_; r.connection = connection_; r.code = code_; }
    else if (a.id == "id-refresh") { Refresh(v, "linked-accounts"); return; }
    else if (a.id == "id-forget") { r.op = IdentityOp::BridgeForget; r.bridge = bridge_; }
    else if (a.id == "id-lookup") {
        r.op = IdentityOp::BridgeInspect; r.origin = origin_; found_ = {};
        message_.clear(); Queue(std::move(r), true); return;
    }
    else if (a.id == "id-approve") { r.op = IdentityOp::BridgeApprove; r.origin = found_.origin; r.bridge = found_.id; }
    else if (a.id.compare(0, 10, "id-unlink:") == 0) { r.op = IdentityOp::LinkRemove; r.bridge = bridge_; r.target = a.id.substr(10); }
    else if (a.id.compare(0, 10, "id-cancel:") == 0) { r.op = IdentityOp::LinkCancel; r.bridge = bridge_; r.target = a.id.substr(10); }
    else return;
    message_.clear();
    Queue(std::move(r));
}

void IdentityPanel::Accept(const MenuAction& a, const ShellView& v) {
    if (a.id == "id-new-passphrase") Assign(newPassphrase_, a.text);
    else if (a.id == "id-new-confirm") Assign(newConfirm_, a.text);
    else if (a.id == "id-backup-passphrase") Assign(backupPassphrase_, a.text);
    else if (a.id == "id-backup-confirm") Assign(backupConfirm_, a.text);
    else if (a.id == "id-restore-passphrase") Assign(restorePassphrase_, a.text);
    else if (a.id == "id-restore-path") restorePath_ = a.text;
    else if (a.id == "id-origin") origin_ = a.text;
    else if (a.id == "id-code") code_ = a.text;
    else if (a.id == "id-unlock" && !a.text.empty()) {
        IdentityRequest r; r.op = IdentityOp::Unlock; r.passphrase = a.text;
        message_.clear(); Queue(std::move(r));
    } else if (a.id == "id-bridge" && a.kind == MenuAction::Chosen) { SelectBridge(v, a.text); wantAssignments_ = true; }
    else if (a.id == "id-connection" && a.kind == MenuAction::Chosen) connection_ = a.text;
}
} }
