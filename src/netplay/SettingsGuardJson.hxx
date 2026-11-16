#pragma once
#include "SettingsGuard.hxx"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sf4e { namespace netplay {

// Whether the file's value for a key is the value the program holds for it.
// Numbers compare within what a float keeps (the file has 0.7, the program
// 0.699999988), and a flag saved as 0 or 1 is the flag the program holds.
inline bool SameSavedValue(const nlohmann::json& saved, const nlohmann::json& held) {
    if (saved.is_boolean() && held.is_number()) { const double n = held.get<double>(); return (n == 0.0 || n == 1.0) && saved.get<bool>() == (n == 1.0); }
    if (held.is_boolean() && saved.is_number()) { const double n = saved.get<double>(); return (n == 0.0 || n == 1.0) && held.get<bool>() == (n == 1.0); }
    if (saved.is_number_integer() && held.is_number_integer()) return saved == held;
    if (saved.is_number() && held.is_number()) {
        const double a = saved.get<double>(), b = held.get<double>();
        return std::fabs(a - b) <= 1e-4 * (std::max)(1.0, std::fabs(a));
    }
    if (saved.type() != held.type()) return false;
    if (saved.is_array()) {
        if (saved.size() != held.size()) return false;
        for (std::size_t i = 0; i < saved.size(); ++i) if (!SameSavedValue(saved[i], held[i])) return false;
        return true;
    }
    if (saved.is_object()) {
        if (saved.size() != held.size()) return false;
        for (auto entry = saved.begin(); entry != saved.end(); ++entry) {
            const auto other = held.find(entry.key());
            if (other == held.end() || !SameSavedValue(entry.value(), *other)) return false;
        }
        return true;
    }
    return saved == held;
}

namespace guard_detail {
// Two keys that are one setting: if either is held, both are.
inline void HoldTogether(SettingsGuard& guard, const nlohmann::json& values, const char* first, const char* second) {
    if (guard.held.count(first) == 0 && guard.held.count(second) == 0) return;
    for (const char* key : {first, second}) {
        const auto value = values.find(key);
        if (value != values.end()) guard.held[key] = value->dump();
    }
}
}

// What a read of `saved` left unread: every key the file has that `values`, the
// preferences as the program holds them after reading it, does not reproduce (a
// value of the wrong type, out of range, or one a newer version wrote that this
// one cannot take). Those keep the file's value until the player changes them.
inline SettingsGuard GuardUnreadKeys(const nlohmann::json& saved, const nlohmann::json& values) {
    SettingsGuard guard;
    if (!saved.is_object() || !values.is_object()) return guard;
    for (auto entry = values.begin(); entry != values.end(); ++entry) {
        const auto found = saved.find(entry.key());
        if (found != saved.end() && !SameSavedValue(*found, entry.value())) guard.held[entry.key()] = entry.value().dump();
    }
    // Held only while there is one: a record this build cannot read.
    if (saved.contains("onlineRecord") && !values.contains("onlineRecord")) guard.held["onlineRecord"] = "null";
    // The saved main reads with its "chosen" mark.
    guard_detail::HoldTogether(guard, values, "mainFighter", "mainChosen");
    return guard;
}

// Nothing read: every key is held at the value the program holds now, so only a
// change the player makes is written, and read later it merges under that.
inline SettingsGuard GuardEverything(const nlohmann::json& values) {
    SettingsGuard guard;
    if (!values.is_object()) return guard;
    for (auto entry = values.begin(); entry != values.end(); ++entry) guard.held[entry.key()] = entry.value().dump();
    return guard;
}

// `values` as a save may carry them: nothing when frozen, else without the keys
// still held at the value they were held at.
inline void ApplyGuard(const SettingsGuard& guard, nlohmann::json& values) {
    if (guard.frozen) { values = nlohmann::json::object(); return; }
    for (const auto& held : guard.held) {
        const auto value = values.find(held.first);
        if (value != values.end() && value->dump() == held.second) values.erase(value);
    }
}

} }
