#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <windows.h>

#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/SessionTransport.hxx"

namespace sf4e {
    // SF4's application object. Ember's room link is the runtime's own
    // (ember::RuntimeSession); this hooks the game's outer tick and starts
    // the battles the room grants.
    struct UserApp : Dimps::UserApp
    {
        static void Install();
        static void Steam_PostUpdate();
        static void StartIrohSession(std::unique_ptr<session::ClientTransport> transport,
            uint16_t port, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay);
        // Deferred: not yet (no opponent, or away from the main menu).
        // Rejected: the room's match is one the game cannot take; nothing of
        // it was written, and the caller leaves the game.
        enum class MatchEntry { Deferred, Entered, Rejected };
        static MatchEntry EnterAuthorizedMatch();
        static void ShutdownNetplay(bool closeGgpo = true);
        static void ResetLobbyForRematch();
        static void TryStartPendingMatch();
        static void _OnVsPreBattleTasksRegistered();
        static void _OnVsBattleTasksRegistered();
    };
}
