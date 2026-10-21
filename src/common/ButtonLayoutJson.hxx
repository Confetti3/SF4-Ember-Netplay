#pragma once
#include "ButtonLayout.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace gamepads {
inline nlohmann::json LayoutsToJson(const std::vector<StoredLayout>& layouts) {
    auto result = nlohmann::json::array();
    for (const auto& layout : layouts) result.push_back({{"guid", layout.identity.guid}, {"serial", layout.identity.serial},
        {"path", layout.identity.path}, {"name", layout.identity.name}, {"buttons", layout.buttons}, {"edited", layout.edited},
        {"menuConfirm", layout.menu.confirm}, {"menuBack", layout.menu.back}, {"extraButtons", layout.extras},
        {"trainingRecord", layout.training.record}, {"trainingPlay", layout.training.play}});
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
        // A missing or unusable menu binding keeps the game's menu buttons; the layout still loads.
        const auto index = [&](const char* key) { const auto field = entry.find(key); return field != entry.end() && field->is_number_integer() ? field->get<std::int64_t>() : -1; };
        const auto confirm = index("menuConfirm"), back = index("menuBack");
        if (confirm >= -1 && confirm < 10 && back >= -1 && back < 10) layout.menu = {static_cast<int>(confirm), static_cast<int>(back)};
        if (!ValidMenuBinding(layout.menu)) layout.menu = {};
        // Missing or unusable extra button copies leave every extra button off; the layout still loads.
        const auto extras = entry.find("extraButtons");
        if (extras != entry.end() && extras->is_array() && extras->size() == ExtraCount &&
            std::all_of(extras->begin(), extras->end(), [](const nlohmann::json& copy) {
                return copy.is_number_integer() && copy.get<std::int64_t>() >= -1 && copy.get<std::int64_t>() < static_cast<std::int64_t>(LayoutGlyphs.size());
            })) layout.extras = extras->get<ExtraCopies>();
        // Missing or unusable Training buttons leave both off; the layout still loads.
        const auto record = index("trainingRecord"), play = index("trainingPlay");
        if (record >= -1 && record < 10 && play >= -1 && play < 10) layout.training = {static_cast<int>(record), static_cast<int>(play)};
        if (!ValidTrainingBinding(layout.training)) layout.training = {};
        UpsertLayout(layouts, std::move(layout));
    }
    return layouts;
}
} }
