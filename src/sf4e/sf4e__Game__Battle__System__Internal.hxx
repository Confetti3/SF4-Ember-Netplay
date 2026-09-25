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
extern sf4e::native_result::Timeline s_nativeResultTimeline;
extern rKey::MementoID GGPO_MEMENTO_ID;
void EmitRollbackDiagSummary(const char* label);
void ResetNativeResultMatch();
void CaptureNativeMatchResult(rSystem* system, int stateFrame);
bool NativeResultEmitted();

// Defined in sf4e__Game__Battle__System__Ggpo.cxx.
void NoteDisconnectFlags(int flags);
void LogPacerSummary(const char* label);
void LeaveOrphanedNetplayBattle(rSystem* system);

// Defined in sf4e__Game__Battle__System__SaveState.cxx, with SaveState::Free.
void LogSaveStateFreePolicy();
const char* SaveStateFreePathName();

// Defined in sf4e__Game__Battle__System__RollbackStress.cxx.
bool StressStep(rSystem* system);
void StressCloseBattle();

// Restores pad playback mode on every exit path. GGPO input playback must
// never leak past the simulation step that installed it.
struct PlaybackFrameScopeGuard {
    ~PlaybackFrameScopeGuard() { fPadSystem::playbackFrame = -1; }
};
