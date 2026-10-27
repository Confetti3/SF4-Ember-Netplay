#pragma once

// Shared by the translation units that implement sf4e::Game::Battle::System.
// Not a public interface: the aliases below are only meant for those files.

#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

#include <windows.h>
#include <detours/detours.h>
#include <ggponet.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__Command.hxx"
#include "../Dimps/Dimps__Game__Battle__Effect.hxx"
#include "../Dimps/Dimps__Game__Battle__Hud.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../training/TrainingRuntime.hxx"
#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../common/sf4e__GgpoAbortLatch.hxx"
#include "../common/RollbackHud.hxx"
#include "../common/Localization.hxx"
#include "../common/sf4e__StateHash.hxx"
#include "../common/NativeMatchResult.hxx"
#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e.hxx"
#include "sf4e__CrashDiagnostics.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__Overlay.hxx"
#include "../common/SpectatorPolicy.hxx"
#include "../common/SpectatorCatchUp.hxx"
#include "../common/EnvFlag.hxx"

using Dimps::Platform::WithReleaser;

namespace rHud = Dimps::Game::Battle::Hud;
using CameraUnit = Dimps::Game::Battle::Camera::Unit;
using CharaActor = Dimps::Game::Battle::Chara::Actor;
using CharaUnit = Dimps::Game::Battle::Chara::Unit;
using CommandUnit = Dimps::Game::Battle::Command::Unit;
using EffectUnit = Dimps::Game::Battle::Effect::Unit;
using GameManager = Dimps::Game::Battle::GameManager;
using HudUnit = Dimps::Game::Battle::Hud::Unit;
using NetworkUnit = Dimps::Game::Battle::Network::Unit;
using rSoundPlayerManager = Dimps::Game::Battle::Sound::SoundPlayerManager;
using rSystem = Dimps::Game::Battle::System;
using PauseUnit = Dimps::Game::Battle::Pause::Unit;
using TrainingManager = Dimps::Game::Battle::Training::Manager;
using VfxUnit = Dimps::Game::Battle::Vfx::Unit;
using rKey = Dimps::Game::GameMementoKey;
using FixedPoint = Dimps::Math::FixedPoint;
using fKey = sf4e::Game::GameMementoKey;
using rPadSystem = Dimps::Pad::System;
using fPadSystem = sf4e::Pad::System;
using StateSnapshot = sf4e::SessionProtocol::StateSnapshot;
namespace diag = sf4e::diag;

namespace fHud = sf4e::Game::Battle::Hud;
using fSoundPlayerManager = sf4e::Game::Battle::Sound::SoundPlayerManager;
using fSystem = sf4e::Game::Battle::System;
using fVsBattle = sf4e::GameEvents::VsBattle;
using rSystem = Dimps::Game::Battle::System;

// Defined in sf4e__Game__Battle__System.cxx.
extern rKey::MementoID GGPO_MEMENTO_ID;
void EmitRollbackDiagSummary(const char* label);

// Defined in sf4e__Game__Battle__System__NativeResult.cxx.
extern sf4e::native_result::Timeline s_nativeResultTimeline;
extern bool s_nativeResultEmitted;
void ResetNativeResultMatch();
void CaptureNativeMatchResult(rSystem* system, int stateFrame);
bool MatchResultKnown();
void PublishConfirmedNativeMatchResult();

// Defined in sf4e__Game__Battle__System__Ggpo.cxx.
void NoteDisconnectFlags(int flags);
void LeaveOrphanedNetplayBattle(rSystem* system);
// A spectator's synchronize_input failed irrecoverably: retire its session.
void EndSpectatorStream(int ggpoResult);
// The session state the GGPO files share: the rollback HUD, the callback
// re-entrancy latch, the spectator policy, links and stream watch, and the
// disconnect timeout GGPO reported with CONNECTION_INTERRUPTED.
extern sf4e::RollbackHud rollbackHud;
// Every GGPO callback body runs inside a GgpoCallbackScope (see s_abortLatch
// in sf4e__Game__Battle__System__Ggpo.cxx).
extern sf4e::gate::AbortLatch s_abortLatch;
struct GgpoCallbackScope : sf4e::gate::AbortLatch::Scope {
    GgpoCallbackScope() : sf4e::gate::AbortLatch::Scope(s_abortLatch) {}
};
extern sf4e::SpectatorPolicy s_spectatorPolicy;
extern sf4e::SpectatorLinks s_spectatorLinks;
extern sf4e::SpectatorStreamWatch s_streamWatch;
extern int s_disconnectTimeoutMs;

