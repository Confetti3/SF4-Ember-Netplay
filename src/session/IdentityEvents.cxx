#include "IdentityEvents.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace session {
namespace {
std::string Text(const nlohmann::json& object, const char* key, std::size_t limit = 256) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return {};
    auto value = it->get<std::string>();
    if (value.size() > limit) value.resize(limit);
    return value;
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
        for (const auto& bridge : data.value("bridges", nlohmann::json::array()))
            if (bridge.is_object() && view.bridges.size() < 16)
                view.bridges.push_back({Text(bridge, "bridge_id", 64), Text(bridge, "origin"), Text(bridge, "display_name", 64)});
    } else if (view.op == "bridge_inspect") {
        const auto profile = data.value("profile", nlohmann::json::object());
        view.inspected = {Text(profile, "bridge_id", 64), Text(profile, "origin"), Text(profile, "display_name", 64)};
        view.connections.clear();
        const auto capabilities = data.value("capabilities", nlohmann::json::object());
        for (const auto& connection : capabilities.value("connections", nlohmann::json::array()))
            if (connection.is_object() && view.connections.size() < 16)
                view.connections.push_back({Text(connection, "id", 64), Text(connection, "display_name", 64)});
        view.inspectedDiscord = false;
        for (const auto& feature : capabilities.value("features", nlohmann::json::array()))
            view.inspectedDiscord = view.inspectedDiscord || feature == "discord";
    } else if (view.op == "link_list") {
        view.links.clear(); view.pending.clear();
        for (const auto& link : data.value("links", nlohmann::json::array()))
            if (link.is_object() && view.links.size() < 32)
                view.links.push_back({Text(link, "link_id", 64), Text(link, "connection_id", 64), Text(link, "provider", 64), Text(link, "account_label", 64)});
        for (const auto& claim : data.value("pending", nlohmann::json::array()))
            if (claim.is_object() && view.pending.size() < 32)
                view.pending.push_back({Text(claim, "claim_id", 64), Text(claim, "connection_id", 64), Text(claim, "provider", 64), {}});
    } else if (view.op == "discord_status" || view.op == "discord_remove") {
        const auto account = data.value("account", nlohmann::json());
        view.discordUser = account.is_object() ? Text(account, "user_id", 20) : std::string();
        view.discordName = account.is_object() ? Text(account, "username", 64) : std::string();
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
    default: complete = false; break;
    }
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
