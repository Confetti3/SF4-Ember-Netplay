#pragma once

#include <map>
#include <array>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "SessionTransport.hxx"
#include "MatchAuthority.hxx"
#include "SessionRecovery.hxx"
#include <nlohmann/json.hpp>

#include "SessionPlainTypes.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "RoomModel.hxx"

namespace sf4e {
	extern const int SESSION_SERVER_MAX_MESSAGES_PER_POLL;

	class SessionServer
	{
	private:
		// A binary blob usable by any host for routing messages to this
		// server. This is most likely an IP address and port, a hostname
		// and port, or in extreme cases an overlay network's concept of
		// addressing (ex. an index in a service discovery protocol).
		// Identities of clients connected to the server are prefixed with
		// this identity. This allows all servers in a cluster to forward
		// messages to any user connected to any server in the cluster-
		// just send it to the prefixed identity, and that host will take
		// care of the rest.
		std::string _identity;

		// Connection related data
		std::string _sidecarHash;
		std::unique_ptr<session::ServerTransport> _transport;
		bool _transportFailed = false;
		std::unique_ptr<session::MatchAuthority> _matchAuthority;
		std::array<std::unique_ptr<session::MatchAuthority>, room::TableCount> _roomMatchAuthorities{};
		std::array<std::uint8_t, 16> _matchAuthorizationRoom{};
		session::MatchAuthority::Identity _matchAuthorizationIdentity;
		std::function<std::uint64_t(session::Connection)> _memberIncarnation;
		bool _matchAuthorizationConfigured = false;
		std::vector<std::pair<session::Connection, nlohmann::json>> _afterDataMessages;
		session::MatchAuthority::Send MatchSender();
		bool BeginAuthorizedTable(std::uint8_t table, std::uint64_t generation);
		// Begins the match on every table a MatchReady event names and returns
		// the resulting room events.
		std::vector<room::Event> StartReadyTables(const std::vector<room::Event>& events);
		session::MatchAuthority* RoomMatchAuthority(std::uint8_t table);
		const session::MatchAuthority* RoomMatchAuthority(std::uint8_t table) const;
		std::uint8_t RoomTableForGeneration(std::uint64_t generation) const;
		void SendRoomTable(std::uint8_t table, const nlohmann::json& message);
		// Whether the connection is in the live native roster of the table's
		// generation. Table members outside it (a spectator still retiring the last
		// game, or one left out of the start) are not.
		bool IsRoomTableParticipant(session::Connection connection, std::uint8_t table) const;
		// One of the table's two fighters in its current match.
		bool IsRoomTableFighter(session::Connection connection, std::uint8_t table) const;
		// A connection reaching a table's native barrier for its live generation:
		// the two fighters gate it, and the arrivals are reset with each grant. A
		// participant that arrives after the fighters is released at once.
		void ArriveAtRoomBarrier(session::Connection connection, bool loadBarrier, const nlohmann::json& release);

		void BroadcastMessage(const nlohmann::json& msg);
		// One handler per message type, called from Step().
		void HandleSessionHello(session::Connection conn, const nlohmann::json& msg, const session::Message& incoming);
		void HandleRoomAction(session::Connection conn, const nlohmann::json& msg, std::vector<room::Event>& deferredRoomEvents);
		void HandleMatchAcknowledgement(session::Connection conn, const nlohmann::json& msg);
		void HandleForward(session::Connection conn, const nlohmann::json& msg, const SessionProtocol::ConnectionID& cid);
		void HandleJoinRequest(session::Connection conn, const nlohmann::json& msg, const session::Message& incoming, SessionProtocol::ConnectionID cid);
		void HandleSetChara(session::Connection conn, const nlohmann::json& msg);
		void HandleSetEnv(session::Connection conn, const nlohmann::json& msg);
		void HandleSetStage(session::Connection conn, const nlohmann::json& msg);
		void HandleLobbySetSettings(session::Connection conn, const nlohmann::json& msg);
		void HandleBattleLoaded(session::Connection conn, bool& bSendBattleSynced);
		void HandleLobbyReady(session::Connection conn, const nlohmann::json& msg, const session::Message& incoming, bool& bSendLobbyAllReady);
		void HandleLobbyReportResults(session::Connection conn, const nlohmann::json& msg);
		void HandleLobbyReset(session::Connection conn, const nlohmann::json& msg);
		void HandlePunchReady(session::Connection conn);
		void HandleGgpoFrame(session::Connection conn, const nlohmann::json& msg, const SessionProtocol::ConnectionID& cid);
		void Respond(session::Connection client, const nlohmann::json& msg, bool retainPublicReplay = true);

