#pragma once

#include <memory>
#include <string>

#include <windows.h>

#include "../common/MatchSide.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionServer.hxx"

namespace sf4e {
    struct UserApp : Dimps::UserApp
    {
        struct Netplay {
            Netplay(
                const SessionClient::Callbacks& callbacks,
                std::string sidecarHash,
                uint16_t ggpoPort,
                std::string& name,
                uint8_t _deviceType,
                uint8_t _deviceIdx,
                uint8_t _delay
            );

            SessionClient client;
            uint8_t deviceType;
            uint8_t deviceIdx;
            uint8_t delay;
            MatchSide matchSides[2];
            // What a spectator's HUD shows for the whole stream: the table's score
            // as the watched game started (see HudScore).
            std::uint32_t startScore[2] = { 0, 0 };
            bool startScoreKnown = false;
            bool spectating = false;
        };

        static std::unique_ptr<Netplay> netplay;
        static std::unique_ptr<SessionServer> server;

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
