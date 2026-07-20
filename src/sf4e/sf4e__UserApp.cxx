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
#include "../common/agent_debug_log.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
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

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Waits approximately `ms` without touching the global timer resolution
// (no timeBeginPeriod). Prefers a high-resolution waitable timer
// (Win10 1803+); falls back to a plain waitable timer, then Sleep. The
// caller measures the actual elapsed time — coarse waits self-correct
// through PacingController::OnWaited.
static void PacedWaitMs(double ms) {
    static HANDLE s_timer = INVALID_HANDLE_VALUE;
    if (s_timer == INVALID_HANDLE_VALUE) {
        s_timer = CreateWaitableTimerExW(
            NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS
        );
        if (!s_timer) {
            s_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
    }
    if (s_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ms * 10000.0); // relative, 100 ns units
        if (SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE)) {
            // Bounded backstop so a timer failure can never hang the tick.
            WaitForSingleObject(s_timer, (DWORD)(ms + 50.0));
            return;
        }
    }
    Sleep((DWORD)(ms + 0.5));
}

static bool StartMatchFromLobby(SessionClient* const client) {
    sf4e::NetplayFacade::ClearBattleState();
    fVsBattle::bSessionSynced = false;
    fVsBattle::bSessionSentLoaded = false;

    sf4e::agent_debug::Log(
        "H3",
        "UserApp.cxx:StartMatchFromLobby",
        "entry",
        {
            { "hasClient", client != nullptr },
            { "memberCount", client ? (int)client->_lobbyData.members.size() : -1 }
        }
    );

    if (!client || client->_lobbyData.members.size() < 2) {
        spdlog::info("Client: deferring match start until opponent joins the lobby");
        sf4e::NetplayFacade::PushAlert("Waiting for opponent to join the lobby...");
        return false;
    }

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
    if (client->_lobbyData.trainingMode) {
        // Training room: an ordinary VS battle stretched into an endless
        // sparring session. Both clients derive these from the same lobby
        // flag, so the settings stay deterministic across peers.
        //
        // Rounds beyond the menu's 1-7 range work as "first to N/2+1".
        //
        // The time limit is deliberately a huge finite value rather than
        // an "infinite" sentinel: live testing showed a zero time limit
        // is taken literally (the round starts at 0 and instantly ends
        // in time over), and the engine's encoding for the menu's
        // infinite option is unknown. ~2.7 hours per round cannot expire
        // in practice; the worst case is cosmetic (the HUD timer only
        // expects two digits).
        BattleTypeSettings->rounds = 99;
        BattleTypeSettings->timeLimit = { 0, 9999 };
        spdlog::info("Netplay: lobby requested training mode; using endless sparring settings");
    }
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
    if (!netplay) {
        sf4e::agent_debug::Log("H3", "UserApp.cxx:_OnVsBattleTasksRegistered", "netplay_null", {});
        return;
    }
    sf4e::agent_debug::Log(
        "H3",
        "UserApp.cxx:_OnVsBattleTasksRegistered",
        "entry",
        {
            { "memberCount", (int)netplay->client._lobbyData.members.size() },
            { "useCentralSession", (int)sf4e::NetplayFacade::GetConfig().useCentralSession },
            { "useRelay", netplay->client._useRelay ? 1 : 0 }
        }
    );
    if (netplay->client._lobbyData.members.size() < 2) {
        spdlog::warn("Netplay: waiting for second player before starting GGPO");
        sf4e::NetplayFacade::PushAlert("Waiting for opponent in the lobby before the match can start.");
        return;
    }

    // Start the GGPO connection
    bool isPlayer = false;
    for (int i = 0; i < 2 && i < (int)netplay->client._lobbyData.members.size(); i++) {
        if (netplay->client._lobbyData.members[i].connId == netplay->client._cid) {
            isPlayer = true;
            break;
        }
    }
    sf4e::agent_debug::Log(
        "H5",
        "UserApp.cxx:_OnVsBattleTasksRegistered",
        "is_player_branch",
        { { "isPlayer", isPlayer } }
    );
    if (isPlayer) {
        NetplayConfig transportCfg = sf4e::NetplayFacade::GetConfig();
        bool useLegacyGgpoTunnel = false;
        DWORD udpRegisteredAt = 0;
        const bool vpsSession = transportCfg.useCentralSession == 2;
        if (
            vpsSession
            && transportCfg.ggpoTransport == 0
            && transportCfg.ggpoRoomToken[0]
            && transportCfg.ggpoRemotePort > 0
        ) {
            transportCfg.ggpoTransport = (uint8_t)GgpoTransportMode::UdpRelay;
        }
        sf4e::NetplayFacade::RestoreBrokerGgpoEndpoint(transportCfg);
        if (vpsSession && transportCfg.ggpoTransport != 0) {
            // Registration owns the same local UDP port as GGPO. Release any
            // deferred/leftover session before binding the v3 registration socket.
            if (fSystem::ggpo) {
                spdlog::info("GgpoTransport: closing previous GGPO session before registration");
                sf4e::NetplayFacade::CancelDeferredGgpoClose();
                ggpo_close_session(fSystem::ggpo);
                fSystem::ggpo = nullptr;
                GgpoRelay::Instance().Reset();
            }

            GgpoTransportMode effective = GgpoTransportMode::UdpRelay;
            const bool prepared = GgpoTransport::PrepareForBattle(
                transportCfg,
                &effective,
                &netplay->client
            );
            sf4e::NetplayFacade::ApplyGgpoTransportConfig(transportCfg);
            if (!prepared) {
                spdlog::error(
                    "GgpoTransport: prepare failed (mode={}) — aborting battle start",
                    GgpoTransport::TransportModeLabel(effective)
                );
                sf4e::NetplayFacade::ReportGgpoTransport((uint8_t)effective, false, nullptr, 0);
                fSystem::AbortGgpoMatch(
                    "UDP relay unavailable — return to lobby and Get code again."
                );
                return;
            }
            useLegacyGgpoTunnel = effective == GgpoTransportMode::LegacySessionTunnel;
            if (!useLegacyGgpoTunnel) {
                if (effective == GgpoTransportMode::UdpRelay) {
                    udpRegisteredAt = GetTickCount();
                }
                spdlog::info(
                    "GgpoTransport: using {} remote {}:{}",
                    GgpoTransport::TransportModeLabel(effective),
                    transportCfg.ggpoRemoteHost,
                    transportCfg.ggpoRemotePort
                );
            }
            else {
                spdlog::info("GgpoTransport: using forced legacy session tunnel");
            }
            sf4e::NetplayFacade::ReportGgpoTransport(
                (uint8_t)effective,
                useLegacyGgpoTunnel,
                useLegacyGgpoTunnel ? nullptr : transportCfg.ggpoRemoteHost,
                useLegacyGgpoTunnel ? 0 : transportCfg.ggpoRemotePort
            );
        }
        else if (vpsSession) {
            spdlog::error("GgpoTransport: VPS session missing UDP relay endpoint/token");
            sf4e::NetplayFacade::ReportGgpoTransport(1, false, nullptr, 0);
            fSystem::AbortGgpoMatch(
                "UDP relay required — return to lobby and Get code again."
            );
            return;
        }
        else {
            // Non-VPS / local relay: keep prior useRelay tunnel behavior for LAN direct.
            useLegacyGgpoTunnel = netplay->client._useRelay;
        }

        if (useLegacyGgpoTunnel) {
            if (!GgpoRelay::Instance().Start(netplay->client._ggpoPort, &netplay->client)) {
                spdlog::error("Netplay: GgpoRelay failed to start (session tunnel)");
                sf4e::NetplayFacade::PushAlert(
                    "Netplay: could not start GGPO session tunnel. Return to lobby and retry."
                );
                return;
            }
        }

        GGPOPlayer players[MAX_SF4E_PROTOCOL_USERS];
        for (int i = 0; i < 2 && i < netplay->client._lobbyData.members.size(); i++) {
            SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
            GGPOPlayer& player = players[i];
            player.size = sizeof(GGPOPlayer);
            player.player_num = i + 1;
            if (netplay->client._lobbyData.members[i].connId == netplay->client._cid) {
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
                bool remoteResolved = false;
                if (useLegacyGgpoTunnel &&
                    GgpoRelay::Instance().GetRemoteEndpoint(
                        memberData.connId,
                        player.u.remote.ip_address,
                        32,
                        &player.u.remote.port
                    )) {
                    remoteResolved = true;
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
                    remoteResolved = true;
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
                    remoteResolved = true;
                }
                else if (!memberData.ip.empty()) {
                    strcpy_s(player.u.remote.ip_address, 32, memberData.ip.c_str());
                    player.u.remote.port = memberData.port;
                    remoteResolved = true;
                }
                if (!remoteResolved) {
                    spdlog::error(
                        "Netplay: could not resolve GGPO remote endpoint (tunnel={})",
                        useLegacyGgpoTunnel
                    );
                    GgpoRelay::Instance().Reset();
                    fSystem::AbortGgpoMatch(
                        "GGPO could not connect to opponent — return to lobby and Ready again."
                    );
                    return;
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
        if (udpRegisteredAt != 0) {
            spdlog::info(
                "GgpoTransport: registration-to-GGPO handoff gap={}ms localPort={}",
                GetTickCount() - udpRegisteredAt,
                netplay->client._ggpoPort
            );
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
        if (netplay->client._lobbyData.members.empty()) {
            spdlog::error("Netplay: cannot spectate without lobby host member");
            sf4e::NetplayFacade::PushAlert("Netplay: lobby host not available for spectate.");
            return;
        }
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
    sf4e::agent_debug::Log(
        "H3",
        "UserApp.cxx:_OnVsPreBattleTasksRegistered",
        "entry",
        {
            { "hasNetplay", netplay != nullptr },
            { "memberCount", netplay ? (int)netplay->client._lobbyData.members.size() : -1 }
        }
    );
    if (!netplay) {
        spdlog::error("VsPreBattle tasks registered but netplay is null");
        return;
    }
    if (netplay->client._lobbyData.members.size() < 2) {
        spdlog::warn("VsPreBattle: deferring until opponent is in lobby");
        sf4e::NetplayFacade::PushAlert("Waiting for opponent in the lobby before the match can start.");
        return;
    }
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
    sf4e::agent_debug::Log(
        "H4",
        "UserApp.cxx:OnReady",
        "lobby_ready_callback",
        {
            { "memberCount", client ? (int)client->_lobbyData.members.size() : -1 },
            { "allReady", client && client->_matchData.IsAllReady() }
        }
    );
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
    if (sf4e::NetplayFacade::IsControlPlaneLost()) {
        // Degraded mode: the room is gone; no rematch coordination and no
        // stale messages toward a dead connection.
        spdlog::info("Netplay: skipping rematch reset — control plane lost");
        return;
    }
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
    if (netplay->client._lobbyData.members.size() < 2) {
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
    namespace diag = sf4e::diag;
    sf4e::NetplayFacade::TickMainMenu();

    if (netplay) {
        netplay->client.PrepareForCallbacks();
    }
    if (server) {
        server->PrepareForCallbacks();
    }
    SteamNetworkingSockets()->RunCallbacks();

    bool netplayStepFailed = false;
    if (netplay) {
        diag::ScopedTimer _t(diag::OP_SESSION_CLIENT_STEP);
        int stepResult = netplay->client.Step();
        if (stepResult < 0) {
            netplayStepFailed = true;
        }
    }

    bool serverStepFailed = false;
    if (server) {
        diag::ScopedTimer _t(diag::OP_SESSION_SERVER_STEP);
        if (server->Step() < 0) {
            serverStepFailed = true;
        }
    }

    if (netplayStepFailed) {
        // Degrades instead of killing an active healthy GGPO fight; falls
        // back to full failure outside that case (Phase 7).
        sf4e::NetplayFacade::HandleControlPlaneLoss(
            "Lost connection to the game room. Check your internet and try again."
        );
    }
    else if (serverStepFailed) {
        sf4e::NetplayFacade::HandleControlPlaneLoss(
            "Session server error. Check your internet and try again."
        );
    }

    {
        diag::ScopedTimer _t(diag::OP_FACADE_TICK_FRAME);
        sf4e::NetplayFacade::TickFrame();
    }

    if (fSystem::ggpo) {
        // Nonblocking poll. In the pinned fork a nonzero timeout is an
        // unconditional Sleep(1) at the end of Peer2PeerBackend::DoPoll
        // (annotated "obviously a farce" upstream), i.e. a 1-15.6 ms hard
        // stall every tick depending on timer resolution. Timeout 0 performs
        // the identical pump work (Poll::Pump(0); the UDP socket is a
        // nonblocking loop sink) without the sleep. The outer game loop
        // already owns frame cadence; GGPO still gets exactly one pump
        // opportunity per application tick. Setup-time registration waits
        // are unaffected (they live in GgpoTransport, not here).
        diag::ScopedTimer _t(diag::OP_GGPO_IDLE);
        ggpo_idle(fSystem::ggpo, 0);
    }

    // Distributed time-sync pacing (Phase 4): repay a small, bounded slice
    // of the outstanding correction per rendered frame, outside every GGPO
    // callback. Deterministic simulation is never skipped or doubled;
    // this only delays presentation. Skipped while GGPO already stalls us
    // (prediction threshold) and while the gate is closed.
    if (
        fSystem::ggpo &&
        fSystem::MayAdvanceDeterministicFrame() &&
        !fSystem::simGate.predictionStalled
    ) {
        double wantMs = fSystem::pacer.NextWaitMs();
        if (wantMs > 0.0) {
            double t0 = diag::NowMs();
            PacedWaitMs(wantMs);
            double actual = diag::NowMs() - t0;
            fSystem::pacer.OnWaited(actual);
            if (diag::Enabled()) {
                diag::G().RecordOp(diag::OP_PACING_WAIT, actual);
            }
        }
    }

    if (diag::Enabled()) {
        double now = diag::NowMs();
        diag::G().OnOuterFrame(now);
        // Periodic development summary — only while a GGPO session exists,
        // and never per frame.
        if (fSystem::ggpo && diag::G().PeriodicSummaryDue(now, 10.0)) {
            static char s_diagBuf[8192];
            size_t n = diag::G().FormatSummary(s_diagBuf, sizeof(s_diagBuf), "periodic");
            if (n) {
                spdlog::info("\n{}", s_diagBuf);
            }
        }
    }

    {
        diag::ScopedTimer _t(diag::OP_STEAM_POST_UPDATE);
        rUserApp::staticMethods.Steam_PostUpdate();
    }
}
