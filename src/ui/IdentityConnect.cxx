// Connect Discord: the steps that take a player from a tournament site's
// link, the Discord row, or Settings > Discord's Tournament account row, to a
// connected Discord account. The sign-in itself, which Challonge shares, is
// in IdentitySignIn.cxx.
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
    const bool discord = signIn_ && signIn_->site == SignInSite::Discord;
    const bool same = bridge == connectBridge_ || (discord && bridge == signIn_->bridge);
    if ((steps_ != Steps::None || discord) && same) { lastScreen_.clear(); return; }
    // A link the player just clicked runs the steps by itself (which end
    // another site's sign-in); one that waited for a room asks first.
    NewJourney();
    connectBridge_ = bridge;
    if (automatic) StartSteps();
    // The screen reads everything again, as if newly opened.
    lastScreen_.clear();
}

MenuEntry IdentityPanel::TournamentAccountRow(const ShellView& v) const {
    auto row = Row("tournament-account", loc::T("discord.tournament_account"), loc::T("discord.tournament_account_detail"));
    const auto& id = v.identity;
    const std::string bridge = UsableBridge(v);
    if (!bridge.empty()) ShowAccount(row, SignInSite::Discord, bridge);
    // Without a ready Ember ID, or with one and no service, no account is linked.
    else if (id.known && (id.state != "ready" || servicesKnown_)) row.value = loc::T("identity.discord_none");
    return row;
}

void IdentityPanel::OpenTournamentAccount(const ShellView& v, MenuNavigation& navigation) {
    OpenDiscord(UsableBridge(v));
    navigation.Push(DiscordConnectScreen);
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

void IdentityPanel::ConnectGo(const ShellView& v) {
    const auto& id = v.identity;
    StartSteps(); message_.clear();
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
    Refresh(v, DiscordConnectScreen);
}

void IdentityPanel::StartSteps() {
    EndOtherSignIn(SignInSite::Discord);
    signIn_.reset();
    steps_ = Steps::Running;
}

bool IdentityPanel::StepsRunFor(SignInSite site) const { return site == SignInSite::Discord && steps_ == Steps::Running; }

void IdentityPanel::ConnectAccountRead(const ShellView& v) {
    if (steps_ != Steps::Running || lastScreen_ != DiscordConnectScreen) return;
    const auto* target = ConnectTarget(v);
    if (!target || target->id != sentBridge_) return;
    // None yet, on a service that offers sign-in: Discord's page opens. A
    // connected account is only shown.
    const auto& id = v.identity;
    if (id.discordUser.empty() && id.inspected.id == target->id && id.inspectedDiscord) OpenSignIn(SignInSite::Discord, sentBridge_);
    else steps_ = Steps::None;
}

// Steps 2 and 3 of Connect Discord, with the Ember ID ready: trust the
// service, then connect Discord on it.
void IdentityPanel::ConnectRows(const ShellView& v, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& ids = Site(SignInSite::Discord).rows;
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
        } else if (busy && Starting()) {
            rows.push_back(InfoRow(ids.progress, loc::T("connect.setting_up"), {}, loc::T("connect.setting_up_detail")));
        } else if (busy) {
            rows.push_back(InfoRow("dc-looking", loc::T("connect.looking_up"), {}, loc::T("connect.looking_up_detail")));
        } else {
            rows.push_back(InfoRow("dc-unknown", loc::T("connect.unknown"), {}, loc::T("connect.unknown_detail")));
            rows.push_back(Row(ids.retry, loc::T("connect.retry"), loc::T("connect.retry_detail")));
            rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
        }
        return;
    }
    SignInRows(v, SignInSite::Discord, *target, rows, busy);
}

bool IdentityPanel::ConnectActivate(const MenuAction& a, const ShellView& v) {
    if (a.id == "dc-go") { ConnectGo(v); return true; }
    // Trusting a service on Connect Discord goes on to connect it.
    if (a.id == "id-approve" && lastScreen_ == DiscordConnectScreen) StartSteps();
    // Try again restarts stopped steps or a stopped sign-in; the shared row
    // then reads everything again (SignInActivate).
    const bool stopped = steps_ == Steps::Stopped ||
        (signIn_ && signIn_->site == SignInSite::Discord && signIn_->phase == SignInAttempt::Phase::Stopped);
    if (a.id == Site(SignInSite::Discord).rows.retry && stopped) StartSteps();
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
