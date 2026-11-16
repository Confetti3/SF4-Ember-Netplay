// Connect Discord: the steps that take a player from a tournament site's
// link, the Discord row, or Settings > Discord's Discord account row, to a
// connected Discord account. The sign-in itself, which the listed sign-in
// sites share, is in IdentitySignIn.cxx.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include "../common/ProductIdentity.hxx"
#include "../common/TournamentLink.hxx"
#include "../netplay/AccountRoutes.hxx"
#include <imgui.h>
#include <algorithm>

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;

void IdentityPanel::OpenDiscord(const std::string& bridge, bool automatic) {
    // A link for the attempt under way, or one that stopped, shows it: no
    // second page, and no restart without Connect or Try again.
    const bool discord = signIn_ && signIn_->target.source == SignInSource::Discord;
    const bool same = bridge == connectBridge_ || (!discordService_.empty() && bridge == discordService_);
    if (discord && same) { lastScreen_.clear(); return; }
    // A link the player just clicked runs the steps by itself (which end
    // another site's sign-in); one that waited for a room asks first.
    NewJourney();
    connectBridge_ = bridge;
    if (automatic) StartSteps();
    // The screen reads everything again, as if newly opened.
    lastScreen_.clear();
}

MenuEntry IdentityPanel::DiscordAccountRow(const netplay::DiscordLink& link, const ShellView& v) const {
    auto row = Row("discord-account", loc::T("discord.account"), loc::T("discord.account_detail"));
    using State = netplay::DiscordLink::State;
    // An account read by name is shown linked by it; none says Not linked; one only known of, or unknown, says nothing.
    if (link.state == State::Linked && !link.name.empty()) {
        row.value = loc::Tf("discord.account_linked", link.name);
        row.userText = true;
        row.detail = loc::T("discord.account_linked_detail");
    } else if (link.state == State::None) row.value = loc::T("discord.account_none");
    // Friends keep their own link on an account service apart: while it is not known connected there, the row says so,
    // and its press connects it (Connect Discord connects whichever still needs it).
    if (ember::SeparateAccount() && link.state == State::Linked) {
        const auto* account = FindOrigin(v, AccountService());
        if (!account || !ConnectedOn(account->id)) row.detail = loc::T("discord.account_friends_missing");
    }
    return row;
}

void IdentityPanel::OpenDiscordAccount(const ShellView& v, MenuNavigation& navigation) {
    // A product with an account service apart connects both, as Connect Discord from the menu does (no service named).
    OpenDiscord(ember::SeparateAccount() ? std::string() : UsableBridge(v));
    navigation.Push(DiscordConnectScreen);
}

const netplay::IdentityBridge* IdentityPanel::EmberBridge(const ShellView& v) const { return FindOrigin(v, EmberService); }

const netplay::IdentityBridge* IdentityPanel::ConnectTarget(const ShellView& v) const {
    return connectBridge_.empty() ? FindOrigin(v, ConnectOrigin(v)) : FindBridge(v, connectBridge_);
}

bool IdentityPanel::ConnectedOn(const std::string& bridge) const {
    const auto* account = FindAccount(SignInTarget::Discord(), bridge);
    return account && account->read && !account->failed && !account->user.empty();
}

std::string IdentityPanel::ConnectOrigin(const ShellView& v) const {
    if (!ember::SeparateAccount()) return EmberService;
    const char* const origins[] = {EmberService, AccountService()};
    netplay::ServiceLink links[2];
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* bridge = FindOrigin(v, origins[i]);
        links[i] = {bridge != nullptr, bridge && ConnectedOn(bridge->id)};
    }
    const auto next = netplay::NextToConnect(links, 2);
    return next < 2 ? origins[next] : EmberService;
}

void IdentityPanel::ConnectOnward(const ShellView& v) {
    if (!ember::SeparateAccount() || !connectBridge_.empty() || lastScreen_ != DiscordConnectScreen) return;
    const auto* next = FindOrigin(v, ConnectOrigin(v));
    if (next && ConnectedOn(next->id)) return;
    // The next service: trusted by the look-up the steps make, then Discord's page for it.
    StartSteps();
    connectLookedUp_ = false;
    Say(loc::T("connect.next_service"), Tone::Success, 30);
    ConnectNext(v);
}

