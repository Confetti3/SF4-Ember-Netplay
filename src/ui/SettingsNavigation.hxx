#pragma once
#include "MenuNavigation.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
inline bool IsInterfaceScreen(const std::string& screen) {
    return screen=="interface"||screen=="menu-display"||screen=="match-hud"||screen=="ready-alerts";
}
inline bool IsPreferenceScreen(const std::string& screen) {
    if(IsInterfaceScreen(screen))return true;
    for(const char* name:{"profile","main-character","settings","player","controllers","defaults",
        "training-replays","discord","problem-reports","country"})
        if(screen==name)return true;
    return false;
}
// Disabled preferences remain focusable. Say how to unlock them before
// their description, rather than replacing it with a generic Unavailable.
inline void ExplainLockedPreferences(std::vector<MenuEntry>& rows, const std::string& reason) {
    for(auto& row:rows)if(!row.enabled&&(row.adjustable||row.text)) {
        if(row.detail==loc::T("settings.leave_room_to_edit"))row.detail=reason;
        else if(row.detail.find(reason)==std::string::npos)row.detail=reason+"\n\n"+row.detail;
    }
}
} }
