#pragma once
#include <string>

namespace sf4e {
// The helper reduces a connection's selected path to "direct" or
// "relay:<code>" (or "unavailable") before it sends the route, so no address,
// port or relay URL ever reaches this process. Native code only reads that
// class and relay code back. A relay code says how this PC connects, never
// where a player is: that is the player's area (Area.hxx).
enum class RouteKind { Unknown, Direct, Relayed };

inline RouteKind ClassifyRoute(const std::string& route) {
    if (route == "direct") return RouteKind::Direct;
    if (route.rfind("relay:", 0) == 0) return RouteKind::Relayed;
    return RouteKind::Unknown;
}

// The helper's names for the relays it pins (rust/sf4-net src/invite.rs,
// NAMES), each with the catalog key of the relay's name: Ember's relay in
// Kansas is "use1" and Ember's for Europe "euc1"; n0's are "n0use1" (US
// East), "usw1" (US West), "n0euc1" (Europe) and "aps1" (Asia). This build
// uses all six.
struct RelayRow { const char* code; const char* key; };
constexpr RelayRow Relays[] = {
    {"use1", "network.relay_use1"},
    {"usw1", "network.relay_usw1"},
    {"euc1", "network.relay_euc1"},
    {"aps1", "network.relay_aps1"},
    {"n0use1", "network.relay_n0use1"},
    {"n0euc1", "network.relay_n0euc1"},
};

// The catalog key of a relay's name: a pinned relay's, "network.relay_other"
// for "other", or null for any other text (empty: no relay).
inline const char* RelayNameKey(const std::string& code) {
    for (const auto& relay : Relays) if (code == relay.code) return relay.key;
    return code == "other" ? "network.relay_other" : nullptr;
}

// Whether `code` is one of the pinned relays' names ("other" is not).
inline bool IsRelayName(const std::string& code) {
    return RelayNameKey(code) != nullptr && code != "other";
}

// Whether `code` is a region code a room member's latency route may carry
// (ember_protocol::relay::REGIONS): n0 and Ember share use1 and euc1, so a
// relay's name is not a region ("n0use1" is not one).
inline bool IsRelayRegionCode(const std::string& code) {
    return code == "use1" || code == "usw1" || code == "euc1" || code == "aps1";
}

// Only a known relay name or "other" may reach the interface and logs: a
// relay address, or any text a helper sent that is not a name, reads as
// "other".
inline std::string NormalizeRelayRegion(const std::string& code) {
    return code.empty() ? std::string() : IsRelayName(code) ? code : std::string("other");
}

// "relay:use1" reads as "use1". Any other text after "relay:", including "other",
// reads as "other"; a route that is not a relay has no relay and reads as empty.
inline std::string RelayRegion(const std::string& route) {
    if (ClassifyRoute(route) != RouteKind::Relayed) return {};
    const auto code = route.substr(std::string("relay:").size());
    return IsRelayName(code) ? code : std::string("other");
}

// Untranslated names for logs and diagnostics exports.
inline const char* RouteLabel(RouteKind kind) {
    switch (kind) {
    case RouteKind::Direct: return "Direct";
    case RouteKind::Relayed: return "Relayed";
    default: return "Unknown";
    }
}
}