// Connect Discord asks only what its steps need: its trusted service's
// profile, whose answer asks for the Discord account, or the service it works
// on (ConnectOrigin) looked up once for the player to trust.
void IdentityPanel::ConnectNext(const ShellView& v) {
    if (v.identity.state != "ready") return;
    if (const auto* target = ConnectTarget(v)) {
        bridge_ = target->id;
        IdentityRequest inspect; inspect.op = IdentityOp::BridgeInspect; inspect.origin = target->origin; Queue(std::move(inspect));
    } else if (!connectLookedUp_) {
        connectLookedUp_ = true; found_ = {};
        lookOrigin_ = ConnectOrigin(v);
        IdentityRequest look; look.op = IdentityOp::BridgeInspect; look.origin = lookOrigin_; Queue(std::move(look), true);
    }
}

bool IdentityPanel::EmberOffered() const {
    return !found_.id.empty() && found_.origin == lookOrigin_ && (connectBridge_.empty() || found_.id == connectBridge_);
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
    // A sign-in under way ends on its service first, Discord's own wait too.
    using Phase = SignInAttempt::Phase;
    if (signIn_ && (signIn_->phase == Phase::Opening || signIn_->phase == Phase::Waiting)) CancelAttempt();
    signIn_ = SignInAttempt{SignInTarget::Discord(), {}, {}, Phase::Steps};
}

void IdentityPanel::ConnectAccountRead(const ShellView& v) {
    if (!signIn_ || signIn_->phase != SignInAttempt::Phase::Steps || lastScreen_ != DiscordConnectScreen) return;
    const auto* target = ConnectTarget(v);
    if (!target || target->id != sentBridge_) return;
    // None yet, on a service that offers sign-in: Discord's page opens. A
    // connected account is only shown.
    const auto& id = v.identity;
    if (id.discordUser.empty() && id.inspected.id == target->id && id.inspectedDiscord) OpenSignIn(SignInTarget::Discord(), sentBridge_);
    else {
        signIn_.reset();
        // Connected here already: the press goes on to a service that still needs Discord, if any.
        if (!id.discordUser.empty()) ConnectOnward(v);
    }
}

// Steps 2 and 3 of Connect Discord, with the Ember ID ready: trust the
// service, then connect Discord on it.
void IdentityPanel::ServiceRows(const ShellView& v, std::vector<MenuEntry>& rows) const {
    const auto line = [&](const char* id, std::string label, std::string detail, const char* origin) {
        auto row = InfoRow(id, std::move(label), {}, std::move(detail));
        const auto* bridge = FindOrigin(v, origin);
        const auto* account = bridge ? FindAccount(SignInTarget::Discord(), bridge->id) : nullptr;
        // Connected by name, none, or nothing while it is not trusted or not read.
        if (account && account->read && !account->failed) {
            row.value = account->user.empty() ? std::string(loc::T("identity.discord_none")) : account->name;
            row.userText = !account->user.empty();
        }
        rows.push_back(std::move(row));
    };
    line("dc-rooms", loc::T("connect.rooms_service"), loc::T("connect.rooms_service_detail"), EmberService);
    line("dc-friends", loc::T("connect.friends_service"), loc::T("connect.friends_service_detail"), AccountService());
}

void IdentityPanel::ConnectRows(const ShellView& v, std::vector<MenuEntry>& rows, bool busy) const {
    const auto& ids = Site(SignInSource::Discord).rows;
    // A product with an account service apart connects Discord on both: each one's state, then the one worked on.
    if (ember::SeparateAccount() && connectBridge_.empty()) ServiceRows(v, rows);
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
    SignInRows(v, SignInTarget::Discord(), *target, rows, busy);
}

bool IdentityPanel::ConnectActivate(const MenuAction& a, const ShellView& v) {
    if (a.id == "dc-go") { ConnectGo(v); return true; }
    // Trusting a service on Connect Discord goes on to connect it.
    if (a.id == "id-approve" && lastScreen_ == DiscordConnectScreen) StartSteps();
    // Try again restarts stopped steps or a stopped sign-in; the shared row
    // then reads everything again (SignInActivate).
    const bool stopped = signIn_ && signIn_->target.source == SignInSource::Discord && signIn_->phase == SignInAttempt::Phase::Stopped;
    if (a.id == Site(SignInSource::Discord).rows.retry && stopped) StartSteps();
    if (a.id == "dc-paste") {
        // The tournament site's link names its service; the journey starts over for it.
        const char* text = ImGui::GetClipboardText();
        const std::string bridge = tournament_link::ParseConnectPasted(text ? text : "");
        if (bridge.empty()) { Say(loc::T("connect.paste_failed"), Tone::Error); return true; }
        // Pasting is the player's press: the steps run by themselves, as for a link.
        message_.clear();
        OpenDiscord(bridge, true);
        return true;
    }
    return false;
}
} }
