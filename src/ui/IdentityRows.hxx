#pragma once
// Rows and look-ups the Ember ID screens share.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
// A trusted service by its bridge ID, or null.
inline const netplay::IdentityBridge* FindBridge(const ShellView& v, const std::string& id) {
    const auto it = std::find_if(v.identity.bridges.begin(), v.identity.bridges.end(),
        [&](const netplay::IdentityBridge& bridge) { return bridge.id == id; });
    return it == v.identity.bridges.end() ? nullptr : &*it;
}
// A trusted service by its address, or null.
inline const netplay::IdentityBridge* FindOrigin(const ShellView& v, const std::string& origin) {
    const auto it = std::find_if(v.identity.bridges.begin(), v.identity.bridges.end(),
        [&](const netplay::IdentityBridge& bridge) { return bridge.origin == origin; });
    return it == v.identity.bridges.end() ? nullptr : &*it;
}
// A service's address, saying when it is built into this version of Ember.
inline std::string ServiceDetail(const netplay::IdentityBridge& bridge) {
    return bridge.builtin ? loc::Tf("identity.service_builtin", bridge.origin) : bridge.origin;
}
} }
