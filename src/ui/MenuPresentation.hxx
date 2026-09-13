#pragma once
#include "MenuNavigation.hxx"
#include <cctype>

namespace sf4e { namespace ui {
// Presentation names stay independent of stable navigation identities.
inline std::string MenuScreenLabel(const std::string& screen) {
    const std::pair<const char*,const char*> names[]={
        {"home","Home"},{"online","Online Play"},{"selection","Fighter Select"},
        {"player","Player & Controller"},{"defaults","Gameplay Defaults"},
        {"main-character","Choose Your Main"},{"about","Help & About"},
        {"room","Room"},{"room-table","Table"},{"room-rules","Table Rules"},
        {"room-members","Members"},{"room-member","Member"},{"room-chat","Chat"},
        {"room-admin","Room Settings"},{"position","Practice Position"},
        {"recording","Dummy Recording"},{"history","Input History"}};
    for(const auto& name:names)if(screen==name.first)return name.second;
    std::string label=screen;bool word=true;
    for(auto& c:label){if(c=='-')c=' ';if(word)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));word=c==' ';}
    return label;
}
inline const char* MenuPrimaryHint(const MenuEntry* entry) {
    if(!entry||!entry->enabled||entry->adjustable)return nullptr;
    return entry->text?"Edit":entry->confirm?"Review":"Select";
}
} }
