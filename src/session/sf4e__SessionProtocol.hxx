#pragma once

#include <string>
#include <utility>
#include <array>


#include <cstdint>
#include <vector>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../common/StageValue.hxx"
#include "RoomModel.hxx"

#define MAX_SF4E_PROTOCOL_USERS 4

namespace sf4e {
	namespace SessionProtocol {
		typedef Dimps::Math::FixedPoint FixedPoint; 

		// Connection IDs are ephemeral and reusable- they can be used to
		// distinguish clients from each other, but should not be used as
		// any kind of stable identifier. "user" in this context is _not_
		// a typical user account- it only correspond to the `username`
		// portion of a URL. Care should be taken that any references to
		// connection IDs (including those inside client code) are released
		// or deleted when the connection is terminated, to prevent
		// referring to duplicate IDs.
		struct ConnectionID {
			std::string host;
			std::string user;

			bool operator==(const ConnectionID& rhs) const;
		};

		// Similar to connection IDs, lobby IDs are ephemeral and reusable.
		// All lobby IDs containing an empty string as the host or key are
		// equivalent and unreachable, and the canonical null lobby, which
		// contains no members, is represented as {"", ""}.
		struct LobbyID {
			std::string host;
			std::string key;

			bool operator==(const LobbyID& rhs);
			static const LobbyID NULL_LOBBY_ID;
		};

		enum MemberFlags {
			MF_BATTLE_LOADED = 1,
		};

		struct MemberData {
			ConnectionID connId;

			// A user-provided display name.
			std::string name;

			// The IP the client has connected from. While not guaranteed to
			// be reachable, other P2P libraries (ex. GGPO) can try to leverage
			// this to connect directly.
			std::string ip;
			uint16_t port = 0;
			uint64_t flags = 0;
			// Stable room identity survives helper/server process restart. The
			// numeric connId remains a local projection only.
			room::MemberId roomMember = 0;
			std::string authenticatedEndpoint;
			std::uint64_t incarnation = 0;
		};

		struct LobbyData {
			LobbyID id;
			// Default the battle settings so that WITH_DEFAULT
			// deserialization (below) fills sane values when talking to a
			// peer that predates a given field.
			bool editionSelect = true;
			int roundCount = 3;
			FixedPoint roundTime = { 0, 99 };
			bool trainingMode = false;
			std::vector<MemberData> members;

			static const LobbyData NULL_LOBBY;
		};

		struct MatchData {
			MatchData();
			void Clear();
			void ClearReady();
			bool IsAllReady();

			std::array<int64_t, 2> readyMessageNum = {{ -1, -1 }};
			std::array<Dimps::GameEvents::VsMode::ConfirmedCharaConditions, 2> chara;
			int64_t stageID;
			DWORD rngSeed;
			std::array<std::uint8_t, 2> inputDelay = {{ 2, 2 }};
		};

		enum MessageType {
			MT_SESSION_HELLO,
			MT_SESSION_HELLO_RESP,

			MT_SESSION_DATAUPDATE,
			MT_SESSION_JOINREQ,
			MT_SESSION_JOINREJ,

			MT_LOBBY_READY,
			MT_LOBBY_ALLREADY,
			MT_LOBBY_REPORTRESULTS,
			MT_LOBBY_RESET,
			MT_LOBBY_SETSETTINGS,

			MT_PREBATTLE_SETENV,
			MT_PREBATTLE_SETCHARA,
			MT_PREBATTLE_SETSTAGE,

			MT_BATTLE_LOADED,
			MT_BATTLE_SYNCED,
			MT_BATTLE_SNAPSHOT,
			MT_BATTLE_HASH,
			MT_BATTLE_GGPO_FRAME,

			MT_PUNCH_READY,
			MT_PUNCH_GO,

