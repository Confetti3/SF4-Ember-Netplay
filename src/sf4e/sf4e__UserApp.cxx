#include <chrono>
#include <memory>

#include <windows.h>
#include <detours/detours.h>

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
#include "../common/StageCatalog.hxx"
#include "../common/Localization.hxx"
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
#include "sf4e__Pad.hxx"


namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::App;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using Dimps::Math::FixedPoint;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rSystem = Dimps::Game::Battle::System;
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
enum PacedWaitResult {
    PACED_WAIT_TIMER = 0,
    PACED_WAIT_FALLBACK_SLEEP,
    PACED_WAIT_TIMEOUT,
    PACED_WAIT_FAILED
};

static PacedWaitResult PacedWaitMs(double ms) {
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
            DWORD result = WaitForSingleObject(s_timer, (DWORD)(ms + 50.0));
            if (result == WAIT_OBJECT_0) {
                return PACED_WAIT_TIMER;
            }
            return result == WAIT_TIMEOUT ? PACED_WAIT_TIMEOUT : PACED_WAIT_FAILED;
        }
    }
    Sleep((DWORD)(ms + 0.5));
    return PACED_WAIT_FALLBACK_SLEEP;
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
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.waiting_opponent_join"));
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
    spdlog::info(
        "Netplay: starting match with rounds={} time={}",
        client->_lobbyData.roundCount,
        client->_lobbyData.roundTime.integral
    );
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

static bool StartRuntimeGgpo() {
    static_assert(sizeof(sf4e::Pad::System::Inputs) == sf4e::session::GgpoInputBytes, "Recalculate gameplay packet admission when SF4 inputs change");
    sf4e::NetplayFacade::RuntimeMatchEndpoints endpoints;
    if (!sf4e::NetplayFacade::GetRuntimeMatchEndpoints(endpoints)) return false;
    auto& netplay = fUserApp::netplay;
    if (!netplay || endpoints.participantCount < 2 || endpoints.participantCount > sf4e::room::MaxMatchParticipants) return false;
    for (std::size_t side = 0; side < 2; ++side)
        netplay->matchNames[side] = side < netplay->client._lobbyData.members.size() ?
            netplay->client._lobbyData.members[side].name : std::string{};
    if (endpoints.localSlot >= 2) {
        if (!endpoints.remotePorts[0]) return false;
        sf4e::NetplayFacade::ReleaseRuntimePortToGgpo();
        char loopback[] = "127.0.0.1";
        fSystem::StartSpectating(endpoints.localPort, 2, loopback, endpoints.remotePorts[0], netplay->client._matchData.rngSeed);
    } else {
        GGPOPlayer players[sf4e::room::MaxMatchParticipants] = {};
        const auto slots = endpoints.localSlot == 0 ? endpoints.participantCount : 2;
        std::size_t count = 0;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            // A spectator whose link did not come up before the start has no
            // port; it sits this generation out rather than blocking the fight.
            if (slot >= 2 && !endpoints.remotePorts[slot]) continue;
            auto& player = players[count++]; player.size = sizeof(player); player.player_num = static_cast<int>(slot) + 1;
            if (slot == endpoints.localSlot) {
                player.type = GGPO_PLAYERTYPE_LOCAL;
                if (!sf4e::NetplayFacade::BindRuntimeInput(static_cast<int>(slot))) return false;
            } else {
                if (!endpoints.remotePorts[slot]) return false;
                player.type = slot < 2 ? GGPO_PLAYERTYPE_REMOTE : GGPO_PLAYERTYPE_SPECTATOR;
                strcpy_s(player.u.remote.ip_address, "127.0.0.1");
                player.u.remote.port = endpoints.remotePorts[slot];
            }
        }
        sf4e::NetplayFacade::ReleaseRuntimePortToGgpo();
        if (netplay->client.IsCustomRoom()) {
            const auto committedDelay=netplay->client._matchData.inputDelay[endpoints.localSlot];
            if (committedDelay>10) return false;
            netplay->delay=committedDelay;
        }
        fSystem::StartGGPO(players, static_cast<int>(count), endpoints.localPort, netplay->delay, netplay->client._matchData.rngSeed);
    }
    sf4e::NetplayFacade::ReportGgpoTransport( "127.0.0.1", endpoints.remotePorts[endpoints.localSlot == 0 ? 1 : 0]);
    sf4e::NetplayFacade::ResetGgpoBattleWatch();
    return fSystem::ggpo != nullptr;
}

