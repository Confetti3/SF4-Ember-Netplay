#pragma once
#include <utility>
#include <vector>


#include <ggponet.h>

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Math.hxx"

#include "../common/sf4e__GgpoGate.hxx"
#include "../common/ConfirmedCheckpoint.hxx"
#include "../common/MatchTelemetry.hxx"
#include "../common/RoomLimits.hxx"
#include "../common/sf4e__PacingController.hxx"
#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e__Platform.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"

#define NUM_SAVE_STATES (GGPO_MAX_PREDICTION_FRAMES + 2)

namespace sf4e {
	namespace Game {
		namespace Battle {
			using Dimps::Game::GameMementoKey;
			using Dimps::Math::FixedPoint;

			struct System : Dimps::Game::Battle::System
			{
				typedef struct AdditionalMemento {
					int nFirstCharaToSimulate;
					DWORD skipRelatedFlags_0xd8c;
					DWORD simulationFlags;
					FixedPoint transitionProgress;
					FixedPoint transitionSpeed;
					int transitionType;

					Dimps::Game::Battle::Network::Unit network;
					Hud::Announce::Unit::AdditionalMemento announce;
					Hud::Notice::Player::AdditionalMemento playerNotices[2];
					Platform::GFxApp::AdditionalMemento gfxApp;
					Eva::TaskCore::AdditionalMemento updateCore;
				} AdditionalMemento;

				struct PlayerConnectionInfo {
					GGPOPlayerType       type;
					GGPOPlayerHandle     handle;
				};

				static bool bHaltAfterNext;
				static bool bUpdateAllowed;

				// Explicit gate model (Phase 2). Tracks session phase,
				// connection warnings, and prediction stalls separately from
				// bUpdateAllowed, which now carries only lifecycle gating,
				// manual/debug pause, and terminal failure.
				static sf4e::gate::GgpoGateModel simGate;

				// The one central answer to "may the next deterministic
				// frame advance?". Connection warnings and prediction
				// stalls intentionally do not gate here.
				static bool MayAdvanceDeterministicFrame();

				// Time-sync pacing (Phase 4): the timesync event only
				// records a bounded correction here; the outer tick
				// repays it in small slices (see fUserApp).
				static sf4e::pacing::PacingController pacer;
				static int nExtraFramesToSimulate;
				static int nNextBattleStartFlowTarget;
				static int nRandomizeLocalInputsEveryXFramesInGGPO;

				static bool extendedLoadRequest;
				static bool extendedSaveRequest;
				static Dimps::Game::GameMementoKey::MementoID mementoLoadRequest;
				static Dimps::Game::GameMementoKey::MementoID mementoSaveRequest;

				static void Install();
				static void RestoreAllFromInternalMementos(Dimps::Game::Battle::System* system, GameMementoKey::MementoID* id);
				static void RecordAllToInternalMementos(Dimps::Game::Battle::System* system, GameMementoKey::MementoID* id);

				int GetMementoSize();
				int RecordToMemento(Memento* memento, GameMementoKey::MementoID* id);
				int RestoreFromMemento(Memento* memento, GameMementoKey::MementoID* id);

				void BattleUpdate();
				void CloseBattle();
				static void OnBattleFlow_BattleStart(System* s);
				void SysMain_HandleTrainingModeFeatures();
				void SysMain_UpdatePauseState();

				// Canonical semantic hashes (desync detection v2). One
				// overall hash plus subsystem hashes for mismatch
				// classification. Computed only from stable semantic
				// values via the canonical encoder; never from raw
				// struct bytes, pointers, or padding.
				struct SemanticHashes {
					uint64_t overall = 0;
					uint64_t flow = 0;
					uint64_t chara[2] = { 0, 0 };
				};

				struct SaveState {
					bool used = false;

					// Whether this state still owns the memento payloads
					// referenced by `keys`. `Save` transfers ownership in (it
					// zeroes the live key afterwards), so a payload is
					// reachable only through `keys` until something hands it
					// back. `FreeByRoundTrip` restores the live keys from its
					// scratch copy and must then drop that copy's claim WITHOUT
					// calling the engine's ClearKey, or it frees the payloads
					// out from under the keys that just took them back. The
					// swap release clears the payloads itself and then drops
					// the claim the same way. See fSystem::SaveState::Free.
					bool ownsKeys = true;

					// Frame identity (v2). simulationFrame is the engine
					// frames-simulated count; ggpoFrame is GGPO's frame
					// argument to the save callback. Reset on slot reuse.
					int simulationFrame = -1;
					int ggpoFrame = -1;
					std::vector<std::pair<GameMementoKey*, GameMementoKey>> keys;
					// Flat, capacity-retaining records of the sound state. A
					// save runs once per simulated and once per re-simulated
					// frame, so these must not allocate per save. Entries are
					// appended in shadowManagerMap order and looked up by a
					// linear scan on restore; an adapter or manager that was
					// not recorded is left untouched (never zeroed).
					std::vector<std::pair<
						Dimps::Game::Battle::Sound::SoundPlayerManager::CriPlayerAdapter*,
						Sound::SoundPlayerManager::DeferredSoundRequest
					>> criPlayerState;
					std::vector<std::pair<
						Dimps::Game::Battle::Sound::SoundPlayerManager*,
						Platform::SoundObjectPool<4>::SaveState
					>> managerState;

