#pragma once
#include "../netplay/IdentityView.hxx"
#include "../netplay/IdentityRequest.hxx"
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace sf4e { namespace session {
// Folds one helper `tournament` event into the view. Fields the event does not
// carry keep their values, so a status-only answer leaves the bridge lists alone.
void ApplyTournamentEvent(const nlohmann::json& event, netplay::IdentityView& view);
// The helper's tournament request for one interface request, or empty when a
// field it needs is missing. The text may hold a passphrase: the caller wipes it.
std::string BuildTournamentRequest(const netplay::IdentityRequest& request);
} }
