#include <chrono>
#include <memory>

#include <windows.h>
#include <detours/detours.h>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <ggponet.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "../session/sf4e__SessionServer.hxx"

#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__UserApp.hxx"

#include "../session/sf4e__GgpoRelay.hxx"
#include "../session/sf4e__GgpoTransport.hxx"

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::App;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using Dimps::Math::FixedPoint;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
using rUserApp = Dimps::UserApp;
using fSystem = sf4e::Game::Battle::System;
using fUserApp = sf4e::UserApp;
using fMainMenu = sf4e::GameEvents::MainMenu;
using fVsBattle = sf4e::GameEvents::VsBattle;
using fVsPreBattle = sf4e::GameEvents::VsPreBattle;
using sf4e::Game::Battle::Sound::SoundPlayerManager;
using sf4e::SessionClient;
using sf4e::SessionServer;

std::unique_ptr<fUserApp::Netplay> fUserApp::netplay;
std::unique_ptr<SessionServer> fUserApp::server;
static bool s_pendingMatchStart = false;

static bool StartMatchFromLobby(SessionClient* const client) {
    sf4e::NetplayFacade::ClearBattleState();
    fVsBattle::bSessionSynced = false;
    fVsBattle::bSessionSentLoaded = false;

    RootEvent* root = App::GetRootEvent();
    if (!root) {
        return false;
    }
    char* mainMenuQuery[1] = { "MainMenu" };
    rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(
        root,
        mainMenuQuery,
        1
    );
    if (!mainMenu) {
        return false;
    }

    ProgressData* progressData = *RootEvent::GetProgressData(root);
    ProgressData::BattleTypeSettings* BattleTypeSettings = &(ProgressData::GetBattleTypeSettings(progressData)[ProgressData::NBT_PVP]);
    *ProgressData::GetNextBattleType(progressData) = ProgressData::NBT_PVP;
    BattleTypeSettings->editionSelect = client->_lobbyData.editionSelect;
    BattleTypeSettings->rounds = client->_lobbyData.roundCount;
    BattleTypeSettings->timeLimit = client->_lobbyData.roundTime;
    fVsPreBattle::bSkipToVersus = true;
    fVsPreBattle::OnTasksRegistered = fUserApp::_OnVsPreBattleTasksRegistered;
    fVsBattle::OnTasksRegistered = fUserApp::_OnVsBattleTasksRegistered;
    (rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
    return true;
}

sf4e::UserApp::Netplay::Netplay(
    const SessionClient::Callbacks& callbacks,
    std::string sidecarHash,
    uint16_t ggpoPort,
    std::string& name,
    uint8_t _deviceType,
    uint8_t _deviceIdx,
    uint8_t _delay
):
    client(callbacks, sidecarHash, ggpoPort, name),
    deviceType(_deviceType),
    deviceIdx(_deviceIdx),
    delay(_delay)
{}

void fUserApp::_OnVsBattleTasksRegistered()
{
    // Start the GGPO connection
    bool isPlayer = false;
    for (int i = 0; i < 2; i++) {
        if (netplay->client._lobbyData.members[i].name == netplay->client._name) {
            isPlayer = true;
            break;
        }
    }
    if (isPlayer) {
        NetplayConfig transportCfg = sf4e::NetplayFacade::GetConfig();
        bool useLegacyGgpoTunnel = netplay->client._useRelay;
        if (
            transportCfg.useCentralSession == 2
            && transportCfg.ggpoTransport == 0
            && transportCfg.ggpoRoomToken[0]
            && transportCfg.ggpoRemotePort > 0
        ) {
            transportCfg.ggpoTransport = (uint8_t)GgpoTransportMode::UdpRelay;
        }
        sf4e::NetplayFacade::RestoreBrokerGgpoEndpoint(transportCfg);
        if (transportCfg.useCentralSession == 2 && transportCfg.ggpoTransport != 0) {
            GgpoTransportMode effective = GgpoTransport::PrepareForBattle(transportCfg);
            sf4e::NetplayFacade::ApplyGgpoTransportConfig(transportCfg);
            useLegacyGgpoTunnel = effective == GgpoTransportMode::LegacySessionTunnel;
            if (!useLegacyGgpoTunnel) {
                spdlog::info(
                    "GgpoTransport: using {} remote {}:{}",
                    GgpoTransport::TransportModeLabel(effective),
                    transportCfg.ggpoRemoteHost,
                    transportCfg.ggpoRemotePort
                );
            }
            else {
                sf4e::NetplayFacade::PushAlert("Netplay: UDP/P2P setup failed; using legacy GGPO tunnel.");
            }
            sf4e::NetplayFacade::ReportGgpoTransport(
                (uint8_t)effective,
                useLegacyGgpoTunnel,
                useLegacyGgpoTunnel ? nullptr : transportCfg.ggpoRemoteHost,
                useLegacyGgpoTunnel ? 0 : transportCfg.ggpoRemotePort
            );
        }
        else if (transportCfg.useCentralSession == 2) {
            sf4e::NetplayFacade::ReportGgpoTransport(0, true, nullptr, 0);
        }

        if (useLegacyGgpoTunnel) {
            GgpoRelay::Instance().Start(netplay->client._ggpoPort, &netplay->client);
        }

        GGPOPlayer players[MAX_SF4E_PROTOCOL_USERS];
        for (int i = 0; i < 2 && i < netplay->client._lobbyData.members.size(); i++) {
            SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
            GGPOPlayer& player = players[i];
            player.size = sizeof(GGPOPlayer);
            player.player_num = i + 1;
            if (netplay->client._lobbyData.members[i].name == netplay->client._name) {
                player.type = GGPO_PLAYERTYPE_LOCAL;

                // Inject the chosen device into this player's side
                Dimps::Pad::System* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
                Dimps::Pad::System::__publicMethods& padSysMethods = Dimps::Pad::System::publicMethods;
                (padSys->*padSysMethods.AssociatePlayerAndGamepad)(i, netplay->deviceIdx);
                (padSys->*padSysMethods.SetDeviceTypeForPlayer)(i, netplay->deviceType);
                (padSys->*padSysMethods.SetSideHasAssignedController)(i, 1);
                (padSys->*padSysMethods.SetActiveButtonMapping)(Dimps::Pad::System::BUTTON_MAPPING_FIGHT);
            }
            else {
                SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
                player.type = GGPO_PLAYERTYPE_REMOTE;
                const NetplayConfig& activeCfg = sf4e::NetplayFacade::GetConfig();
                if (useLegacyGgpoTunnel &&
                    GgpoRelay::Instance().GetRemoteEndpoint(
                        memberData.connId,
                        player.u.remote.ip_address,
                        32,
                        &player.u.remote.port
                    )) {
                    spdlog::info(
                        "GgpoRelay: remote endpoint {}:{}",
                        player.u.remote.ip_address,
                        player.u.remote.port
                    );
                }
                else if (
                    !useLegacyGgpoTunnel &&
                    activeCfg.ggpoRemoteHost[0] &&
                    activeCfg.ggpoRemotePort > 0
                ) {
                    strncpy_s(
                        player.u.remote.ip_address,
                        activeCfg.ggpoRemoteHost,
                        _TRUNCATE
                    );
                    player.u.remote.port = activeCfg.ggpoRemotePort;
                    spdlog::info(
                        "GgpoTransport: remote endpoint {}:{}",
                        player.u.remote.ip_address,
                        player.u.remote.port
                    );
                }
                else if (memberData.ip.empty()) {
                    char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
                    netplay->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);
                    strcpy_s(player.u.remote.ip_address, 32, szAddr);
                    player.u.remote.port = memberData.port;
                }
                else {
                    strcpy_s(player.u.remote.ip_address, 32, memberData.ip.c_str());
                    player.u.remote.port = memberData.port;
                }
            }
        }
        for (int i = 2; i < netplay->client._lobbyData.members.size(); i++) {
            SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
            GGPOPlayer& player = players[i];
            player.type = GGPO_PLAYERTYPE_SPECTATOR;
            player.u.remote.port = memberData.port;

            if (memberData.ip.empty()) {
                char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
                netplay->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);
                strcpy_s(player.u.remote.ip_address, 32, szAddr);
            }
            else {
                strcpy_s(player.u.remote.ip_address, 32, memberData.ip.c_str());
            }
        }
        fSystem::StartGGPO(
            players,
            netplay->client._lobbyData.members.size(),
            netplay->client._ggpoPort,
            netplay->delay,
            netplay->client._matchData.rngSeed
        );
        sf4e::NetplayFacade::ResetGgpoBattleWatch();
    }
    else {
        // Always spectate from	P1 for now- the protocol has
        // limited enough players that there's marginal bandwidth
        // differences.	
        // 
        char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
        char* hostIP;
        if (netplay->client._lobbyData.members[0].ip.empty()) {
            netplay->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);
            hostIP = szAddr;
        }
        else {
            // Safe-_ish_ removal of const. This gets passed through
            // to an inet_pton() call and never modified.
            hostIP = (char*)netplay->client._lobbyData.members[0].ip.c_str();
        }

        fSystem::StartSpectating(
            netplay->client._ggpoPort,
            2,
            hostIP,
            netplay->client._lobbyData.members[0].port,
            netplay->client._matchData.rngSeed
        );
    }
}