					struct GlobalData {
						DWORD CurrentBattleFlow = 0;
						DWORD PreviousBattleFlow = 0;
						DWORD CurrentBattleFlowSubstate = 0;
						DWORD PreviousBattleFlowSubstate = 0;
						FixedPoint CurrentBattleFlowFrame = { 0, 0 };
						FixedPoint CurrentBattleFlowSubstateFrame = { 0, 0 };
						FixedPoint PreviousBattleFlowFrame = { 0, 0 };
						FixedPoint PreviousBattleFlowSubstateFrame = { 0, 0 };
						void (*BattleFlowSubstateCallable_aa9258)(Dimps::Game::Battle::System * s) = nullptr;
						void (*BattleFlowCallback_CallEveryFrame_aa9254)(Dimps::Game::Battle::System * s) = nullptr;

						Dimps::Game::Battle::GameManager gameManager = { 0 };
					};
					GlobalData d;

					SaveState();

					// Copying would duplicate the `keys` ownership records
					// and hand two objects a claim on the same memento
					// payloads — the exact aliasing this type exists to
					// prevent. Slots are referenced by pointer everywhere.
					SaveState(const SaveState&) = delete;
					SaveState& operator=(const SaveState&) = delete;

					// Releases a state's memento payloads without changing
					// the live game. Swap-and-clear by default; see
					// docs/SAVESTATE_FREE.md.
					static void Free(SaveState* dst);
					// The v0.8.5 release (install victim, clear, restore live),
					// selected with SF4E_LEGACY_SAVESTATE_FREE=1.
					static void FreeByRoundTrip(SaveState* dst);
					static void Save(SaveState* dst, bool temporary = false);
					static void Load(SaveState* src);

					// Returns a slot to the clean, unowned, unused state
					// without touching engine memento data. Only safe when
					// the payloads are known to be owned elsewhere (or gone,
					// as after a battle teardown) — otherwise use Free.
					static void Reclaim(SaveState* victim, const char* reason, int slotIndex);
				};

				struct StateSnapshotMeta {
					bool sent;
					bool confirmed;
				};

				static void CaptureSnapshot(Dimps::Game::Battle::System* src);
				static std::map<int, std::pair<SessionProtocol::StateSnapshot, StateSnapshotMeta>> snapshotMap;

				// Desync detection v2: a bounded ring of per-frame hash
				// checkpoints captured every HASH_CHECKPOINT_INTERVAL
				// frames. `frameIdx` is the non-wrapping GGPO state frame, not
				// the signed 16-bit engine counter. Entries are exchanged only
				// after GGPO confirms all inputs contributing to the captured
				// state. The legacy snapshot system above stays fully operational
				// alongside.
				struct HashCheckpoint {
					int frameIdx = -1;
					int ggpoStateFrame = -1;
					bool valid = false;
					bool sent = false;
					SemanticHashes hashes;
				};
				static const int NUM_HASH_CHECKPOINTS = sf4e::statehash::CheckpointRingSize;
				static const int HASH_CHECKPOINT_INTERVAL = sf4e::statehash::CheckpointInterval;
				static HashCheckpoint hashCheckpoints[NUM_HASH_CHECKPOINTS];
				static SemanticHashes ComputeSemanticHashes(Dimps::Game::Battle::System* src);
				static void CaptureHashCheckpoint(Dimps::Game::Battle::System* src);
				static HashCheckpoint* FindHashCheckpoint(int frameIdx);
				static void ClearHashCheckpoints();
				static GGPOPlayerHandle localPlayerHandle;
				static int lastGgpoSaveFrame;
				static PlayerConnectionInfo players[room::MaxMatchParticipants];
				static GGPOSession* ggpo;
				static SaveState saveStates[NUM_SAVE_STATES];

				// Logs savestate pool occupancy on teardown/startup paths so
				// the existing log files can localize pool corruption without
				// a crash dump.
				static void LogSaveSlotOccupancy(const char* label);

				static void ApplyGgpoDisconnectSettings(GGPOSession* session);
				static void RetireGgpoSession(const char* diagnosticsLabel);
				// Safe to call from anywhere, including GGPO callbacks: inside a
				// callback the abort is latched and completed by
				// DrainPendingAbort() once the top-level GGPO call returns.
				static void AbortGgpoMatch(const char* reason);
				// Completes an abort latched inside a callback. Returns true
				// when a session was closed. Must be called after every
				// top-level GGPO API call that can run callbacks.
				static bool DrainPendingAbort();
				// Milliseconds until GGPO drops the interrupted peer, or -1
				// when no connection warning is active. HUD only.
				static int DisconnectCountdownMs();
				static void StartGGPO(GGPOPlayer* players, int numPlayers, int port, int frameDelay, DWORD rngSeed);
				static void StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port, DWORD rngSeed);
				static bool ggpo_on_event_callback(GGPOEvent* info);
				static bool ggpo_begin_game_callback(const char*);
				static unsigned RecentRollbackFrames();
				// Publishes a GGPO-confirmed native outcome if one is waiting.
				// Safe to call outside GGPO callbacks at any time.
				static void PollNativeMatchResult();
                static sf4e::MatchTelemetry matchTelemetry;
                static void PollMatchTelemetry();
                static bool ggpo_advance_frame_callback(int);
				static bool ggpo_load_game_state_callback(unsigned char*, int);
				static bool ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int);
				static void ggpo_free_buffer(void* buffer);
				static bool ggpo_log_game_state(char* filename, unsigned char* buffer, int);
			};
		}
	}
}
