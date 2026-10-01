#pragma once
#include "../netplay/IdentityView.hxx"
#include <nlohmann/json_fwd.hpp>

namespace sf4e { namespace session {
// Folds one helper `tournament` event into the view. Fields the event does not
// carry keep their values, so a status-only answer leaves the bridge lists alone.
void ApplyTournamentEvent(const nlohmann::json& event, netplay::IdentityView& view);
} }
