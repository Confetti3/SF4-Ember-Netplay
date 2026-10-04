#include "PublicRoomsSetup.hxx"
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;

IdentityRequest PublicRoomsSetup::Start() {
    view_ = PublicSetupView{PublicSetupStep::Checking, {}}; ++run_; service_.clear();
    IdentityRequest status; status.op = IdentityOp::Status;
    return status;
}

std::optional<IdentityRequest> PublicRoomsSetup::Answered(const ShellView& v, IdentityOp op) {
    if (!Running()) return std::nullopt;
    const auto& id = v.identity;
    IdentityRequest next;
    switch (op) {
    case IdentityOp::Status:
        // Created when nothing must be typed, its services read when it is ready,
        // and the Ember ID screen's when the player has to unlock it or type a passphrase.
        if (id.state == "ready") { view_.step = PublicSetupStep::Finding; next.op = IdentityOp::BridgeList; }
        else if (id.state == "disabled" && !id.passphraseRequired && v.canEditPreferences) { view_.step = PublicSetupStep::Creating; next.op = IdentityOp::Enable; }
        else { view_.step = PublicSetupStep::NeedsId; return std::nullopt; }
        return next;
    case IdentityOp::Enable:
        view_.step = PublicSetupStep::Finding; next.op = IdentityOp::BridgeList;
        return next;
    case IdentityOp::BridgeList:
        // Ember's own service is trusted already (the setup is done), or it is looked
        // up to be trusted. A list that still lacks it after the trust was given means
        // the helper did not keep it.
        if (id.state != "ready") { Fail(loc::T("identity.failure.state")); return std::nullopt; }
        if (const auto* ember = FindOrigin(v, IdentityPanel::EmberService)) {
            service_ = ember->id; view_.step = PublicSetupStep::Done; return std::nullopt;
        }
        if (view_.step == PublicSetupStep::Trusting) { Failed("bridge_not_approved", {}); return std::nullopt; }
        view_.step = PublicSetupStep::Finding;
        next.op = IdentityOp::BridgeInspect; next.origin = IdentityPanel::EmberService;
        return next;
    case IdentityOp::BridgeInspect:
        // Ember's own service and nothing else: one at another address is not what the player agreed to.
        if (id.inspected.origin != IdentityPanel::EmberService || id.inspected.id.empty()) { Failed("not_a_bridge", {}); return std::nullopt; }
        view_.step = PublicSetupStep::Trusting;
        next.op = IdentityOp::BridgeApprove; next.origin = id.inspected.origin; next.bridge = id.inspected.id;
        return next;
    case IdentityOp::BridgeApprove:
        next.op = IdentityOp::BridgeList;
        return next;
    default: return std::nullopt;
    }
}

// The player typed no address, so a service that did not answer or did not
// look right is Ember's room service, said as such.
void PublicRoomsSetup::Failed(const std::string& code, std::string general) {
    static const char* const unreachable[] = {"bridge_unreachable", "service_unavailable"};
    static const char* const unusable[] = {"not_a_bridge", "invalid_origin", "unsupported_bridge", "invalid_bridge",
        "bridge_invalid_response", "bridge_response_too_large", "unsupported_version", "bridge_changed", "bridge_mismatch",
        "bridge_not_approved"};
    for (const char* c : unreachable) if (code == c) return Fail(loc::T("public.failure.unavailable"));
    for (const char* c : unusable) if (code == c) return Fail(loc::T("public.failure.rooms_unavailable"));
    Fail(std::move(general));
}

// Its words are kept on the setup, which public rooms show, because the panel's
// own status line is silent away from the Ember ID screens.
void PublicRoomsSetup::Fail(std::string text) {
    if (!Running()) return;
    view_.step = PublicSetupStep::Failed; view_.failure = std::move(text);
}
} }