		MT_FORWARD,
		MT_GAME_PREPARE, MT_GAME_PREPARED, MT_GAME_CONNECT, MT_GAME_READY, MT_GAME_START, MT_GAME_END, MT_GAME_PEER_END,
		MT_ROOM_SNAPSHOT, MT_ROOM_ACTION, MT_ROOM_RESULT, MT_ROOM_EVENT,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(MessageType, {
			{MT_SESSION_HELLO, "hello"},
			{MT_SESSION_HELLO_RESP, "hello_resp"},
			{MT_SESSION_DATAUPDATE, "data_update"},
			{MT_SESSION_JOINREJ, "join_rej"},
			{MT_SESSION_JOINREQ, "join_req"},

			{MT_LOBBY_READY, "lobby_ready"},
			{MT_LOBBY_ALLREADY, "lobby_allready"},
			{MT_LOBBY_REPORTRESULTS, "lobby_reportresults"},
			{MT_LOBBY_RESET, "lobby_reset"},
			{MT_LOBBY_SETSETTINGS, "lobby_setsettings"},

			{MT_PREBATTLE_SETENV, "prebattle_setenv"},
			{MT_PREBATTLE_SETCHARA, "prebattle_setchara"},
			{MT_PREBATTLE_SETSTAGE, "prebattle_setstage"},

			{MT_BATTLE_LOADED, "battle_loaded"},
			{MT_BATTLE_SYNCED, "battle_synced"},
			{MT_BATTLE_SNAPSHOT, "battle_snapshot"},
			{MT_BATTLE_HASH, "battle_hash"},
			{MT_BATTLE_GGPO_FRAME, "battle_ggpo_frame"},

			{MT_PUNCH_READY, "punch_ready"},
			{MT_PUNCH_GO, "punch_go"},

			{MT_FORWARD, "forward"},
			{MT_GAME_PREPARE, "game_prepare"}, {MT_GAME_PREPARED, "game_prepared"},
			{MT_GAME_CONNECT, "game_connect"}, {MT_GAME_READY, "game_ready"},
			{MT_GAME_START, "game_start"}, {MT_GAME_END, "game_end"}, {MT_GAME_PEER_END, "game_peer_end"},
			{MT_ROOM_SNAPSHOT, "room_snapshot"}, {MT_ROOM_ACTION, "room_action"},
			{MT_ROOM_RESULT, "room_result"}, {MT_ROOM_EVENT, "room_event"},
		})

		enum JoinResult {
			JOIN_OK = 0,
			JR_REQUEST_INVALID = 1,
			JR_LOBBY_FULL = 2,
			JR_NAME_TAKEN = 3,
			JR_HASH_INVALID = 4,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(JoinResult, {
			{JOIN_OK, "ok"},
			{JR_REQUEST_INVALID, "request_invalid"},
			{JR_LOBBY_FULL, "lobby_full"},
			{JR_NAME_TAKEN, "name_taken"},
			{JR_HASH_INVALID, "hash_invalid"}
		})

		struct SessionHelloMsg {
			MessageType type = MT_SESSION_HELLO;
			ConnectionID cid;
			std::string authenticatedEndpoint;
			std::uint64_t incarnation = 0;
			// New recovery bootstrap clients carry the complete admission request
			// in the first authenticated hello. Older clients leave this object
			// empty and continue with the legacy join_req round trip.
			nlohmann::json admission = nlohmann::json::object();
		};

		struct SessionHelloResp {
			MessageType type = MT_SESSION_HELLO_RESP;
			ConnectionID cid;
			room::MemberId roomMember = 0;
			std::string authenticatedEndpoint;
			std::uint64_t incarnation = 0;
		};

		struct SessionDataUpdate {
			MessageType type = MT_SESSION_DATAUPDATE;
			LobbyData lobbyData;
			MatchData matchData;
			// Custom-room projections carry the room-wide generation that produced
			// this native roster/settings payload. Legacy sessions leave it zero.
			std::uint64_t matchGeneration = 0;
			std::uint64_t authorityTerm = 0;
			std::uint64_t authorityRevision = 0;
		};

		struct SessionJoinReject {
			MessageType type = MT_SESSION_JOINREJ;
			JoinResult result;
		};