void fUserApp::TryRestartGgpoLegacyTunnel() {
    if (!netplay || !fSystem::ggpo) {
        return;
    }

    const sf4e::GgpoSyncPhase phase = sf4e::NetplayFacade::GetGgpoSyncPhase();
    if (
        phase == sf4e::GgpoSyncPhase::Connected ||
        phase == sf4e::GgpoSyncPhase::Synchronizing ||
        phase == sf4e::GgpoSyncPhase::Running
    ) {
        spdlog::warn(
            "GgpoTransport: skip legacy tunnel restart during active GGPO sync (phase={})",
            (int)phase
        );
        return;
    }

    spdlog::warn("GgpoTransport: restarting GGPO on legacy session tunnel");
    ggpo_close_session(fSystem::ggpo);
    fSystem::ggpo = nullptr;
    fSystem::bUpdateAllowed = false;

    netplay->client._useRelay = true;
    if (!GgpoRelay::Instance().Start(netplay->client._ggpoPort, &netplay->client)) {
        spdlog::error("GgpoTransport: legacy tunnel restart failed (GgpoRelay start)");
        sf4e::NetplayFacade::PushAlert("Netplay: GGPO sync failed. Disconnect and retry.");
        return;
    }

    GGPOPlayer players[MAX_SF4E_PROTOCOL_USERS];
    for (int i = 0; i < 2 && i < netplay->client._lobbyData.members.size(); i++) {
        SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
        GGPOPlayer& player = players[i];
        player.size = sizeof(GGPOPlayer);
        player.player_num = i + 1;
        if (netplay->client._lobbyData.members[i].name == netplay->client._name) {
            player.type = GGPO_PLAYERTYPE_LOCAL;
            Dimps::Pad::System* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
            Dimps::Pad::System::__publicMethods& padSysMethods = Dimps::Pad::System::publicMethods;
            (padSys->*padSysMethods.AssociatePlayerAndGamepad)(i, netplay->deviceIdx);
            (padSys->*padSysMethods.SetDeviceTypeForPlayer)(i, netplay->deviceType);
            (padSys->*padSysMethods.SetSideHasAssignedController)(i, 1);
            (padSys->*padSysMethods.SetActiveButtonMapping)(Dimps::Pad::System::BUTTON_MAPPING_FIGHT);
        }
        else {
            player.type = GGPO_PLAYERTYPE_REMOTE;
            if (!GgpoRelay::Instance().GetRemoteEndpoint(
                    memberData.connId,
                    player.u.remote.ip_address,
                    32,
                    &player.u.remote.port
                )) {
                spdlog::error("GgpoTransport: legacy tunnel missing remote virtual endpoint");
                sf4e::NetplayFacade::PushAlert("Netplay: GGPO sync failed. Disconnect and retry.");
                GgpoRelay::Instance().Reset();
                return;
            }
            spdlog::info(
                "GgpoRelay: remote endpoint {}:{}",
                player.u.remote.ip_address,
                player.u.remote.port
            );
        }
    }

    fSystem::StartGGPO(
        players,
        netplay->client._lobbyData.members.size(),
        netplay->client._ggpoPort,
        netplay->delay,
        netplay->client._matchData.rngSeed
    );
    sf4e::NetplayFacade::ReportGgpoTransport(0, true, nullptr, 0);
    sf4e::NetplayFacade::MarkGgpoBattleStarted();
    sf4e::NetplayFacade::PushAlert("Netplay: UDP GGPO failed; using legacy session tunnel.");
}

