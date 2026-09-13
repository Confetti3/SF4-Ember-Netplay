#pragma once

#include <memory>
#include <string>

#include <windows.h>

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
            std::string matchNames[2];
        };

        static std::unique_ptr<Netplay> netplay;
        static std::unique_ptr<SessionServer> server;

        static void Install();
        static void Steam_PostUpdate();
        static void StartIrohSession(std::unique_ptr<session::ClientTransport> transport,
            uint16_t port, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay);
        static bool EnterAuthorizedMatch();
        static void ShutdownNetplay(bool closeGgpo = true);
        static void ResetLobbyForRematch();
        static void TryStartPendingMatch();
        static void _OnVsPreBattleTasksRegistered();
        static void _OnVsBattleTasksRegistered();
    };
}
