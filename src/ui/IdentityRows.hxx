#pragma once
// Rows and look-ups the Ember ID screens share.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
// A trusted service by its bridge ID, or null.
inline const netplay::IdentityBridge* FindBridge(const ShellView& v, const std::string& id) {
    const auto it = std::find_if(v.identity.bridges.begin(), v.identity.bridges.end(),
        [&](const netplay::IdentityBridge& bridge) { return bridge.id == id; });
    return it == v.identity.bridges.end() ? nullptr : &*it;
}
} }
