#pragma once

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>
#include "SessionTransport.hxx"

#include "sf4e__SessionProtocol.hxx"
#include "RoomModel.hxx"

namespace sf4e {
	extern const int SESSION_CLIENT_MAX_MESSAGES_PER_POLL;

	class SessionClient
	{

	public:
		static bool bVerboseLogging;

		enum ErrorType {
			SCE_UNKNOWN,
			SCE_JOIN_REJECTED_HASH_INVALID,
			SCE_JOIN_REJECTED_LOBBY_FULL,
			SCE_JOIN_REJECTED_NAME_TAKEN,
			SCE_JOIN_REJECTED_REQUEST_INVALID,
		};

		struct Callbacks {
			void* data;
			void (*OnError)(ErrorType errorType, SessionClient* const client, const Callbacks& callbacks);
			void (*OnReady)(SessionClient* const client, const Callbacks& callbacks);
			void (*OnBattleSynced)(SessionClient* const client, const Callbacks& callbacks);
			void (*OnRoomSnapshot)(SessionClient* const client, const room::Snapshot& snapshot, const Callbacks& callbacks);
			void (*OnRoomEvent)(SessionClient* const client, const room::Event& event, const Callbacks& callbacks);
		};

		SessionClient(
			const Callbacks& callbacks,
			std::string sidecarHash,
			uint16_t ggpoPort,
			std::string& name
		);
		~SessionClient();

		int Connect(std::unique_ptr<session::ClientTransport> transport,
			bool snapshotsEnabled = true, bool sendHello = true);
		std::string ServerAddress() const;
		void Disconnect();
		int Step();
		void PrepareForCallbacks();
		bool IsConnected() const { return _connected; }
		void RequireMatchAuthorization() { _matchAuthorizationRequired = true; }
		bool TakeGameplayMessage(nlohmann::json& message);
		void SetGameplayGeneration(std::uint64_t generation) { _gameplayGeneration = generation; }
		void NotifyAuthorizedReady() { if (_callbacks.OnReady) _callbacks.OnReady(this, _callbacks); }
		const room::Snapshot& GetRoomSnapshot() const { return _roomSnapshot; }
		// Menu readiness uses live room authority, not the immutable projection
		// retained by native gameplay while the previous match drains.
		bool LocalSelectionLocked(int slot) const;
		session::SendResult SendRoomAction(room::Action action, std::uint64_t* actionId = nullptr);
		// Retry only a previously allocated RecordResult action ID. The caller
		// retains the first queued revision fields so the control bytes remain
		// identical and exact retries can be coalesced safely.
		session::SendResult RetryRoomResult(room::Action action, std::uint64_t actionId);
		// MatchFinished uses the same bounded exact-retry contract as RecordResult,
		// but remains a separately typed API so unrelated room actions cannot reuse
		// an old action ID.
		session::SendResult RetryMatchFinished(room::Action action, std::uint64_t actionId);
		// Queue the authenticated terminal receipt acknowledgement only after the
		// native owner has reached Idle and the local profile outcome is durable.
		// The queue is retried by Step() and remains live until the server accepts
		// the exact room/table/generation action.
		session::SendResult AcknowledgeTerminal(std::uint8_t table, std::uint64_t generation);
		struct ActionReply { std::uint64_t actionId=0; bool accepted=false; room::RejectReason reason=room::RejectReason::None; };
        bool TakeActionReply(ActionReply& reply);
        void SetSelectedDelay(unsigned delay) { if (delay<=10) _selectedDelay=static_cast<std::uint8_t>(delay); }
		void RequireCustomRooms() { _customRoomsRequired = true; }
		// Set before the hello/join exchange; profile editing is offline-only.
		void SetProfileMain(int fighter) { _mainFighter = fighter >= 0 && fighter < 44 ? fighter : -1; }
		bool IsCustomRoom() const { return _customRoomsSeen; }
		const std::string& RoomError() const { return _roomError; }
		void SelectRoomTable(std::uint8_t table);
		bool TakeRoomEvent(room::Event& event);
		// Release the room-table projection after the native match owner has
		// retired GGPO and the match coordinator has reached Idle. This is
		// idempotent so the game-thread owner can call it on every tick.
		bool ReleaseRoomProjection();
		// Freeze the selected room-table projection at the native match grant
		// boundary. The gameplay message path does this for the first grant; the
		// coordinator also calls it after accepting a queued grant that arrived
		// while the previous match was ending.
		void FreezeRoomProjection() { _projectionFrozen = true; }
		// Apply the buffered projection belonging to this exact room-wide
		// generation while frozen so the grant can validate its native roster.
		bool ApplyPendingRoomProjection(std::uint64_t generation);