// Defined in sf4e__Game__Battle__System__GgpoPacing.cxx.
// Always-on netplay time series: one log line per 15 s window and one at the
// end of a session, so tester logs show rift, ping, rollbacks and stalls
// without the diagnostics switch. Pacer totals are logged as window deltas.
struct NetplayWindow {
    uint64_t startMs = 0; // 0 until the first session starts
    uint32_t rollbacks = 0, resimFrames = 0, depth = 0, maxDepth = 0, stallTicks = 0;
    int pingMin = -1, pingMax = -1, localBehind = 0, remoteBehind = 0;
    double maxAbsRift = 0.0;
    uint32_t recsAtStart = 0;
    double slowedAtStart = 0.0, spedUpAtStart = 0.0;
    // A spectator's lag behind the host stream; see SpectatorCatchUp.
    sf4e::SpectatorBacklogWindow spectator;

    void Start(uint64_t now) {
        *this = {};
        startMs = now;
        recsAtStart = fSystem::pacer.recommendationsReceived;
        slowedAtStart = fSystem::pacer.msSlowedTotal;
        spedUpAtStart = fSystem::pacer.msSpedUpTotal;
    }

    void Sample(const GGPONetworkStats& stats) {
        const double ema = fSystem::pacer.riftFramesEma;
        maxAbsRift = (std::max)(maxAbsRift, ema < 0.0 ? -ema : ema);
        localBehind = stats.timesync.local_frames_behind;
        remoteBehind = stats.timesync.remote_frames_behind;
        // GGPO reports 0 until the first round-trip reply arrives.
        const int ping = stats.network.ping;
        if (ping <= 0) return;
        pingMin = pingMin < 0 ? ping : (std::min)(pingMin, ping);
        pingMax = (std::max)(pingMax, ping);
    }

    // Logs the window so far and starts the next one.
    void Flush(uint64_t now) {
        const sf4e::pacing::PacingController& p = fSystem::pacer;
        if (startMs != 0 && now > startMs) {
            spdlog::info(
                "Netplay [{:.0f}s]: ping={}..{} riftFrames={:.2f} maxAbsRift={:.2f} behind={}/{} rollbacks={} "
                "resimFrames={} maxDepth={} stallTicks={} slowedMs={:.1f} spedUpMs={:.1f} timesyncEvents={}",
                (now - startMs) / 1000.0, pingMin, pingMax, p.riftFramesEma, maxAbsRift, localBehind, remoteBehind,
                rollbacks, resimFrames, maxDepth, stallTicks, p.msSlowedTotal - slowedAtStart,
                p.msSpedUpTotal - spedUpAtStart, p.recommendationsReceived - recsAtStart);
            if (fSystem::ggpo && !fSystem::LocalFighter()) {
                spdlog::info("Spectator [{:.0f}s]: maxBacklog={} catchUpFrames={}",
                    (now - startMs) / 1000.0, spectator.maxBacklog, spectator.catchUpFrames);
            }
        }
        Start(now);
    }
};
extern NetplayWindow s_window;
void LogPacerSummary(const char* label);
// A spectator's backlog before its catch-up and the extra frames it played.
void NoteSpectatorBacklog(int backlogFrames, int catchUpFrames);
// Applies development overrides for the pacing caps and resets the
// controller for a new session. Called from StartGGPO/StartSpectating.
void ResetPacerForSession();

// Defined in sf4e__Game__Battle__System__SaveState.cxx.
// Asserts that save state work stays on the game main thread.
void AssertSaveStateThreadAffinity();
// The live simulation frame, for diagnostics; -1 outside a battle.
int SimulatedFrame();
// Installs a state's battle-flow globals, GameManager, recorded sound state
// and memento keys, then restores every unit from them.
void CopyIntoPlace(fSystem::SaveState* src);

// Defined in sf4e__Game__Battle__System__SaveStateRelease.cxx, with SaveState::Free.
void LogSaveStateFreePolicy();
const char* SaveStateFreePathName();
// The default release: each saved payload through a copy of its key, then
// the records are dropped.
void FreeDetached(fSystem::SaveState* victim);

// Defined in sf4e__Game__Battle__System__FrameTrace.cxx. The row of the frame
// fSystem::CaptureSimulatedFrame sampled, its fighters already read (chara).
// The trace starts and ends with the hash checkpoints (ClearHashCheckpoints).
void TraceFrame(rSystem* system, int stateFrame, const fPadSystem::Inputs inputs[2], const fSystem::CharaSemantics chara[2]);
void ClearFrameTrace();

// Defined in sf4e__Game__Battle__System__RollbackStress.cxx.
bool StressStep(rSystem* system);
void StressOpenBattle();
void StressCloseBattle();

// Restores pad playback mode on every exit path. GGPO input playback must
// never leak past the simulation step that installed it.
struct PlaybackFrameScopeGuard {
    ~PlaybackFrameScopeGuard() { fPadSystem::playbackFrame = -1; }
};
