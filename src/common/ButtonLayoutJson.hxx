#pragma once
#include "ButtonLayout.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace gamepads {
inline nlohmann::json LayoutsToJson(const std::vector<StoredLayout>& layouts) {
    auto result = nlohmann::json::array();
    for (const auto& layout : layouts) {
        nlohmann::json entry{{"guid", layout.identity.guid}, {"serial", layout.identity.serial}, {"path", layout.identity.path},
            {"name", layout.identity.name}, {"buttons", layout.buttons}, {"edited", layout.edited}, {"extraButtons", layout.extras}};
        for (unsigned action = 0; action < BoundActionCount; ++action) entry[BoundActions[action].json] = layout.bound[action];
        result.push_back(std::move(entry));
    }
    return result;
}
struct LayoutLoadError {
    enum class Reason { Length, Mask, Integer } reason;
    std::uint64_t mask = 0;
};
template<class Rejected> std::vector<StoredLayout> ReadLayouts(const nlohmann::json& values, const Rejected& rejected) {
    std::vector<StoredLayout> layouts;
    const auto entries = values.find("buttonLayouts");
    if (entries == values.end() || !entries->is_array()) return layouts;
    unsigned index = 0;
    for (const auto& entry : *entries) {
        const unsigned current = index++;
        if (!entry.is_object() || !entry.contains("buttons") || !entry["buttons"].is_array() || entry["buttons"].size() != 10) {
            rejected(current, LayoutLoadError{LayoutLoadError::Reason::Length}); continue;
        }
        StoredLayout layout;
        bool valid = true;
        for (unsigned i = 0; i < 10; ++i) {
            const auto& button = entry["buttons"][i];
            if (!button.is_number_integer()) {
                rejected(current, LayoutLoadError{LayoutLoadError::Reason::Integer}); valid = false; break;
            }
            const auto mask = button.get<std::uint64_t>();
            if (mask & ~std::uint64_t(FightMask)) {
                rejected(current, LayoutLoadError{LayoutLoadError::Reason::Mask, mask}); valid = false; break;
            }
            layout.buttons[i] = static_cast<std::uint32_t>(mask);
        }
        if (!valid) continue;
        const auto text = [&](const char* key) { const auto field = entry.find(key); return field != entry.end() && field->is_string() ? field->get<std::string>() : std::string{}; };
        layout.identity = {text("guid"), text("serial"), text("path"), text("name")};
        if (entry.contains("edited") && entry["edited"].is_number_integer()) layout.edited = entry["edited"].get<std::int64_t>();
        // A missing or unusable binding, or a pair on one button, leaves that
        // pair unbound (the game's menu buttons, no Training buttons); the layout still loads.
        const auto index = [&](const char* key) { const auto field = entry.find(key); return field != entry.end() && field->is_number_integer() ? field->get<std::int64_t>() : -1; };
        for (unsigned action = 0; action < BoundActionCount; ++action) {
            const unsigned partner = BoundActions[action].partner;
            if (partner < action) continue;
            const auto first = index(BoundActions[action].json), second = index(BoundActions[partner].json);
            if (!BoundIndexInRange(first) || !BoundIndexInRange(second) || (first >= 0 && first == second)) continue;
            layout.bound[action] = static_cast<int>(first); layout.bound[partner] = static_cast<int>(second);
        }
        // Missing or unusable extra button copies leave every extra button off; the layout still loads.
        const auto extras = entry.find("extraButtons");
        if (extras != entry.end() && extras->is_array() && extras->size() == ExtraCount &&
            std::all_of(extras->begin(), extras->end(), [](const nlohmann::json& copy) {
                return copy.is_number_integer() && copy.get<std::int64_t>() >= -1 && copy.get<std::int64_t>() < static_cast<std::int64_t>(LayoutGlyphs.size());
            })) layout.extras = extras->get<ExtraCopies>();
        UpsertLayout(layouts, std::move(layout));
    }
    return layouts;
}
} }
