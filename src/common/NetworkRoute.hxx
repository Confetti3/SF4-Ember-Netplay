#pragma once
#include <string>

namespace sf4e {
// The helper names a connection's selected path "ip:<address>:<port>" or
// "relay:<url>". Only these categories leave this header, so logs, exports
// and the interface never carry a peer's address.
enum class RouteKind { Unknown, Direct, Relayed };

inline RouteKind ClassifyRoute(const std::string& route) {
    if (route.rfind("ip:", 0) == 0) return RouteKind::Direct;
    if (route.rfind("relay:", 0) == 0) return RouteKind::Relayed;
    return RouteKind::Unknown;
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
