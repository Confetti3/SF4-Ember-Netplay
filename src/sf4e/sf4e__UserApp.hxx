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
        };

        static std::unique_ptr<Netplay> netplay;
        static std::unique_ptr<SessionServer> server;

        static void Install();
        static void Steam_PostUpdate();
        static void StartSession(char* joinAddr, uint16_t port, std::string& sidecarHash, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay, bool useRelay = true);
        static void ShutdownNetplay(bool closeGgpo = true);
        static bool StartServer(uint16 hostPort, std::string& identity, std::string& sidecarHash, bool editionSelect, int roundCount, Dimps::Math::FixedPoint roundTime);
        static void ResetLobbyForRematch();
        static void TryStartPendingMatch();
        static void TryRestartGgpoLegacyTunnel();
        static void _OnVsPreBattleTasksRegistered();
        static void _OnVsBattleTasksRegistered();
    };
}