void fUserApp::_OnVsBattleTasksRegistered() {
    if (!netplay) return;
    if (!sf4e::NetplayFacade::IsRuntimeRoomActive() || !StartRuntimeGgpo())
        fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.authorized_connection_unavailable"));
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
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.waiting_opponent_start"));
        return;
    }
    if (!sf4e::selection::FindStage(netplay->client._matchData.stageID)) {
        spdlog::error("VsPreBattle: rejected unsupported stage ID {}", netplay->client._matchData.stageID);
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.unsupported_stage"));
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

bool fUserApp::EnterAuthorizedMatch() {
    return netplay && StartMatchFromLobby(&netplay->client);
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
    if (netplay && netplay->client.GetRoomSnapshot().roomEpoch) {
        netplay->client.Lobby_ResetRematch();
    }
    else if (server) {
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





void fUserApp::StartIrohSession(std::unique_ptr<session::ClientTransport> transport,
    uint16_t port, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay) {
    SessionClient::Callbacks callbacks = clientCallbacks;
    // IrohMatchSession requires authorization and suppresses legacy all-ready.
    // The normal battle-loaded synchronization callback remains shared.
    netplay.reset(new Netplay(callbacks, sf4e::sidecarHash, port, name, deviceType, deviceIdx, delay));
    netplay->client.Connect(std::move(transport));
}

void fUserApp::Steam_PostUpdate() {
    namespace diag = sf4e::diag;
    diag::ScopedTimer completeOuterCall(diag::OP_COMPLETE_OUTER_CALL);
    const bool diagnosticsEnabled = diag::Enabled();
    const double outerTickStartMs = diagnosticsEnabled ? diag::NowMs() : 0.0;
    double pacingRequestedThisTickMs = 0.0;
    double pacingActualThisTickMs = 0.0;
    {
        diag::ScopedTimer runtimeTimer(diag::OP_RUNTIME_TICK);
        sf4e::NetplayFacade::TickRuntime();
    }


    if (netplay) {
        netplay->client.PrepareForCallbacks();
    }
    if (server) {
        server->PrepareForCallbacks();
    }


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
        // are unaffected (they use the authenticated Iroh UDP bridge).
        diag::ScopedTimer _t(diag::OP_GGPO_IDLE);
        ggpo_idle(fSystem::ggpo, 0);
    }
    // An abort raised inside a callback during that poll closes the session
    // here, once GGPO has unwound.
    fSystem::DrainPendingAbort();
    // After the final advance, the peer's inputs for the result frames can
    // still be confirmed by this poll. Publish from here too.
    fSystem::PollNativeMatchResult();
    fSystem::PollSpectators();
    fSystem::PollTimesync();

    const bool mayPace = fSystem::ggpo && fSystem::MayAdvanceDeterministicFrame() &&
        !fSystem::simGate.predictionStalled;
    // Distributed time-sync pacing (Phase 4): repay a small, bounded slice
    // of the outstanding correction per rendered frame, outside every GGPO
    // callback. Deterministic simulation is never skipped or doubled;
    // this only delays presentation. Skipped while GGPO already stalls us
    // (prediction threshold) and while the gate is closed.
    if (fSystem::pacer.enabled && mayPace) {
        double wantMs = fSystem::pacer.NextWaitMs();
        if (wantMs > 0.0) {
            pacingRequestedThisTickMs = wantMs;
            fSystem::pacer.OnWaitRequested(wantMs);
            double t0 = diag::NowMs();
            PacedWaitResult waitResult = PacedWaitMs(wantMs);
            double actual = diag::NowMs() - t0;
            pacingActualThisTickMs = actual;
            fSystem::pacer.OnWaited(actual);
            if (waitResult == PACED_WAIT_FALLBACK_SLEEP) {
                fSystem::pacer.OnFallbackSleep();
            }
            else if (waitResult == PACED_WAIT_TIMEOUT || waitResult == PACED_WAIT_FAILED) {
                fSystem::pacer.OnWaitFailure(waitResult == PACED_WAIT_TIMEOUT);
            }
            if (diag::Enabled()) {
                diag::G().RecordOp(diag::OP_PACING_WAIT, actual);
            }
        }
    }

    {
        // This timer is deliberately only the original engine method. The
        // complete detoured outer tick is recorded separately below.
        diag::ScopedTimer _t(diag::OP_STEAM_POST_UPDATE);
        rUserApp::staticMethods.Steam_PostUpdate();
    }

    if (diagnosticsEnabled) {
        const double now = diag::NowMs();
        const double outerTickMs = now - outerTickStartMs;
        diag::RollbackDiagnostics& d = diag::G();
        d.RecordOp(diag::OP_OUTER_TICK, outerTickMs);

        // Attribute every over-budget frame. The room.* values nest inside
        // roomRuntime/serverStep. Rate-limited; hitchCounts keep exact totals.
        static double s_lastFrameOverMs = -1.0;
        if (
            fSystem::ggpo &&
            outerTickMs >= 16.67 &&
            (s_lastFrameOverMs < 0.0 || now - s_lastFrameOverMs >= 250.0)
        ) {
            s_lastFrameOverMs = now;
            diag::ScopedTimer logTimer(diag::OP_DIAGNOSTIC_ENQUEUE);
            // Every op that ran this frame, by its TimedOpName.
            std::string ops;
            char part[64];
            for (int op = 0; op < diag::OP_COUNT; ++op) {
                if (d.frameMs[op] <= 0.0) continue;
                snprintf(part, sizeof(part), " %s=%.2f", diag::TimedOpName(op), d.frameMs[op]);
                ops += part;
            }
            spdlog::warn("FrameOver outerTickMs={:.2f} rollbackCallbacks={}{}",
                outerTickMs, d.rollbackCallbacksThisOuterFrame, ops);
        }

        // A 25 ms complete outer tick is a useful rendered-frame hitch
        // candidate at 60 Hz. Rate-limit detailed records: summaries retain
        // exact threshold counts for every occurrence.
        static double s_lastFreezeCandidateMs = -1.0;
        if (
            fSystem::ggpo &&
            outerTickMs >= 25.0 &&
            (s_lastFreezeCandidateMs < 0.0 || now - s_lastFreezeCandidateMs >= 2000.0)
        ) {
            s_lastFreezeCandidateMs = now;
            rSystem* system = rSystem::staticMethods.GetSingleton();
            int simulationFrame = system
                ? rSystem::GetNumFramesSimulated_FixedPoint(system)->integral
                : -1;
            int pingMs = -1;
            int localBehind = 0;
            int remoteBehind = 0;
            GGPONetworkStats stats = { 0 };
            if (fSystem::GetRemoteNetworkStats(stats)) {
                pingMs = stats.network.ping;
                localBehind = stats.timesync.local_frames_behind;
                remoteBehind = stats.timesync.remote_frames_behind;
            }
            const sf4e::GgpoTransportStatus transport =
                sf4e::NetplayFacade::GetGgpoTransportStatus();
            const long long wallMs = (long long)std::chrono::duration_cast<
                std::chrono::milliseconds
            >(std::chrono::system_clock::now().time_since_epoch()).count();
            diag::ScopedTimer logTimer(diag::OP_DIAGNOSTIC_ENQUEUE);
            spdlog::warn(
                "FreezeCandidate wallMs={} outerTickMs={:.2f} simFrame={} ggpoSaveFrame={} "
                "gate={} predictionStalled={} connectionWarning={} pacingDebtMs={:.2f} riftFrames={:.2f} "
                "pacingRequestedMs={:.2f} pacingActualMs={:.2f} rollbackCallbacks={} "
                "lastSaveMs={:.2f} lastLoadMs={:.2f} lastFreeMs={:.2f} saveSlots={} "
                "transport={} pingMs={} localBehind={} remoteBehind={}",
                wallMs,
                outerTickMs,
                simulationFrame,
                fSystem::lastGgpoSaveFrame,
                sf4e::gate::PhaseName(fSystem::simGate.phase),
                fSystem::simGate.predictionStalled,
                fSystem::simGate.connectionWarningActive,
                fSystem::pacer.outstandingMs,
                fSystem::pacer.riftFramesEma,
                pacingRequestedThisTickMs,
                pacingActualThisTickMs,
                d.rollbackCallbacksThisOuterFrame,
                d.ops[diag::OP_SAVE_TOTAL].lastMs,
                d.ops[diag::OP_LOAD_TOTAL].lastMs,
                d.ops[diag::OP_FREE_TOTAL].lastMs,
                d.occupiedSaveSlots.current,
                "Iroh",
                pingMs,
                localBehind,
                remoteBehind
            );
        }

        d.OnOuterFrame(now);
        // Periodic development summary — only while a GGPO session exists,
        // and never per frame.
        if (fSystem::ggpo && d.PeriodicSummaryDue(now, 10.0)) {
            diag::ScopedTimer logTimer(diag::OP_DIAGNOSTIC_ENQUEUE);
            static char s_diagBuf[16384];
            size_t n = d.FormatSummary(s_diagBuf, sizeof(s_diagBuf), "periodic");
            if (n) {
                spdlog::info("\n{}", s_diagBuf);
            }
        }
    }
}
