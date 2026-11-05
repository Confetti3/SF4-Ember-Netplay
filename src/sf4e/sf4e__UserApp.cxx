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
#include "../common/StageCatalog.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../common/Localization.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "../session/sf4e__SessionServer.hxx"

#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__InputLag.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__UserApp.hxx"
#include "sf4e__RuntimeSession.hxx"
#include "sf4e__RuntimeTick.hxx"
#include "sf4e__CustomContent.hxx"
#include "sf4e__Pad.hxx"

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::App;
using Dimps::Event::EventBase;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using Dimps::Math::FixedPoint;
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
using ember::RuntimeSession;

// The room's match checked as the game would take it, logged and named to the
// player when it cannot be: the catalog id of the notice, or nullptr.
static const char* RejectedMatch(const SessionClient& client, const char* where) {
    const auto& lobby = client._lobbyData;
    const auto& match = client._matchData;
    switch (SessionProtocol::FindMatchProblem(lobby, match)) {
    case SessionProtocol::MatchProblem::None:
        return nullptr;
    case SessionProtocol::MatchProblem::Stage:
        spdlog::error("{}: rejected unsupported stage ID {}", where, match.stageID);
        return "runtime.unsupported_stage";
    case SessionProtocol::MatchProblem::Settings:
        spdlog::error("{}: rejected match settings rounds={} time={}.{}", where,
            lobby.roundCount, lobby.roundTime.integral, lobby.roundTime.fractional);
        break;
    case SessionProtocol::MatchProblem::Delay:
        spdlog::error("{}: rejected input delay {}/{}", where, match.inputDelay[0], match.inputDelay[1]);
        break;
    case SessionProtocol::MatchProblem::Fighter:
        spdlog::error("{}: rejected fighters {}/{}/{} and {}/{}/{} (fighter/costume/edition)", where,
            match.chara[0].charaID, match.chara[0].costume, match.chara[0].unc_edition,
            match.chara[1].charaID, match.chara[1].costume, match.chara[1].unc_edition);
        break;
    }
    return "runtime.unsupported_match";
}

using Entry = ember::game::Battle::Entry;

static Entry StartMatchFromLobby(SessionClient* const client) {
    sf4e::NetplayFacade::ClearBattleState();
    fVsBattle::bSessionSynced = false;
    fVsBattle::bSessionSentLoaded = false;
    // Only an entered match arms the next battle; any other answer leaves it empty.
    fVsBattle::nextRequest = {};

    if (!client || client->_lobbyData.members.size() < 2) {
        spdlog::info("Client: deferring match start until opponent joins the lobby");
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.waiting_opponent_join"));
        return Entry::Deferred;
    }
    // Before anything of the match reaches the game: its settings go into the
    // battle settings below and its stage and fighters into VS mode.
    if (const char* notice = RejectedMatch(*client, "Client")) {
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T(notice));
        return Entry::Rejected;
    }

    fVsPreBattle::VersusSettings settings;
    settings.editionSelect = client->_lobbyData.editionSelect;
    settings.rounds = client->_lobbyData.roundCount;
    settings.timeLimit = client->_lobbyData.roundTime;
    // A Versus battle saves its own replay when this is set (0x63CA40), and
    // Sidecar copies each one out of the game's slots as it is saved
    // (platform/ReplayFiles.hxx). A match this PC only
    // watches is recorded unless the player turned that off.
    sf4e::NetplayFacade::RuntimeMatchEndpoints seats;
    const bool watching = sf4e::NetplayFacade::GetRuntimeMatchEndpoints(seats) && seats.localSlot >= 2;
    settings.recordReplay = !watching || sf4e::NetplayFacade::GetRuntimeSnapshotShared()->preferences.recordWatched;
    // The seed and Training rules join the battle request beside the
    // settings above, for the fighters and for whoever watches them. The
    // game prepares that request before the battle registers its tasks,
    // where GGPO starts, so they cannot wait for GGPO.
    settings.next = sf4e::battle::NextRequest::ForMatch(client->_lobbyData.trainingMode, client->_matchData.rngSeed);
    settings.onBattleTasksRegistered = fUserApp::_OnVsBattleTasksRegistered;
    if (!fVsPreBattle::GoToVersus(settings, fUserApp::_OnVsPreBattleTasksRegistered)) {
        return Entry::Deferred;
    }
    // The fighters and editions name the HUD's edition badges the first round
    // shows, for a crash at the battle's start.
    const auto& chara = client->_matchData.chara;
    spdlog::info(
        "Netplay: starting match with rounds={} time={} training={} edition_select={} "
        "fighters {}/{}/{} and {}/{}/{} (fighter/costume/edition) stage={}",
        client->_lobbyData.roundCount,
        client->_lobbyData.roundTime.integral,
        client->_lobbyData.trainingMode,
        client->_lobbyData.editionSelect,
        chara[0].charaID, chara[0].costume, chara[0].unc_edition,
        chara[1].charaID, chara[1].costume, chara[1].unc_edition,
        client->_matchData.stageID
    );
    return Entry::Entered;
}

