// Connect Discord: the attempt that takes a player from a tournament site's
// link, or the Discord row, to a connected Discord account. Its answers are
// handled with the rest in IdentityPanel.cxx.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include "../common/TournamentLink.hxx"
#include <imgui.h>
#include <algorithm>

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;

void IdentityPanel::OpenDiscord(const std::string& bridge, bool automatic) {
    // A link for the attempt under way, or one that stopped, shows it: no
    // second page, and no restart without Connect or Try again.
    const bool same = bridge == connectBridge_ || (!discordWaitBridge_.empty() && bridge == discordWaitBridge_);
    if (attempt_ != Attempt::None && same) { lastScreen_.clear(); return; }
    NewJourney();
    connectBridge_ = bridge;
    attempt_ = automatic ? Attempt::Setup : Attempt::None;
    // The screen reads everything again, as if newly opened.
    lastScreen_.clear();
}

const netplay::IdentityBridge* IdentityPanel::EmberBridge(const ShellView& v) const { return FindOrigin(v, EmberService); }

const netplay::IdentityBridge* IdentityPanel::ConnectTarget(const ShellView& v) const {
    return connectBridge_.empty() ? EmberBridge(v) : FindBridge(v, connectBridge_);
}

// Connect Discord asks only what its steps need: its trusted service's
// profile, whose answer asks for the Discord account, or Ember's own service
// looked up once for the player to trust.
void IdentityPanel::ConnectNext(const ShellView& v) {
    if (v.identity.state != "ready") return;
    if (const auto* target = ConnectTarget(v)) {
        bridge_ = target->id;
        IdentityRequest inspect; inspect.op = IdentityOp::BridgeInspect; inspect.origin = target->origin; Queue(std::move(inspect));
    } else if (!connectLookedUp_) {
        connectLookedUp_ = true; found_ = {};
        IdentityRequest look; look.op = IdentityOp::BridgeInspect; look.origin = EmberService; Queue(std::move(look), true);
    }
}

bool IdentityPanel::EmberOffered() const {
    return !found_.id.empty() && found_.origin == EmberService && (connectBridge_.empty() || found_.id == connectBridge_);
}

bool IdentityPanel::Removed(const ShellView& v, const std::string& bridge) const {
    const auto& removed = v.identity.removedBridges;
    return std::find(removed.begin(), removed.end(), bridge) != removed.end();
}

void IdentityPanel::OpenSignIn(const std::string& bridge) {
    const auto account = discord_.find(bridge);
    attemptBefore_ = account == discord_.end() ? std::string() : account->second.user;
    attempt_ = Attempt::Opening; discordWaitBridge_ = bridge;
    IdentityRequest r; r.op = IdentityOp::DiscordConnect; r.bridge = bridge; Queue(std::move(r));
}

void IdentityPanel::CancelAttempt() {
    // A sign-in already asked of the service ends there too, so a Discord
    // page left open connects nothing.
    const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Queued& q) {
        return q.journey == journey_ && q.request.op == IdentityOp::DiscordConnect;
    });
    const bool asked = attempt_ == Attempt::Waiting || (attempt_ == Attempt::Opening && !queued);
    // The public rooms setup's own requests are not the attempt's to stop.
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Queued& q) {
        return q.owner == Owner::Screens && q.journey == journey_ && (q.request.op == IdentityOp::Enable || q.request.op == IdentityOp::BridgeApprove ||
            q.request.op == IdentityOp::DiscordConnect);
    }), queue_.end());
    if (asked && !discordWaitBridge_.empty()) cancelBridge_ = discordWaitBridge_;
    attempt_ = Attempt::Stopped; discordWaitUntil_ = 0; discordPaused_ = false;
}

void IdentityPanel::ConnectGo(const ShellView& v) {
    const auto& id = v.identity;
    attempt_ = Attempt::Setup; message_.clear();
    if (id.state == "disabled") {
        IdentityRequest r; r.op = IdentityOp::Enable;
        if (id.passphraseRequired) r.passphrase = newPassphrase_;
        Queue(std::move(r));
        return;
    }
    if (!ConnectTarget(v) && EmberOffered()) {
        IdentityRequest r; r.op = IdentityOp::BridgeApprove; r.origin = found_.origin; r.bridge = found_.id; Queue(std::move(r));
        return;
    }
    Refresh(v, "discord-connect");
}

