#pragma once

#include <windows.h>

#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "sf4e__GameAdapter.hxx"

namespace sf4e {
    // SF4's application object. Ember's room link is the runtime's own
    // (ember::RuntimeSession); this hooks the game's outer tick and starts
    // the battles the room grants (ember::game::Battle).
    struct UserApp : Dimps::UserApp
    {
        static void Install();
        static void Steam_PostUpdate();
        // Battle::Enter: the room's granted match, entered from the main menu.
        static ember::game::Battle::Entry EnterAuthorizedMatch();
        static void ResetLobbyForRematch();
        static void _OnVsPreBattleTasksRegistered();
        static void _OnVsBattleTasksRegistered();
    };
}