// Every PC simulating the match, fighters and spectators alike, reads inputs
// the match's way from its first frame.
static bool StartMatchInputLag() {
    const auto& netplay = RuntimeSession::netplay;
    bool seats[2] = {false, false};
    if (const auto* table = sf4e::room::LocalTable(netplay->client.GetRoomSnapshot()))
        for (int side = 0; side < 2; ++side) seats[side] = table->lowerLag[side];
    return sf4e::InputLag::StartMatch(netplay->client._matchData.lowerInputLag, seats);
}

// Why a netplay battle has no GGPO session. NoStream: the session has no
// started match or no link to P1 left, as when P1 dropped this spectator while
// its battle loaded. Failed: this PC could not set GGPO up.
enum class RuntimeGgpoStart { Started, NoStream, Failed };

// A Failed start from fSystem::StartGGPO or StartSpectating names its reason in
// `failure`; the other failures leave it null.
static RuntimeGgpoStart StartRuntimeGgpo(const char*& failure) {
    static_assert(sizeof(sf4e::Pad::System::Inputs) == sf4e::session::GgpoInputBytes, "Recalculate gameplay packet admission when SF4 inputs change");
    sf4e::NetplayFacade::RuntimeMatchEndpoints endpoints;
    if (!sf4e::NetplayFacade::GetRuntimeMatchEndpoints(endpoints)) return RuntimeGgpoStart::NoStream;
    auto& netplay = RuntimeSession::netplay;
    if (!netplay || endpoints.participantCount < 2 || endpoints.participantCount > sf4e::room::MaxMatchParticipants) return RuntimeGgpoStart::Failed;
    sf4e::NetplayFacade::NoteRuntimeMatchStart(endpoints);
    const auto& room = netplay->client.GetRoomSnapshot();
    const auto* table = sf4e::room::LocalTable(room);
    bool started = false;
    if (endpoints.localSlot >= 2) {
        if (!endpoints.remotePorts[0]) return RuntimeGgpoStart::NoStream;
        sf4e::NetplayFacade::ReleaseRuntimePortToGgpo();
        char loopback[] = "127.0.0.1";
        started = fSystem::StartSpectating(endpoints.localPort, 2, loopback, endpoints.remotePorts[0], failure);
        // The HUD keeps this game's delay, as its grant carries it, until the
        // stream ends, as it keeps the starting score.
        const auto& seats = netplay->client._matchData.inputDelay;
        const auto delay = sf4e::room::WatchedGameDelay(room.delayPolicy, seats[0], seats[1]);
        fSystem::matchTelemetry.GrantedDelay(delay.frames, delay.own);
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
                if (!sf4e::NetplayFacade::BindRuntimeInput(static_cast<int>(slot))) return RuntimeGgpoStart::Failed;
            } else {
                if (!endpoints.remotePorts[slot]) return RuntimeGgpoStart::Failed;
                player.type = slot < 2 ? GGPO_PLAYERTYPE_REMOTE : GGPO_PLAYERTYPE_SPECTATOR;
                strcpy_s(player.u.remote.ip_address, "127.0.0.1");
                player.u.remote.port = endpoints.remotePorts[slot];
            }
        }
        sf4e::NetplayFacade::ReleaseRuntimePortToGgpo();
        if (netplay->client.IsCustomRoom()) {
            const auto committedDelay=netplay->client._matchData.inputDelay[endpoints.localSlot];
            if (committedDelay>sf4e::InputDelayWireCeiling) return RuntimeGgpoStart::Failed;
            netplay->delay=committedDelay;
        }
        // A Shared room puts the match delay in both seats, so the two players'
        // logs show the same values here (F-018); an Own room puts each
        // fighter's chosen delay in its seat, and the logs still agree on the pair.
        spdlog::info("Netplay: match delay seats={}/{} local_slot={} applied={}",
            netplay->client._matchData.inputDelay[0], netplay->client._matchData.inputDelay[1],
            endpoints.localSlot, netplay->delay);
        started = fSystem::StartGGPO(players, static_cast<int>(count), endpoints.localPort, netplay->delay,
            netplay->client.SpectatorSyncDeadlineMs(), failure);
    }
    if (started) {
        auto* const root = App::GetRootEvent();
        auto* const progress = root ? *RootEvent::GetProgressData(root) : nullptr;
        // A practice battle never reaches a match result, so the game saves no
        // replay of it for the names to wait for.
        const bool recordingEnabled = progress && ProgressData::GetBattleTypeSettings(progress)[ProgressData::NBT_PVP].recordReplay != 0 &&
            !netplay->client._lobbyData.trainingMode;
        // A fighter's game at a tournament table is a game of the bound set,
        // on the bridge this PC plays it through.
        sf4e::replayfiles::SetTag set;
        const auto published = sf4e::NetplayFacade::GetRuntimeSnapshotShared();
        if (!netplay->spectating && table && table->id == sf4e::room::TournamentTable && room.tournament.Active() &&
            published->tournament.matchId == room.tournament.matchId)
            set = {published->tournament.bridgeId, room.tournament.matchId, room.tournament.bindingRevision};
        sf4e::platform::replays::NoteMatchStart(netplay->matchSides[0].name, netplay->matchSides[1].name, netplay->spectating,
            netplay->client._matchData.chara[0].charaID, netplay->client._matchData.chara[1].charaID,
            recordingEnabled, set);
    }
    sf4e::NetplayFacade::ReportGgpoTransport( "127.0.0.1", endpoints.remotePorts[endpoints.localSlot == 0 ? 1 : 0]);
    sf4e::NetplayFacade::ResetGgpoBattleWatch();
    return started ? RuntimeGgpoStart::Started : RuntimeGgpoStart::Failed;
}

