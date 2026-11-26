#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>
#include <string>

namespace sf4e { namespace netplay {
// Fight chat is one object in the netplay section: {"scope": "table" | "everyone"
// | "off", "quickLines": bool, "slots": [four quick line codes]}. A profile from
// before it has none; a field that is missing, of another type or unknown loads
// as its default, a slot alone included, and the other fields load as saved.
inline FightChatPreferences ReadFightChatPreferences(const nlohmann::json& saved) {
    FightChatPreferences value;
    if (!saved.is_object()) return value;
    const auto found = saved.find("fightChat");
    if (found == saved.end() || !found->is_object()) return value;
    const auto scope = found->find("scope");
    if (scope != found->end() && scope->is_string()) {
        const auto text = scope->get<std::string>();
        for (const auto& row : FightChatScopes) if (text == row.word) value.scope = row.scope;
    }
    const auto quick = found->find("quickLines");
    if (quick != found->end() && quick->is_boolean()) value.quickLines = quick->get<bool>();
    const auto slots = found->find("slots");
    if (slots != found->end() && slots->is_array() && slots->size() == quickchat::Slots)
        for (int slot = 0; slot < quickchat::Slots; ++slot) {
            const auto& code = (*slots)[static_cast<std::size_t>(slot)];
            if (!code.is_string()) continue;
            for (const auto& row : quickchat::Table) if (code.get<std::string>() == row.code) value.slots[slot] = row.line;
        }
    return value;
}
inline void WriteFightChatPreferences(const PlayerPreferences& value, nlohmann::json& saved) {
    const auto& chat = value.fightChat;
    auto slots = nlohmann::json::array();
    for (const QuickLine line : chat.slots) slots.push_back(quickchat::RowOf(line).code);
    saved["fightChat"] = {{"scope", ScopeRowOf(chat.scope).word},
        {"quickLines", chat.quickLines}, {"slots", slots}};
}
} }
