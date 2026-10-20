#include "IdentityEvents.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace session {
namespace {
// The string at `key`, kept to `limit` characters, as the helper counts
// them: a name is never cut inside a character.
std::string Text(const nlohmann::json& object, const char* key, std::size_t limit = 256) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return {};
    const auto& value = it->get_ref<const std::string&>();
    std::size_t end = 0;
    for (std::size_t characters = 0; end < value.size() && characters < limit; ++characters)
        do ++end; while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80);
    return value.substr(0, end);
}

// The account at `data.account`, its user ID kept to `idLimit` characters;
// empty when there is none, and 0 for a connection time not given as a number.
netplay::IdentityAccount AccountOf(const nlohmann::json& data, std::size_t idLimit) {
    const auto account = data.value("account", nlohmann::json());
    if (!account.is_object()) return {};
    const auto at = account.find("connected_at");
    return {Text(account, "user_id", idLimit), Text(account, "username", 64),
        at != account.end() && at->is_number_unsigned() ? at->get<std::uint64_t>() : 0};
}
}

void ApplyTournamentEvent(const nlohmann::json& event, netplay::IdentityView& live) {
    // The answer is decoded into a copy that replaces the live view only once
    // all of it has parsed. A malformed answer changes nothing, and never
    // reaches the room's own failure handling.
    netplay::IdentityView view = live;
    try {
    const auto status = event.find("identity");
    if (status != event.end() && status->is_object()) {
        view.known = true;
        view.state = Text(*status, "state", 32);
        view.reason = Text(*status, "reason", 64);
        view.emberId = Text(*status, "ember_id", 64);
        view.fingerprint = Text(*status, "fingerprint", 32);
        view.backend = Text(*status, "backend", 32);
        view.passphraseRequired = status->value("passphrase_required", false);
    }
    view.requestId = event.value("request_id", std::uint64_t(0));
    view.op = Text(event, "op", 64);
    view.ok = event.value("ok", false);
    view.failure = Text(event, "reason", 64);
    if (view.op == "identity_preview_import" || view.op == "identity_import" || view.op == "identity_reset") {
        view.previewEmberId.clear(); view.previewFingerprint.clear();
        view.previewSame = view.previewReplaces = false;
    }
    const auto found = event.find("data");
    if (!view.ok || found == event.end() || !found->is_object()) { live = std::move(view); return; }
    const auto& data = *found;
    if (view.op == "bridge_list") {
        view.bridges.clear();
        // Up to 16 the player approved, and the services built into the helper.
        for (const auto& bridge : data.value("bridges", nlohmann::json::array()))
            if (bridge.is_object() && view.bridges.size() < 24)
                view.bridges.push_back({Text(bridge, "bridge_id", 64), Text(bridge, "origin"), Text(bridge, "display_name", 64),
                    bridge.value("builtin", false), bridge.value("active", false)});
        view.removedBridges.clear();
        for (const auto& removed : data.value("removed", nlohmann::json::array()))
            if (removed.is_string() && view.removedBridges.size() < 64)
                view.removedBridges.push_back(removed.get<std::string>().substr(0, 64));
    } else if (view.op == "bridge_inspect") {
        const auto profile = data.value("profile", nlohmann::json::object());
        view.inspected = {Text(profile, "bridge_id", 64), Text(profile, "origin"), Text(profile, "display_name", 64)};
        view.connections.clear();
        const auto capabilities = data.value("capabilities", nlohmann::json::object());
        for (const auto& connection : capabilities.value("connections", nlohmann::json::array()))
            if (connection.is_object() && view.connections.size() < 16)
                view.connections.push_back({Text(connection, "id", 64), Text(connection, "display_name", 64)});
        view.inspectedDiscord = view.inspectedDiscordAccounts = false;
        for (const auto& feature : capabilities.value("features", nlohmann::json::array())) {
            view.inspectedDiscord = view.inspectedDiscord || feature == "discord";
            view.inspectedDiscordAccounts = view.inspectedDiscordAccounts || feature == "discord.accounts";
        }
        // The helper has already checked each site's ID and dropped repeats.
        view.signInSites.clear();
        const auto sites = capabilities.value("sign_in_sites", nlohmann::json::array());
        if (sites.is_array())
            for (const auto& site : sites) {
                if (view.signInSites.size() == netplay::MaxSignInSites) break;
                const auto id = site.is_object() ? Text(site, "id", 32) : std::string();
                if (id.empty()) continue;
                const auto flag = [&](const char* name) {
                    const auto it = site.find(name);
                    return it != site.end() && it->is_boolean() && it->get<bool>();
                };
                view.signInSites.push_back({id, Text(site, "display_name", 64), flag("sign_in"), flag("organizer")});
            }
    } else if (view.op == "link_list") {
        view.links.clear(); view.pending.clear();
        for (const auto& link : data.value("links", nlohmann::json::array()))
            if (link.is_object() && view.links.size() < 32)
                view.links.push_back({Text(link, "link_id", 64), Text(link, "connection_id", 64), Text(link, "provider", 64), Text(link, "account_label", 64)});
        for (const auto& claim : data.value("pending", nlohmann::json::array()))
            if (claim.is_object() && view.pending.size() < 32)
                view.pending.push_back({Text(claim, "claim_id", 64), Text(claim, "connection_id", 64), Text(claim, "provider", 64), {}});
    } else if (view.op == "discord_status" || view.op == "discord_remove") {
        const auto account = AccountOf(data, 20);
        view.discordUser = account.user; view.discordName = account.name; view.discordConnectedAt = account.connectedAt;
    } else if (data.contains("organizer")) {
        // An organizer read: the grant held or not; the account read stays.
        view.siteOrganizer = data["organizer"].is_object();
    } else if (view.op == "site_status" || view.op == "site_remove") {
        const auto account = AccountOf(data, 64);
        view.siteUser = account.user; view.siteName = account.name; view.siteConnectedAt = account.connectedAt;
    } else if (view.op == "desk_read" && data.contains("sets")) {
        view.deskEvent = Text(data, "event", 64);
        view.deskSets.clear();
        for (const auto& set : data.value("sets", nlohmann::json::array())) {
            if (!set.is_object() || view.deskSets.size() >= 160) continue;
            netplay::DeskSet d;
            d.id = Text(set, "id", 64); d.roundLabel = Text(set, "round_label", 64); d.phase = Text(set, "phase", 64);
            d.side = Text(set, "side", 16); d.state = Text(set, "desk_state", 32); d.matchId = Text(set, "match_id", 64);
            d.revision = Text(set, "revision", 32); d.posted = Text(set, "posted", 16);
            const auto players = set.value("players", nlohmann::json::array());
            for (std::size_t slot = 0; slot < 2 && slot < players.size(); ++slot)
                if (players[slot].is_string()) d.players[slot] = players[slot].get<std::string>(); // the helper keeps them to 64 characters
            const auto score = set.find("score");
            if (score != set.end() && score->is_array() && score->size() == 2 && (*score)[0].is_number_integer() && (*score)[1].is_number_integer()) {
                d.score[0] = (*score)[0].get<int>(); d.score[1] = (*score)[1].get<int>(); d.scored = true;
            }
            const auto round = set.find("round");
            if (round != set.end() && round->is_number_integer()) d.round = round->get<int>();
            d.held = set.value("held", false);
            const auto called = set.find("called_at");
            if (called != set.end() && called->is_number_unsigned()) d.calledAt = called->get<std::uint64_t>();
            for (const auto& game : set.value("games", nlohmann::json::array()))
                if (game.is_object() && d.games.size() < 16)
                    d.games.push_back({Text(game, "attempt_id", 64), Text(game, "outcome", 32), Text(game, "state", 32)});
            view.deskSets.push_back(std::move(d));
        }
    } else if (view.op == "desk_web") {
        view.webSessions.clear();
        for (const auto& session : data.value("sessions", nlohmann::json::array())) {
            if (!session.is_object() || view.webSessions.size() >= 20) continue;
            netplay::WebSession w;
            w.site = Text(session, "site", 32); w.browser = Text(session, "browser", 32);
            const auto at = [&](const char* key) {
                const auto it = session.find(key);
                return it != session.end() && it->is_number_unsigned() ? it->get<std::uint64_t>() : std::uint64_t{0};
            };
            w.startedAt = at("started_at"); w.lastUsedAt = at("last_used_at");
            w.fresh = session.value("new", false);
            view.webSessions.push_back(std::move(w));
        }
    } else if (view.op == "desk_read") {
        view.deskEvents.clear();
        for (const auto& event : data.value("events", nlohmann::json::array()))
            if (event.is_object() && view.deskEvents.size() < 32)
                view.deskEvents.push_back({Text(event, "id", 64), Text(event, "title", 64), Text(event, "status", 16), Text(event, "site", 32)});
    } else if (view.op == "link_claim") {
        view.claimFingerprint = Text(data, "fingerprint", 32);
    } else if (view.op == "identity_preview_import") {
        view.previewEmberId = Text(data, "ember_id", 64);
        view.previewFingerprint = Text(data, "fingerprint", 32);
        view.previewSame = data.value("same_identity", false);
        view.previewReplaces = data.value("replaces_existing", false);
    } else if (view.op == "identity_export") {
        view.exportPath = Text(data, "path", 1024);
    }
    } catch (const nlohmann::json::exception&) { return; }
    live = std::move(view);
}