		struct SessionJoinRequest {
			MessageType type = MT_SESSION_JOINREQ;
			std::string sidecarHash;
			std::string username;
			uint16_t port;
			bool customRooms = false;
			std::uint32_t roomProtocol = 0;
			int mainFighter = -1;
			// The client keeps its room chat when a snapshot says chat_unchanged.
			bool roomChatDelta = false;
		};

		struct LobbyReady {
			MessageType type = MT_LOBBY_READY;
		};

		struct LobbyAllReady {
			MessageType type = MT_LOBBY_ALLREADY;
		};

		struct LobbyReportResults {
			MessageType type = MT_LOBBY_REPORTRESULTS;
			int32_t loserSide;
		};

		struct LobbyReset {
			MessageType type = MT_LOBBY_RESET;
		};

		// Sent by the host (member 0) after joining its lobby, so that the
		// lobby's battle settings reflect the host's launcher configuration
		// rather than the server defaults.
		struct LobbySetSettings {
			MessageType type = MT_LOBBY_SETSETTINGS;
			bool editionSelect = true;
			int32_t roundCount = 3;
			FixedPoint roundTime = { 0, 99 };
			bool trainingMode = false;
		};

		struct PreBattleSetEnv {
			MessageType type = MT_PREBATTLE_SETENV;
			uint32_t rngSeed;
		};

		struct PreBattleSetChara {
			MessageType type = MT_PREBATTLE_SETCHARA;
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions chara;
		};

		struct PreBattleSetStage {
			MessageType type = MT_PREBATTLE_SETSTAGE;
			int32_t stageID;
		};

		struct BattleLoaded {
			MessageType type = MT_BATTLE_LOADED;
		};

		struct BattleSynced {
			MessageType type = MT_BATTLE_SYNCED;
		};

		struct StateSnapshot {
			struct CharaStateSnapshot {
				int status;
				float rootPos[4];
				int side;
				FixedPoint vit;
				FixedPoint vitmax;
				FixedPoint revenge;
				FixedPoint revengemax;
				FixedPoint recoverable;
				FixedPoint recoverablemax;
				FixedPoint super;
				FixedPoint supermax;
				FixedPoint sctimeamt;
				FixedPoint sctimemax;
				FixedPoint uctime;
				FixedPoint uctimemax;
				FixedPoint damage;
				FixedPoint combodamage;
			};

			int frameIdx;
			CharaStateSnapshot chara[2];
		};

		struct BattleSnapshot {
			MessageType type = MT_BATTLE_SNAPSHOT;
			StateSnapshot snapshot;
		};

		// Desync detection v2 (Phase 6): a canonical semantic hash of one
		// aged (non-speculative) frame checkpoint. "Aged" means the frame
		// is older than the maximum rollback/prediction window on the
		// sender; it is NOT a formal GGPO confirmed frame. Subsystem hashes
		// (battle flow / per-character) classify mismatches. Compatibility:
		// the existing sidecarHash join gate guarantees both clients run
		// the same build, and therefore the same encoder; servers that
		// predate this message simply do not forward it, leaving legacy
		// snapshot verification (which stays active) as the only channel.
		struct BattleHashV2 {
			MessageType type = MT_BATTLE_HASH;
			int32_t frameIdx = -1;
			uint64_t overall = 0;
			uint64_t flow = 0;
			uint64_t chara0 = 0;
			uint64_t chara1 = 0;
			// True when the sender is one of the two players. Spectator
			// hashes are diagnostics only and must never affect the fight.
			bool fromPlayer = false;
		};

		struct BattleGgpoFrame {
			MessageType type = MT_BATTLE_GGPO_FRAME;
			ConnectionID src;
			ConnectionID dest;
			std::vector<uint8_t> data;
		};

		struct PunchReady {
			MessageType type = MT_PUNCH_READY;
		};

		struct PunchGo {
			MessageType type = MT_PUNCH_GO;
		};

		struct ForwardMessage {
			MessageType type = MT_FORWARD;
			ConnectionID src;
			ConnectionID dest;
			nlohmann::json msg;
		};

