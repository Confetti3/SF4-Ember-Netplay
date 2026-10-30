// V2: the shell's value rows' values, in the order Left and Right step
// through them and in the words the rows show (OfferSteps). A row whose
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
    if (id == "hud-spacing") return Words({"spacing.normal", "spacing.raised"});
    if (id == "delay-policy") return Words({"room.delay_policy.shared", "room.delay_policy.own"});
    if (id == "room-type") return Words({"room.type.classic", "room.type.arcade", "room.type.king"});
    if (id == "visibility") return Words({"public.visibility.private", "public.visibility.public"});
    if (id == "watched-picture" || id == "arrange-picture") return Words({"size.small", "size.large", "room.v2.picture_none"});
    if (id == "hud-size" || id == "picture-size") return Words({"size.small", "size.standard", "size.large"});
    if (id == "hud-layout") for (int i = 0; i <= 1; ++i) labels.push_back(MatchStripLayoutName(i));
    if (id == "hud-position") for (int i = 0; i <= netplay::MaxMatchHudAnchor; ++i) labels.push_back(MatchStripAnchorName(i));
    // Picture corners step top right, top left, bottom right, bottom left (PlayerPreferences::pictureSpot).
    if (id == "picture-corner") for (int anchor : {4, 3, 2, 1}) labels.push_back(MatchStripAnchorName(anchor));
    if (id == "arrange-preset") return Words({"settings.room_preset.board", "settings.room_preset.balanced", "settings.room_preset.chat"});
    if (id == "arrange-column") for (int v = netplay::MinRoomColumn; v <= netplay::MaxRoomColumn; v += netplay::RoomColumnStep) labels.push_back(std::to_string(v) + "%");
    if (id == "arrange-split") for (int v = netplay::MinRoomSplit; v <= netplay::MaxRoomSplit; v += netplay::RoomSplitStep) labels.push_back(std::to_string(v) + " / " + std::to_string(100 - v));
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
    for (auto& e : rows) OfferSteps(e, ValueLabels(e.id, e.value));
}
} }
