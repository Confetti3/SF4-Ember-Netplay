#pragma once
#include <utility>
#include <vector>

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <ggponet.h>

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Math.hxx"

#include "../common/sf4e__GgpoGate.hxx"
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
				static bool bGgpoConnectionInterrupted;

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

					// Frame identity (v2). simulationFrame is the engine
					// frames-simulated count; ggpoFrame is GGPO's frame
					// argument to the save callback. Reset on slot reuse.
					int simulationFrame = -1;
					int ggpoFrame = -1;
					std::vector<std::pair<GameMementoKey*, GameMementoKey>> keys;
					std::map<
						Dimps::Game::Battle::Sound::SoundPlayerManager::CriPlayerAdapter*,
						Sound::SoundPlayerManager::DeferredSoundRequest
					> criPlayerState;
					std::map<
						Dimps::Game::Battle::Sound::SoundPlayerManager*,
						Platform::SoundObjectPool<4>::SaveState
					> managerState;

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

					static void Free(SaveState* dst);
					static void Save(SaveState* dst);
					static void Load(SaveState* src);
				};

				struct StateSnapshotMeta {
					bool sent;
					bool confirmed;
				};

				static void CaptureSnapshot(Dimps::Game::Battle::System* src);
				static std::map<int, std::pair<SessionProtocol::StateSnapshot, StateSnapshotMeta>> snapshotMap;

				// Desync detection v2: a bounded ring of per-frame hash
				// checkpoints captured every HASH_CHECKPOINT_INTERVAL
				// frames. Entries are exchanged by SessionClient once aged
				// past the rollback/prediction window ("aged", NOT formally
				// GGPO-confirmed). The legacy 60-frame snapshot system
				// above stays fully operational alongside.
				struct HashCheckpoint {
					int frameIdx = -1;
					bool valid = false;
					bool sent = false;
					SemanticHashes hashes;
				};
				static const int NUM_HASH_CHECKPOINTS = 64;
				static const int HASH_CHECKPOINT_INTERVAL = 30;
				// A checkpoint may be exchanged once the sender has
				// simulated this many frames past it (> GGPO's 8-frame
				// prediction window plus input delay margin).
				static const int HASH_CHECKPOINT_AGE_FRAMES = 30;
				static HashCheckpoint hashCheckpoints[NUM_HASH_CHECKPOINTS];
				static SemanticHashes ComputeSemanticHashes(Dimps::Game::Battle::System* src);
				static void CaptureHashCheckpoint(Dimps::Game::Battle::System* src);
				static HashCheckpoint* FindHashCheckpoint(int frameIdx);
				static void ClearHashCheckpoints();
				static GGPOPlayerHandle localPlayerHandle;
				static PlayerConnectionInfo players[MAX_SF4E_PROTOCOL_USERS];
				static GGPOSession* ggpo;
				static SaveState saveStates[NUM_SAVE_STATES];

				static void ApplyGgpoDisconnectSettings(GGPOSession* session);
				static void AbortGgpoMatch(const char* reason);
				static void StartGGPO(GGPOPlayer* players, int numPlayers, int port, int frameDelay, DWORD rngSeed);
				static void StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port, DWORD rngSeed);
				static bool ggpo_on_event_callback(GGPOEvent* info);
				static bool ggpo_begin_game_callback(const char*);
				static bool ggpo_advance_frame_callback(int);
				static bool ggpo_load_game_state_callback(unsigned char*, int);
				static bool ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int);
				static void ggpo_free_buffer(void* buffer);
				static bool ggpo_log_game_state(char* filename, unsigned char* buffer, int);
			};
		}
	}
}