std::string BuildTournamentRequest(const netplay::IdentityRequest& r) {
    using netplay::IdentityOp;
    nlohmann::json body = nlohmann::json::object();
    // The fields each op needs, in the helper's names; any one empty refuses it.
    const auto need = [&](std::initializer_list<std::pair<const char*, const std::string*>> fields) {
        for (const auto& field : fields) { if (field.second->empty()) return false; body[field.first] = *field.second; }
        return true;
    };
    bool complete = true;
    switch (r.op) {
    case IdentityOp::Status: body["op"] = "identity_status"; break;
    case IdentityOp::Enable:
        body["op"] = "identity_enable";
        if (!r.passphrase.empty()) body["passphrase"] = r.passphrase;
        break;
    case IdentityOp::Unlock: body["op"] = "identity_unlock"; complete = need({{"passphrase", &r.passphrase}}); break;
    case IdentityOp::Export: body["op"] = "identity_export"; complete = need({{"path", &r.path}, {"passphrase", &r.passphrase}}); break;
    case IdentityOp::PreviewImport:
        body["op"] = "identity_preview_import"; complete = need({{"path", &r.path}, {"passphrase", &r.passphrase}}); break;
    case IdentityOp::Import:
        body["op"] = "identity_import";
        complete = need({{"path", &r.path}, {"passphrase", &r.passphrase}, {"expected_ember_id", &r.emberId}});
        if (!r.localPassphrase.empty()) body["local_passphrase"] = r.localPassphrase;
        if (r.replace) body["replace"] = true;
        break;
    case IdentityOp::Reset:
        body["op"] = "identity_reset";
        if (!r.emberId.empty()) body["confirm_ember_id"] = r.emberId;
        break;
    case IdentityOp::BridgeList: body["op"] = "bridge_list"; break;
    case IdentityOp::BridgeInspect: body["op"] = "bridge_inspect"; complete = need({{"origin", &r.origin}}); break;
    case IdentityOp::BridgeApprove:
        body["op"] = "bridge_approve"; complete = need({{"origin", &r.origin}, {"bridge_id", &r.bridge}}); break;
    case IdentityOp::BridgeForget: body["op"] = "bridge_forget"; complete = need({{"bridge_id", &r.bridge}}); break;
    case IdentityOp::LinkList: body["op"] = "link_list"; complete = need({{"bridge_id", &r.bridge}}); break;
    case IdentityOp::LinkClaim:
        body["op"] = "link_claim";
        complete = need({{"bridge_id", &r.bridge}, {"connection_id", &r.connection}, {"code", &r.code}});
        break;
    case IdentityOp::LinkCancel:
        body["op"] = "link_cancel"; complete = need({{"bridge_id", &r.bridge}, {"claim_id", &r.target}}); break;
    case IdentityOp::LinkRemove:
        body["op"] = "link_remove"; complete = need({{"bridge_id", &r.bridge}, {"link_id", &r.target}}); break;
    case IdentityOp::DiscordStatus: body["op"] = "discord_status"; complete = need({{"bridge_id", &r.bridge}}); break;
    case IdentityOp::DiscordConnect: body["op"] = "discord_connect"; complete = need({{"bridge_id", &r.bridge}}); break;
    case IdentityOp::DiscordRemove: body["op"] = "discord_remove"; complete = need({{"bridge_id", &r.bridge}}); break;
    case IdentityOp::DiscordCancel: body["op"] = "discord_cancel"; complete = need({{"bridge_id", &r.bridge}}); break;
    case IdentityOp::SiteStatus: body["op"] = "site_status"; complete = need({{"bridge_id", &r.bridge}, {"site", &r.site}}); break;
    case IdentityOp::SiteConnect: body["op"] = "site_connect"; complete = need({{"bridge_id", &r.bridge}, {"site", &r.site}}); break;
    case IdentityOp::SiteRemove: body["op"] = "site_remove"; complete = need({{"bridge_id", &r.bridge}, {"site", &r.site}}); break;
    case IdentityOp::SiteCancel: body["op"] = "site_cancel"; complete = need({{"bridge_id", &r.bridge}, {"site", &r.site}}); break;
    case IdentityOp::DeskRead:
        body["op"] = "desk_read"; complete = need({{"bridge_id", &r.bridge}});
        if (!r.target.empty()) body["event"] = r.target;
        break;
    case IdentityOp::DeskOpen:
        body["op"] = "desk_handoff"; complete = need({{"bridge_id", &r.bridge}});
        break;
    case IdentityOp::DeskApprove:
        body["op"] = "desk_approve"; complete = need({{"bridge_id", &r.bridge}, {"code", &r.command}});
        break;
    case IdentityOp::DeskWeb: case IdentityOp::DeskWebSeen: case IdentityOp::DeskWebEnd:
        body["op"] = "desk_web"; complete = need({{"bridge_id", &r.bridge}});
        if (r.op == IdentityOp::DeskWebSeen) body["seen"] = true;
        if (r.op == IdentityOp::DeskWebEnd) body["end"] = true;
        break;
    case IdentityOp::DeskAct: {
        body["op"] = "desk_act"; complete = need({{"bridge_id", &r.bridge}, {"path", &r.target}});
        auto command = nlohmann::json::parse(r.command, nullptr, false);
        if (!command.is_object()) complete = false; else body["command"] = std::move(command);
        break;
    }
    default: complete = false; break;
    }
    // The organizer sign-in has no cancel: its path drops the grant instead.
    if (r.organizer && (r.op == IdentityOp::SiteStatus || r.op == IdentityOp::SiteConnect || r.op == IdentityOp::SiteRemove))
        body["organizer"] = true;
    std::string text;
    if (complete) {
        try { text = body.dump(); } catch (const nlohmann::json::exception&) { text.clear(); }
    }
    // The tree holds its own copies of any passphrase.
    for (const char* key : {"passphrase", "local_passphrase"}) {
        const auto it = body.find(key);
        if (it != body.end() && it->is_string()) WipeText(it->get_ref<std::string&>());
    }
    return text;
}
} }