		// Lobby data
		std::string _name;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
		int64_t _outstandingReadyRequestNumber = -1;
		bool _snapshotsEnabled = false;

		session::SendResult Lobby_Ready();
		session::SendResult Lobby_ReportResults(int loserSide);
		session::SendResult Lobby_ResetRematch();
		session::SendResult Lobby_SetSettings(
			bool editionSelect,
			int roundCount,
			Dimps::Math::FixedPoint roundTime,
			bool trainingMode
		);

		session::SendResult PreBattle_SetEnv(uint32_t rngSeed);
		session::SendResult PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara);
		session::SendResult PreBattle_SetStage(int32_t stageID);

		session::SendResult Battle_Loaded();

		session::SendResult Forward(const SessionProtocol::ConnectionID& dest, const nlohmann::json& msg);

		// Public for testing
		session::SendResult Send(nlohmann::json& msg, int64_t* outMessageNum);

		// Connection related data - public for testing
		std::string _sidecarHash;
		uint16_t _ggpoPort;

		std::map<int, SessionProtocol::StateSnapshot> pendingRemoteSnapshots;
		// Desync v2: remote hash checkpoints received before this client
		// simulated (or aged) the same frame. Bounded — see Step().
		std::map<int, SessionProtocol::BattleHashV2> pendingRemoteHashes;
		SessionProtocol::ConnectionID _cid;

		// True when this client occupies one of the two player slots of the
		// current lobby. Spectator hash mismatches are diagnostics only.
		bool IsLocalPlayer() const;
	private:

		// Connection related data
		Callbacks _callbacks;
		bool _connected = false;
		std::unique_ptr<session::ClientTransport> _transport;
		bool _helloPending = false;
		bool _joinRequestPending = false;
		std::uint64_t _joinRequestNextStep = 0;
		bool _matchAuthorizationRequired = false;
		std::uint64_t _gameplayGeneration = 0;
		std::deque<nlohmann::json> _gameplayMessages;
		std::deque<room::Event> _roomEvents;
		struct PendingTerminalAck {
			std::uint8_t table = 0;
			std::uint64_t generation = 0;
			std::uint64_t roomEpoch = 0;
			std::uint64_t nextStep = 0;
			std::uint64_t actionId = 0;
			room::MemberId member = 0;
			std::uint64_t incarnation = 0;
			// A later authenticated projection may confirm a lost action reply,
			// but only after this exact recipient/generation was observed pending.
			std::uint64_t observedPendingRevision = 0;
		};
		std::deque<PendingTerminalAck> _pendingTerminalAcks;
		std::uint64_t _stepCounter = 0;
		room::Snapshot _roomSnapshot;
		std::string _roomError;
		std::uint64_t _nextRoomActionId = 1;
		struct RetainedRoomRetry {
			std::uint64_t actionId = 0;
			std::string payload;
		};
		RetainedRoomRetry _resultRetry, _finishRetry;
        std::deque<ActionReply> _actionReplies;
        std::uint8_t _selectedDelay=2;
		bool _customRoomsRequired = false;
		int _mainFighter = -1;
		bool _customRoomsSeen = false;
		bool _projectionFrozen = false;
		std::optional<SessionProtocol::SessionDataUpdate> _pendingRoomProjection;
		std::optional<SessionProtocol::SessionDataUpdate> _queuedGrantProjection;
		std::uint64_t _queuedGrantGeneration = 0;
		std::uint64_t _appliedRoomProjectionGeneration = 0;
		std::uint8_t _selectedRoomTable = 0;
		void ProjectSelectedRoomTable();
		void TrySendPendingJoinRequest();
		void ReconcileTerminalAcks();
		RetainedRoomRetry* RetryRecord(room::ActionKind kind);
		session::SendResult RetryRoomLifecycleAction(room::Action action,
			std::uint64_t actionId, room::ActionKind expectedKind);



	};
}