// Steps 2 and 3 of Connect Discord, with the Ember ID ready: trust the
// service, then connect Discord on it.
void IdentityPanel::ConnectRows(const ShellView& v, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& id = v.identity;
    const auto* target = ConnectTarget(v);
    if (!target) {
        const bool offered = EmberOffered();
        const bool removed = offered && Removed(v, found_.id);
        if (removed) {
            // Removed by the player this run: trusting it again is theirs to say.
            auto trust = ConfirmRow("id-approve", loc::Tf("identity.trust", found_.name.empty() ? found_.origin : found_.name),
                loc::Tf("connect.trust_detail", found_.origin), !busy);
            trust.userText = true; rows.push_back(std::move(trust));
        } else if (offered && !busy) {
            rows.push_back(Row("dc-go", loc::T("screen.connect_discord"), loc::T("connect.go_detail")));
        } else if (busy && (attempt_ == Attempt::Setup || attempt_ == Attempt::Opening)) {
            rows.push_back(InfoRow("dc-progress", loc::T("connect.setting_up"), {}, loc::T("connect.setting_up_detail")));
        } else if (busy) {
            rows.push_back(InfoRow("dc-looking", loc::T("connect.looking_up"), {}, loc::T("connect.looking_up_detail")));
        } else {
            rows.push_back(InfoRow("dc-unknown", loc::T("connect.unknown"), {}, loc::T("connect.unknown_detail")));
            rows.push_back(Row("dc-retry", loc::T("connect.retry"), loc::T("connect.retry_detail")));
            rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
        }
        return;
    }
    auto service = InfoRow("dc-service", loc::T("identity.service"), target->name.empty() ? target->origin : target->name, target->origin);
    service.userText = true; rows.push_back(std::move(service));
    // Read, and the latest read did not fail.
    const auto account = discord_.find(target->id);
    const bool read = bridge_ == target->id && account != discord_.end() && account->second.read && !account->second.failed;
    const bool waiting = discordWaitUntil_ > 0 && !discordPaused_ && discordWaitBridge_ == target->id;
    if (waiting) {
        rows.push_back(InfoRow("dc-waiting", loc::T("connect.waiting_label"), {}, loc::T("connect.waiting_detail")));
    } else if ((attempt_ == Attempt::Setup || attempt_ == Attempt::Opening) && busy) {
        rows.push_back(InfoRow("dc-progress", loc::T("connect.setting_up"), {}, loc::T("connect.setting_up_detail")));
    } else if (read && !account->second.user.empty()) {
        auto connected = InfoRow("dc-connected", loc::T("connect.connected"), account->second.name, loc::T("connect.connected_detail"));
        connected.userText = true; rows.push_back(std::move(connected));
        // Another account, or none: both here, not only under Linked accounts.
        rows.push_back(Row("id-discord-connect", loc::T("connect.change"), loc::T("connect.change_detail"), !busy));
        auto remove = ConfirmRow("id-discord-remove", loc::T("identity.unlink"), loc::T("identity.discord_remove_detail"), !busy);
        remove.hint = loc::T("identity.unlink"); rows.push_back(std::move(remove));
    } else if (id.inspected.id == target->id && !id.inspectedDiscord && !(account != discord_.end() && account->second.failed)) {
        // Off as the latest read said; a failed read offers Try again below.
        rows.push_back(InfoRow("dc-off", loc::T("connect.off"), {}, loc::T("connect.off_detail")));
    } else {
        rows.push_back(Row("id-discord-connect", loc::T("screen.connect_discord"), loc::T("connect.go_detail"),
            !busy && read));
        // Nothing in flight and the account not read: reading it failed.
        if (!busy && !read) rows.push_back(Row("dc-retry", loc::T("connect.retry"), loc::T("connect.retry_detail")));
    }
}

bool IdentityPanel::ConnectActivate(const MenuAction& a, const ShellView& v) {
    if (a.id == "dc-retry") {
        // Try again restarts a stopped attempt; a paused wait resumes its polls.
        if (attempt_ == Attempt::Stopped) attempt_ = Attempt::Setup;
        message_.clear(); Refresh(v, "discord-connect"); return true;
    }
    if (a.id == "dc-go") { ConnectGo(v); return true; }
    if (a.id == "dc-cancel") { CancelAttempt(); Say(loc::T("connect.cancelled"), false); return true; }
    // Trusting a service on Connect Discord goes on to connect it.
    if (a.id == "id-approve" && lastScreen_ == "discord-connect") attempt_ = Attempt::Setup;
    if (a.id == "dc-paste") {
        // The tournament site's link names its service; the journey starts over for it.
        const char* text = ImGui::GetClipboardText();
        const std::string bridge = tournament_link::ParseConnectPasted(text ? text : "");
        if (bridge.empty()) { Say(loc::T("connect.paste_failed"), true); return true; }
        // Pasting is the player's press: the steps run by themselves, as for a link.
        message_.clear();
        OpenDiscord(bridge, true);
        return true;
    }
    return false;
}
} }
