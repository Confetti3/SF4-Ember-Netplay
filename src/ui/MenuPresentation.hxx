#pragma once
#include "MenuNavigation.hxx"
#include "../common/Localization.hxx"
#include <cctype>

namespace sf4e { namespace ui {
// Presentation names stay independent of stable navigation identities.
// nullptr for a screen with no name of its own.
inline const char* MenuScreenName(const std::string& screen) {
    // Every screen the shell, the fighter selector, training and recovery can
    // push. ScreenNames in controller_navigation_test checks the screens it
    // lists, so add a new screen there too.
    const std::pair<const char*,const char*> names[]={
        {"home",loc::T("screen.home")},{"online",loc::T("screen.online")},{"selection",loc::T("screen.selection")},
        {"player",loc::T("screen.player")},{"defaults",loc::T("screen.defaults")},
        {"main-character",loc::T("screen.main_character")},{"about",loc::T("screen.about")},
        {"profile",loc::T("home.profile")},{"settings",loc::T("home.settings")},{"interface",loc::T("settings.interface")},
        {"discord",loc::T("screen.discord")},{"discord-invitation",loc::T("screen.discord_invitation")},
        {"public-rooms",loc::T("screen.public_rooms")},{"identity",loc::T("screen.identity")},{"identity-backup",loc::T("screen.identity_backup")},
        {"linked-accounts",loc::T("screen.linked_accounts")},{"tournament-matches",loc::T("screen.tournament_matches")},
        {"assignment",loc::T("screen.assignment")},{"create",loc::T("online.create")},{"join",loc::T("online.join")},
        {"room",loc::T("screen.room")},{"room-table",loc::T("screen.table")},
        {"room-members",loc::T("screen.members")},{"room-member",loc::T("screen.member")},{"room-chat",loc::T("screen.chat")},
        {"room-admin",loc::T("screen.room_settings")},
        {"roster",loc::T("selection.fighter")},{"appearance",loc::T("selection.appearance")},
        {"costumes",loc::T("selection.costume_gallery")},{"colors",loc::T("selection.color_gallery")},
        {"ultra",loc::T("selection.ultra_combo")},{"stage",loc::T("selection.stage")},{"options",loc::T("selection.additional_options")},
        {"recording",loc::T("screen.dummy_recording")},{"history",loc::T("screen.input_history")},{"developer","Developer"},
        {"recovery",loc::T("screen.recovery")},{"updates",loc::T("screen.updates")}};
    for(const auto& name:names)if(screen==name.first)return name.second;
    return nullptr;
}
inline std::string MenuScreenLabel(const std::string& screen) {
    if(const char* name=MenuScreenName(screen))return name;
    std::string label=screen;bool word=true;
    for(auto& c:label){if(c=='-')c=' ';if(word)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));word=c==' ';}
    return label;
}
inline const char* MenuPrimaryHint(const MenuEntry* entry) {
    if(!entry)return nullptr;
    const auto opens=MenuSelectOpens(*entry);
    if(opens==SelectOpens::Nothing)return nullptr;
    if(!entry->hint.empty())return entry->hint.c_str();
    switch(opens){
    case SelectOpens::Reader:return loc::T("menu.read");
    case SelectOpens::Edit:return loc::T("menu.edit");
    case SelectOpens::Choice:return loc::T("menu.choose");
    case SelectOpens::Confirm:return loc::T("menu.review");
    default:return loc::T("menu.select");
    }
}
} }
