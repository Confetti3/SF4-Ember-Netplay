// V2: a value row's every value as a choice, so Select opens them all in a
// panel on the row instead of stepping through them with Left and Right. A
// choice's id is "#<n>", n its place in the order Left and Right step
// through, so picking it is that many steps from where the value is, through
// the same handler the arrows use (ValueStep). The labels are the ones the
// row itself shows, which is how the value's place is found. A row whose
// values are not listed here keeps its arrows only.
#include "ApplicationShell.hxx"
#include "ButtonPromptSetting.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cstdio>

namespace sf4e { namespace ui {
namespace {
std::vector<std::string> Words(std::initializer_list<const char*> keys) {
    std::vector<std::string> words;
    for (const char* key : keys) words.push_back(loc::T(key));
    return words;
}
std::vector<std::string> ValueLabels(const std::string& id, const std::string& value) {
    std::vector<std::string> labels;
    if (value == loc::T("common.on") || value == loc::T("common.off")) return Words({"common.off", "common.on"});
    if (id == "look") return Words({"settings.look.classic", "settings.look.v2"});
    if (id == "hud-spacing") return Words({"spacing.normal", "spacing.raised"});
    if (id == "delay-policy") return Words({"room.delay_policy.shared", "room.delay_policy.own"});
    if (id == "visibility") return Words({"public.visibility.private", "public.visibility.public"});
    if (id == "hud-size" || id == "picture-size") return Words({"size.small", "size.standard", "size.large"});
    if (id == "hud-layout") for (int i = 0; i <= 1; ++i) labels.push_back(MatchStripLayoutName(i));
    if (id == "hud-position") for (int i = 0; i <= netplay::MaxMatchHudAnchor; ++i) labels.push_back(MatchStripAnchorName(i));
    // Picture corners step top right, top left, bottom right, bottom left (PlayerPreferences::pictureSpot).
    if (id == "picture-corner") for (int anchor : {4, 3, 2, 1}) labels.push_back(MatchStripAnchorName(anchor));
    if (id == "ready-volume") for (int v = 10; v <= 100; v += 10) labels.push_back(std::to_string(v) + "%");
    if (id == "scale") for (int i = 0; i <= 10; ++i) { char size[16]; std::snprintf(size, sizeof(size), "%.2fx", 1 + i * .05f); labels.push_back(size); }
    if (id == "button-prompts") for (int i = 0; i <= 3; ++i) labels.push_back(ButtonPromptValue(static_cast<gamepads::ButtonPrompts>(i)));
    if (id == "rounds") for (int n : {1, 3, 5, 7, 15, 99}) labels.push_back(std::to_string(n));
    if (id == "time") for (int t : room::RoundTimes) labels.push_back(std::to_string(t));
    if (id == "set-length") for (int n = 0; n <= room::MaxSetLength; ++n) labels.push_back(SetLengthText(static_cast<room::SetFormat>(n)));
    if (id == "rotation") for (int r = 0; r <= 2; ++r) labels.push_back(RotationText(static_cast<room::RotationMode>(r)));
    return labels;
}
}

void ApplicationShell::OfferValues(std::vector<MenuEntry>& rows) {
    for (auto& e : rows) {
        if (!e.adjustable || !e.choices.empty()) continue;
        const auto labels = ValueLabels(e.id, e.value);
        const auto at = std::find(labels.begin(), labels.end(), e.value);
        if (labels.size() < 2 || at == labels.end()) continue;
        for (std::size_t i = 0; i < labels.size(); ++i) e.choices.push_back({"#" + std::to_string(i), labels[i], {}, e.enabled});
        e.chosen = "#" + std::to_string(at - labels.begin());
    }
}

MenuAction ApplicationShell::ValueStep(const MenuAction& chosen, const std::vector<MenuEntry>& rows) {
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == chosen.id; });
    if (row == rows.end() || row->chosen.size() < 2 || chosen.text.size() < 2) return {};
    const int steps = std::stoi(chosen.text.substr(1)) - std::stoi(row->chosen.substr(1));
    return steps ? MenuAction{MenuAction::Adjust, chosen.id, {}, steps} : MenuAction{};
}
} }
