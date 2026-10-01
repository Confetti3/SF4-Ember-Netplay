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

void ApplyTournamentEvent(const nlohmann::json& event, netplay::IdentityView& view) {
    // A malformed answer is ignored: it must never reach the room's own
    // failure handling.
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
    if (!view.ok || found == event.end() || !found->is_object()) return;
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
    } else if (view.op == "link_list") {
        view.links.clear(); view.pending.clear();
        for (const auto& link : data.value("links", nlohmann::json::array()))
            if (link.is_object() && view.links.size() < 32)
                view.links.push_back({Text(link, "link_id", 64), Text(link, "connection_id", 64), Text(link, "provider", 64), Text(link, "account_label", 64)});
        for (const auto& claim : data.value("pending", nlohmann::json::array()))
            if (claim.is_object() && view.pending.size() < 32)
                view.pending.push_back({Text(claim, "claim_id", 64), Text(claim, "connection_id", 64), Text(claim, "provider", 64), {}});
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
    } catch (const nlohmann::json::exception&) {}
}
} }
