#pragma once
#include <string>

namespace sf4e {
// The helper reduces a connection's selected path to "direct" or
// "relay:<region>" (or "unavailable") before it sends the route, so no address,
// port or relay URL ever reaches this process. Native code only reads that
// class and region back.
enum class RouteKind { Unknown, Direct, Relayed };

inline RouteKind ClassifyRoute(const std::string& route) {
    if (route == "direct") return RouteKind::Direct;
    if (route.rfind("relay:", 0) == 0) return RouteKind::Relayed;
    return RouteKind::Unknown;
}

// The helper's region codes for the pinned relays, or "other".
inline bool IsRelayRegionCode(const std::string& code) {
    return code == "use1" || code == "usw1" || code == "euc1" || code == "aps1";
}

// Only a known code or "other" may reach the interface and logs: a relay
// address, or any text a helper sent that is not a code, reads as "other".
inline std::string NormalizeRelayRegion(const std::string& code) {
    return code.empty() ? std::string() : IsRelayRegionCode(code) ? code : std::string("other");
}

// "relay:use1" reads as "use1". Any other text after "relay:", including "other",
// reads as "other"; a route that is not a relay has no region and reads as empty.
inline std::string RelayRegion(const std::string& route) {
    if (ClassifyRoute(route) != RouteKind::Relayed) return {};
    const auto code = route.substr(std::string("relay:").size());
    return IsRelayRegionCode(code) ? code : std::string("other");
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
