#pragma once
#include "MenuNavigation.hxx"
#include "../common/Localization.hxx"
#include <cctype>

namespace sf4e { namespace ui {
// Presentation names stay independent of stable navigation identities.
inline std::string MenuScreenLabel(const std::string& screen) {
    const std::pair<const char*,const char*> names[]={
        {"home",loc::T("screen.home")},{"online",loc::T("screen.online")},{"selection",loc::T("screen.selection")},
        {"player",loc::T("screen.player")},{"defaults",loc::T("screen.defaults")},
        {"main-character",loc::T("screen.main_character")},{"about",loc::T("screen.about")},
        {"room",loc::T("screen.room")},{"room-table",loc::T("screen.table")},{"room-rules",loc::T("screen.table_rules")},
        {"room-members",loc::T("screen.members")},{"room-member",loc::T("screen.member")},{"room-chat",loc::T("screen.chat")},
        {"room-admin",loc::T("screen.room_settings")},{"position",loc::T("screen.practice_position")},
        {"recording",loc::T("screen.dummy_recording")},{"history",loc::T("screen.input_history")}};
    for(const auto& name:names)if(screen==name.first)return name.second;
    std::string label=screen;bool word=true;
    for(auto& c:label){if(c=='-')c=' ';if(word)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));word=c==' ';}
    return label;
}
inline const char* MenuPrimaryHint(const MenuEntry* entry) {
    if(!entry||!entry->enabled||entry->adjustable)return nullptr;
    return entry->text?loc::T("menu.edit"):entry->confirm?loc::T("menu.review"):loc::T("menu.select");
}
} }