void fUserApp::_OnVsPreBattleTasksRegistered()
{
    size_t charaConditionSize = sizeof(rVsMode::ConfirmedCharaConditions);

    // XXX (adanducci): this is a little fragile- it's technically possible
    // that the pre-battle event is constructed in another context, but
    // practically speaking the VsPreBattle event will always be used in
    // the context of VsMode.
    char* vsModeQuery[] = { "VSMode" };
    rVsMode* mode = (rVsMode*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), vsModeQuery, 1);
    if (!mode) {
        spdlog::error("VsPreBattle tasks registered, but the current foreground event isn't VSMode!");
        return;
    }

    Dimps::Platform::dString* stageName = rVsMode::GetStageName(mode);
    rVsMode::ConfirmedPlayerConditions* conditions = rVsMode::GetConfirmedPlayerConditions(mode);
    for (int i = 0; i < 2; i++) {
        *(rVsMode::ConfirmedPlayerConditions::GetCharaID(&conditions[i])) = netplay->client._matchData.chara[i].charaID;
        *(rVsMode::ConfirmedPlayerConditions::GetSideActive(&conditions[i])) = 1;
        rVsMode::ConfirmedCharaConditions* charaConditions = rVsMode::ConfirmedPlayerConditions::GetCharaConditions(&conditions[i]);
        memcpy_s(charaConditions, charaConditionSize, &netplay->client._matchData.chara[i], charaConditionSize);
    }

    (stageName->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[netplay->client._matchData.stageID], 4);
    *(rVsMode::GetStageCode(mode)) = netplay->client._matchData.stageID;
}