		// Direct lobby data manipulation utilities
		SessionProtocol::JoinResult RegisterToWait(
			const session::Connection& conn,
			const uint16_t& port,
			const std::string& sidecarHash,
			const std::string& name,
			const std::string& peerAddr,
			SessionProtocol::ConnectionID& cid,
			const room::MemberProfile& profile = {}
		);
		void HandleResults(int loserSide);

	public:
		SessionServer(std::string identity, std::string sidecarHash,
			bool editionSelect, int roundCount, Dimps::Math::FixedPoint roundTime,
			std::unique_ptr<session::ServerTransport> transport);
		~SessionServer();

		int Listen(uint16_t nPort);
		int Step();
		int Close();
		void PrepareForCallbacks();
		void ResetBattleSync();
		void ResetLobbyForRematch();
		void EnableMatchAuthorization(std::array<std::uint8_t, 16> room, session::MatchAuthority::Identity identity,
			std::function<std::uint64_t(session::Connection)> incarnation = {});
		// Enables the multi-table room authority while retaining the legacy
		// two-player path until this method is called by the room bootstrap.
		void EnableCustomRooms(const std::string& name = "Private room",
			std::uint8_t capacity = static_cast<std::uint8_t>(room::MaximumMembers),
			std::uint64_t roomEpoch = 1, room::Rules defaults = room::Rules());
		// Server-owned mode, for a room host process with no player host: no
		// connection is the local host, and every join names an account (the
		// player's Ember ID) that the embedding process supplied for that
		// connection with SetConnectionAccount before the join arrives. Kicks ban
		// the account. False when the room could not be created in that mode.
		bool EnableServerOwnedRooms(const std::string& name = "Public room",
			std::uint8_t capacity = static_cast<std::uint8_t>(room::MaximumMembers),
			std::uint64_t roomEpoch = 1, room::Rules defaults = room::Rules());
		void SetConnectionAccount(session::Connection connection, std::string account);
		// The moderation view: the accounts banned as of the last committed state.
		// A kick still in an uncommitted recovery candidate is not in it, so what
		// is reported or forwarded can never be taken back.
		std::vector<std::string> BannedAccounts() const;
		// Closes a server-owned room in the model, as a private room's host does
		// with the Close action: every member's snapshot shows it closed. False
		// while the authority cannot take a mutation (retry next tick).
		bool CloseServerOwnedRoom();
		// True when a server-owned room is empty in the committed view because its
		// last member left with a Leave action. A member who dropped may come back,
		// so a room emptied that way is false and waits out the supervisor's grace.
		bool ServerOwnedRoomLeftEmpty() const;
		bool CustomRoomsEnabled() const { return static_cast<bool>(_roomAuthority); }
		const room::Snapshot* RoomSnapshot() const { return !_roomAuthority ? nullptr : (_hasRecoveryProjection ? &_recoveryProjection : &_roomAuthority->SnapshotView()); }
		void AdvanceCustomRoom(std::uint64_t nowMs);
		// Applies the tournament binding this server's own helper checked to
		// the room it leads, broadcasts the room and proposes the change. False
		// when the room cannot take it now (not the writable owner, a quorum
		// round in progress) or refuses it; the caller tries again later.
		bool BindTournament(const room::TournamentBinding& binding);
		// Private recovery state, never a player-facing room snapshot. Import is
		// atomic and does not send messages, create capabilities, or touch sockets.
		nlohmann::json Checkpoint() const;
		bool RestoreCheckpoint(const nlohmann::json& checkpoint);
		nlohmann::json RecoveryCheckpoint() const;
		std::uint64_t RecoveryCheckpointBuilds() const { return _recoveryCheckpointBuilds; }
		bool RestoreRecoveryCheckpoint(const nlohmann::json& checkpoint);
		// Replicated import. journal and authority replace the checkpoint's own
		// "effect_journal" and "authority", which are then neither copied nor
		// re-encoded. journal must already be compacted (CompactEffectJournal),
		// which is what bounds its encoded size.
		bool RestoreRecoveryCheckpoint(const nlohmann::json& checkpoint,
			std::vector<session::EffectEnvelope> journal, const session::AuthorityStamp& authority);