		struct RoomSnapshotMessage {
			MessageType type = MT_ROOM_SNAPSHOT;
			room::Snapshot snapshot;
			// Wire: "chat_unchanged" replaces "chat" inside the snapshot object.
			// snapshot.chat is then empty and the recipient keeps the chat it holds.
			bool chatUnchanged = false;
		};

		struct RoomActionMessage {
			MessageType type = MT_ROOM_ACTION;
			room::Action action;
		};

		struct RoomResultMessage {
			MessageType type = MT_ROOM_RESULT;
			room::Result result;
			// Echoes the client action so a retry can retire its pending command.
			// Zero preserves the legacy fixture shape.
			std::uint64_t actionId = 0;
		};

		struct RoomEventMessage {
			MessageType type = MT_ROOM_EVENT;
			room::Event event;
		};

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ConnectionID, host, user);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyID, host, key);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MemberData, connId, name, ip, port, flags, roomMember, authenticatedEndpoint, incarnation);
		// WITH_DEFAULT so a LobbyData from a peer built before a field was
		// added (e.g. trainingMode) deserializes with that field's default
		// instead of throwing on the missing key.
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LobbyData, id, editionSelect, roundCount, roundTime, trainingMode, members);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MatchData, readyMessageNum, chara, stageID, rngSeed, inputDelay);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SessionHelloMsg, type, cid, authenticatedEndpoint, incarnation, admission);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SessionHelloResp, type, cid, roomMember, authenticatedEndpoint, incarnation);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SessionDataUpdate, type, lobbyData, matchData, matchGeneration, authorityTerm, authorityRevision);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionJoinReject, type, result);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SessionJoinRequest, type, sidecarHash, username, port, customRooms, roomProtocol, mainFighter, roomChatDelta);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyAllReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReportResults, type, loserSide);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReset, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LobbySetSettings, type, editionSelect, roundCount, roundTime, trainingMode);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetChara, type, chara);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetEnv, type, rngSeed);
		inline void to_json(nlohmann::json& j, const PreBattleSetStage& value) {
			j = nlohmann::json{{"type", value.type}, {"stageID", value.stageID}};
		}
		inline void from_json(const nlohmann::json& j, PreBattleSetStage& value) {
			PreBattleSetStage parsed;
			j.at("type").get_to(parsed.type);
			parsed.stageID = selection::ReadStage(j.at("stageID"));
			value = parsed;
		}
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleGgpoFrame, type, src, dest, data);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PunchReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PunchGo, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForwardMessage, type, src, dest, msg);
		inline void to_json(nlohmann::json& value, const RoomSnapshotMessage& message) {
			value = {{"type", message.type}, {"snapshot", message.snapshot}};
			if (message.chatUnchanged) { value["snapshot"].erase("chat"); value["snapshot"]["chat_unchanged"] = true; }
		}
		inline void from_json(const nlohmann::json& value, RoomSnapshotMessage& message) {
			value.at("type").get_to(message.type);
			const auto& snapshot = value.at("snapshot");
			message.chatUnchanged = snapshot.is_object() && snapshot.value("chat_unchanged", false);
			if (!message.chatUnchanged) { snapshot.get_to(message.snapshot); return; }
			auto withoutChat = snapshot;
			withoutChat["chat"] = nlohmann::json::array();
			withoutChat.get_to(message.snapshot);
		}
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RoomActionMessage, type, action);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RoomResultMessage, type, result, actionId);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RoomEventMessage, type, event);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StateSnapshot::CharaStateSnapshot, status, rootPos, side, vit, vitmax, revenge, revengemax, recoverable, recoverablemax, super, supermax, sctimeamt, sctimemax, uctime, uctimemax, damage, combodamage);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StateSnapshot, frameIdx, chara);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleSnapshot, type, snapshot);
		// WITH_DEFAULT so fields added later deserialize with defaults
		// instead of throwing when talking to an older same-hash dev build.
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(BattleHashV2, type, frameIdx, overall, flow, chara0, chara1, fromPlayer);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleLoaded, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleSynced, type);
	}
}