void OnReady(sf4e::SessionClient* const client, const sf4e::SessionClient::Callbacks& c) {
    if (!StartMatchFromLobby(client)) {
        s_pendingMatchStart = true;
        spdlog::info("Client: deferring match start until main menu");
    }
    else {
        s_pendingMatchStart = false;
    }
}

void OnBattleSynced(SessionClient* const client, const sf4e::SessionClient::Callbacks& callbacks) {
    fVsBattle::bSessionSynced = true;
}

sf4e::SessionClient::Callbacks clientCallbacks = {
    nullptr,
    sf4e::Overlay::OnClientError,
    OnReady,
    OnBattleSynced,
};

void fUserApp::Install() {
    DetourAttach((PVOID*)&rUserApp::staticMethods.Steam_PostUpdate, Steam_PostUpdate);
}

void fUserApp::ShutdownNetplay(bool closeGgpo) {
    s_pendingMatchStart = false;
    sf4e::NetplayFacade::ShutdownNetplay(closeGgpo);
}

void fUserApp::ResetLobbyForRematch() {
    if (server) {
        server->ResetLobbyForRematch();
    }
    else if (netplay) {
        netplay->client.Lobby_ResetRematch();
    }
}

void fUserApp::TryStartPendingMatch() {
    if (!s_pendingMatchStart || !netplay) {
        return;
    }
    if (StartMatchFromLobby(&netplay->client)) {
        s_pendingMatchStart = false;
    }
}

bool fUserApp::StartServer(uint16 hostPort, std::string& identity, std::string& sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) {
    server.reset(new SessionServer(identity, sidecarHash, editionSelect, roundCount, roundTime));
    if (server->Listen(hostPort) != 0) {
        server.reset();
        return false;
    }
    server->PrepareForCallbacks();
    return true;
}

void fUserApp::StartSession(char* joinAddr, uint16_t port, std::string& sidecarHash, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay, bool useRelay) {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.ParseString(joinAddr);
    netplay.reset(new Netplay(
        clientCallbacks,
        sidecarHash,
        port,
        name,
        deviceType,
        deviceIdx,
        delay
    ));
    netplay->client._useRelay = useRelay;
    netplay->client.Connect(addr);
    netplay->client.PrepareForCallbacks();
}

void fUserApp::Steam_PostUpdate() {
    sf4e::NetplayFacade::TickMainMenu();

    if (netplay) {
        netplay->client.PrepareForCallbacks();
    }
    if (server) {
        server->PrepareForCallbacks();
    }
    SteamNetworkingSockets()->RunCallbacks();

    if (netplay) {
        int stepResult = netplay->client.Step();
        if (stepResult < 0) {
            delete netplay.release();
        }
    }

    if (server) {
        if (server->Step() < 0) {
            delete server.release();
        }
    }

    sf4e::NetplayFacade::TickFrame();

    if (fSystem::ggpo) {
        ggpo_idle(fSystem::ggpo, 1);
    }

    rUserApp::staticMethods.Steam_PostUpdate();
}