// A spectator without its stream watches the next game at the table, and
// one whose game has already finished needs no notice.
static const char* RuntimeGgpoStartNotice(RuntimeGgpoStart start) {
    if (start == RuntimeGgpoStart::NoStream && sf4e::NetplayFacade::IsRuntimeSpectator())
        return sf4e::NetplayFacade::IsRuntimeMatchEndCommitted() ? "" : sf4e::loc::T("runtime.spectator_stream_lost");
    return sf4e::loc::T("runtime.authorized_connection_unavailable");
}

void fUserApp::_OnVsBattleTasksRegistered() {
    if (!RuntimeSession::netplay) return;
    // Every netplay battle passes here once, before any session exists, so a
    // start that fails below still ends in a battle that leaves (F-016).
    fSystem::simGate.OnNetplayBattleClaimed();
    if (!sf4e::NetplayFacade::IsRuntimeRoomActive())
        fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.authorized_connection_unavailable"));
    else if (!StartMatchInputLag())
        fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.input_lag_unavailable"));
    else {
        // The one abort of a failed start, with the start's own reason when it has one.
        const char* failure = nullptr;
        const auto start = StartRuntimeGgpo(failure);
        if (start != RuntimeGgpoStart::Started) fSystem::AbortGgpoMatch(failure ? failure : RuntimeGgpoStartNotice(start));
    }
}