		// Root/helper authority bridge. A proposal is made only after the owner
		// has privately applied a poll/timer command and journaled its effects.
		void SetAuthority(std::uint64_t term, std::uint64_t revision, bool writable,
			bool coordinationHealthy = false);
		bool RecoveryEnabled() const { return _recovery.Enabled(); }
		bool RecoveryWritable() const { return _recovery.Writable(); }
		bool HasRecoveryCandidate() const { return _recoveryCandidateReady; }
		bool RecoveryCandidateOverflowed() const { return _candidate.overflow; }
		bool ProposeCheckpoint(std::uint64_t request, std::uint64_t term, std::uint64_t baseRevision,
			const nlohmann::json& checkpoint);
		std::shared_ptr<const session::SessionProposal> PendingProposal() const { return _recovery.PendingProposal(); }
		bool ApplyCommit(std::uint64_t request, std::uint64_t term, std::uint64_t revision,
			const nlohmann::json& committedCheckpoint, const session::EffectDigest& effectsDigest);
		void DiscardProposal();
		// After a term transition, the new writable owner calls this once after
		// its stable control handles have been rebound. It creates a normal
		// private candidate that tears down committed-but-not-Started native
		// preparations and publishes the ordered game_end/room effects.
		bool CancelInterruptedPreparations();
		const std::vector<session::EffectEnvelope>& CommittedEffectHistory() const { return _committedEffectHistory; }
		using StableRebind = std::tuple<room::MemberId, session::Connection, SessionProtocol::ConnectionID, std::uint64_t>;
		// Rebind the complete stable roster in one operation. Numeric handles
		// may be a permutation of the prior process; all native authorities and
		// readiness sets are updated only after every endpoint validates.
		bool RebindMembers(const std::vector<StableRebind>& bindings);
		// Re-delivers every MatchEnded receipt this member has not yet
		// acknowledged (lost while disconnected or compacted). Returns the
		// number of events sent. Without this a recipient that missed its
		// MatchEnded could never acknowledge, and the table stayed fenced.
		std::size_t ReplayPendingTerminalEvents(session::Connection connection, room::MemberId member);

		size_t ConnectedClientCount() const { return clients.size(); }

		// Identifies the room chat a recipient already holds: its last sequence
		// and length (a departed sender's lines are pruned without a new one).
		struct ChatVersion {
			std::uint64_t last = 0; std::size_t count = 0;
			bool operator==(const ChatVersion& other) const { return last == other.last && count == other.count; }
		};
		typedef struct SessionMember {
			SessionProtocol::MemberData data;
			session::Connection conn;
			// Local to this owner, never checkpointed: a successor starts from
			// false and sends every member the full chat again.
			bool chatDelta = false;        // the client accepts snapshots without unchanged chat
			bool chatSent = false;         // chatVersion was committed to this client
			ChatVersion chatVersion;
		} SessionMember;
		ChatVersion CurrentChatVersion() const;
		void EnableChatDelta(session::Connection connection, bool enabled);

		std::map<session::Connection, SessionProtocol::ConnectionID> cidMap;
		std::map<session::Connection, room::MemberId> roomMembers;
		std::map<session::Connection, std::uint8_t> roomSelectedTables;
		// RoomModel keeps the native ConnectionID in Member.connection. The
		// authorization identity used to prevent a kicked peer from reconnecting
		// is tracked separately so UI/native projections remain CID-compatible.
		std::map<room::MemberId, std::string> roomPeerIdentities;
		std::map<room::MemberId, std::uint64_t> roomIncarnations;
		// A spectator can leave after native Started while its frozen slot and
		// departed endpoint remain in MatchAuthority. Keep that stable tombstone
		// outside the public RoomModel roster so portable recovery can restore the
		// authority without treating the departed peer as a live room member.
		struct FrozenMember {
			room::ConnectionRef endpoint;
			SessionProtocol::MemberData data;
			std::uint64_t incarnation = 1;
		};
		std::map<room::MemberId, FrozenMember> roomFrozenMembers;
		std::map<room::MemberId, std::uint8_t> _recoveryPendingSelected;
		std::array<std::set<room::MemberId>, room::TableCount> _recoveryPendingBattleLoaded{};
		std::array<std::set<room::MemberId>, room::TableCount> _recoveryPendingPunchReady{};
		std::set<std::string> roomBannedIdentities;
		std::set<session::Connection> _departingConnections;
		// Server-owned rooms: the account the embedding process vouched for per
		// connection. Local to this process and dropped when the connection closes.
		std::map<session::Connection, std::string> _connectionAccounts;
		std::uint64_t _incarnation = 1;
		session::SessionRecoveryGate _recovery;
		mutable std::uint64_t _recoveryCheckpointBuilds = 0;
		bool _recoveryCandidateReady = false;
		bool _recoveryFlushing = false;
		nlohmann::json _recoveryBaseline;
		// What the open recovery candidate has journaled. Reset as a whole.
		struct RecoveryCandidate {
			// envelope is the journal entry; encoded is the payload, dumped once
			// for its digest and reused for the live send; envelopeBytes is
			// json(envelope).dump().size(), the entry's share of bytes.
			struct Effect {
				session::EffectEnvelope envelope; session::Connection local = 0;
				std::string encoded; std::size_t envelopeBytes = 0;
			};
			std::vector<Effect> effects;
			std::size_t bytes = 2;   // of json(the envelopes).dump()
			bool overflow = false;
			// Chat versions sent in this candidate, committed by ApplyCommit.
			std::map<session::Connection, ChatVersion> chatSent;
			std::vector<session::EffectEnvelope> Envelopes() const {
				std::vector<session::EffectEnvelope> envelopes;
				envelopes.reserve(effects.size());
				for (const auto& effect : effects) envelopes.push_back(effect.envelope);
				return envelopes;
			}
		};
		RecoveryCandidate _candidate;
		std::vector<session::EffectEnvelope> _committedEffectHistory;
		// Encoded size of each _committedEffectHistory entry, or empty when not
		// yet known (after a restore); ApplyCommit fills it on first use.
		std::vector<std::size_t> _committedEffectSizes;
		// Captured before a private candidate mutates the authenticated maps. A
		// rejected term transition must rebind the exact prior handles, including
		// a join/leave permutation, rather than resolving from the candidate.
		std::vector<StableRebind> _recoveryBaselineBindings;
		struct InterruptedPreparation { std::uint8_t table = 0; std::uint64_t generation = 0; };
		std::vector<InterruptedPreparation> _pendingInterruptedPreparations;
		bool _preparationCancellationRequested = false;
		bool _cancellationCandidate = false;
		std::uint64_t _nextEffectSequence = 1;
		// Passive replicas retain relative timer ages. This local anchor advances
		// them only while the helper still reports a writable, rebound quorum.
		std::uint64_t _passiveTimerClock = 0;
		bool _coordinationHealthy = false;
		bool _hasRecoveryProjection = false;
		room::Snapshot _recoveryProjection;
		std::vector<std::string> _recoveryBanned; // the committed bans, beside _recoveryProjection
		void BeginRecoveryCandidate();
		void FinishRecoveryCandidate();
		void JournalEffect(session::Connection client, const nlohmann::json& payload, bool retainPublicReplay = true);
		bool ValidateEffectRecipient(const session::EffectEnvelope& effect, session::Connection candidate, session::Connection& local) const;
		void CancelPrecommittedGenerations();
		void RememberInterruptedPreparations();
		bool IsInterruptedPreparation(std::uint8_t table, std::uint64_t generation) const;
		// A game_prepared/game_ready its match authority no longer expects.
		bool IsStaleMatchAck(const session::Message& message) const;
		void DropRecoveryCandidate();
		bool RestoreRecoveryBaseline();
		// authority is null when the checkpoint carried none.
		bool RestoreRecoveryState(const nlohmann::json& checkpoint,
			std::vector<session::EffectEnvelope> journal, const session::AuthorityStamp* authority);
		void CaptureFrozenMember(room::MemberId member, const room::Snapshot& prior);
		// Drops a member the room removed: its connection, identity and slots.
		void ForgetRoomMember(room::MemberId member);
		void PruneFrozenMembers();
		std::vector<SessionMember> clients;

		// Lobby data: Public for visibility into tests only.
		bool _dataDirty;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
		bool _punchReady[2] = { false, false };
		std::unique_ptr<room::RoomAuthority> _roomAuthority;
		// The last departure from a server-owned room was a Leave that emptied it.
		bool _serverOwnedLeftEmpty = false;
		std::map<std::uint8_t, SessionProtocol::MatchData> _roomMatchData;
		std::array<std::set<session::Connection>, room::TableCount> _roomBattleLoaded{};
		std::array<std::set<session::Connection>, room::TableCount> _roomPunchReady{};

		void BroadcastRoomState(const std::vector<room::Event>& events = {});
		void SendRoomProjection(session::Connection connection);
		void ProjectRoomTable(session::Connection connection, std::uint8_t table);
		std::uint8_t RoomTableFor(session::Connection connection) const;
		int RoomSideFor(session::Connection connection, std::uint8_t table) const;
	};
}