void fUserApp::_OnVsPreBattleTasksRegistered()
{
    if (!RuntimeSession::netplay) {
        spdlog::error("VsPreBattle tasks registered but netplay is null");
        return;
    }
    if (RuntimeSession::netplay->client._lobbyData.members.size() < 2) {
        spdlog::warn("VsPreBattle: deferring until opponent is in lobby");
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.waiting_opponent_start"));
        return;
    }
    // Entry refused such a match before VS mode began. This keeps the bytes
    // out of the game should they change after entry, which a session that
    // takes host updates until the battle allows.
    if (const char* notice = RejectedMatch(RuntimeSession::netplay->client, "VsPreBattle")) {
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T(notice));
        return;
    }
    // What this PC shows: custom costumes and colors it has play as stand-ins, ones it doesn't have become the
    // original (the match data stays as sent).
    rVsMode::ConfirmedCharaConditions shown[2] = { RuntimeSession::netplay->client._matchData.chara[0], RuntimeSession::netplay->client._matchData.chara[1] };
    sf4e::custom::ShowInstalledLooks(shown);
    // A custom stage plays as its fallback stage, loading its own files if this PC has them.
    const int stage = sf4e::custom::ApplyStage(static_cast<int>(RuntimeSession::netplay->client._matchData.stageID));
    if (!fVsPreBattle::SetSides(shown, stage))
        spdlog::error("VsPreBattle tasks registered, but the current foreground event isn't VSMode!");
}

Entry fUserApp::EnterAuthorizedMatch() {
    return RuntimeSession::netplay ? StartMatchFromLobby(&RuntimeSession::netplay->client) : Entry::Deferred;
}

void fUserApp::Install() {
    DetourAttach((PVOID*)&rUserApp::staticMethods.Steam_PostUpdate, Steam_PostUpdate);
}

void fUserApp::ResetLobbyForRematch() {
    if (sf4e::NetplayFacade::IsControlPlaneLost()) {
        // Degraded mode: the room is gone; no rematch coordination and no
        // stale messages toward a dead connection.
        spdlog::info("Netplay: skipping rematch reset: control plane lost");
        return;
    }
    // A room clears both fighters' Ready itself when it ends the match, and a
    // spectator has no seat to unready; an Unready from here only races that.
    if (RuntimeSession::netplay && RuntimeSession::netplay->client.IsCustomRoom()) {
        return;
    }
    if (RuntimeSession::server) {
        RuntimeSession::server->ResetLobbyForRematch();
    }
    else if (RuntimeSession::netplay) {
        RuntimeSession::netplay->client.Lobby_ResetRematch();
    }
}

namespace {
// SF4's part of Ember's tick: the battle's GGPO poll and what it settled, then
// the frame pacing and the engine's own update. The pacing step is kept for
// the outer tick's diagnostics.
struct Sf4Tick final : ember::GameTick {
    fSystem::PacingTick pacing{};

    void PollBattle() override {
        namespace diag = sf4e::diag;
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
    }

    void UpdateGame() override {
        namespace diag = sf4e::diag;
        // Rift pacing: accounts for the last frame's shift and asks the frame
        // limiter for the next one.
        pacing = fSystem::StepPacing();
        {
            // This timer is deliberately only the original engine method. The
            // complete detoured outer tick is recorded separately below.
            diag::ScopedTimer _t(diag::OP_STEAM_POST_UPDATE);
            rUserApp::staticMethods.Steam_PostUpdate();
        }
    }
};
}

void fUserApp::Steam_PostUpdate() {
    namespace diag = sf4e::diag;
    diag::ScopedTimer completeOuterCall(diag::OP_COMPLETE_OUTER_CALL);
    const bool diagnosticsEnabled = diag::Enabled();
    const double outerTickStartMs = diagnosticsEnabled ? diag::NowMs() : 0.0;
    Sf4Tick game;
    ember::Tick(game);
    const fSystem::PacingTick pacing = game.pacing;

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
                fSystem::ggpoTimeline.Held(),
                sf4e::gate::PhaseName(fSystem::simGate.phase),
                fSystem::simGate.predictionStalled,
                fSystem::simGate.connectionWarningActive,
                fSystem::pacer.outstandingMs,
                fSystem::pacer.riftFramesEma,
                pacing.requestedMs,
                pacing.appliedMs,
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
        // Periodic development summary, only while a GGPO session exists,
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
