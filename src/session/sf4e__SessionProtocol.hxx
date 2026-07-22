#pragma once

#include <string>
#include <utility>


#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"

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
			uint16_t port;
			uint64_t flags;
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

			int64_t readyMessageNum[2];
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions chara[2];
			int64_t stageID;
			DWORD rngSeed;
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
		};

		struct SessionHelloResp {
			MessageType type = MT_SESSION_HELLO_RESP;
			ConnectionID cid;
		};

		struct SessionDataUpdate {
			MessageType type = MT_SESSION_DATAUPDATE;
			LobbyData lobbyData;
			MatchData matchData;
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
		// even when the SessionServer runs remotely (VPS relay) with
		// defaulted settings.
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

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ConnectionID, host, user);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyID, host, key);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MemberData, connId, name, ip, port);
		// WITH_DEFAULT so a LobbyData from a peer built before a field was
		// added (e.g. trainingMode) deserializes with that field's default
		// instead of throwing on the missing key.
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LobbyData, id, editionSelect, roundCount, roundTime, trainingMode, members);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchData, readyMessageNum, chara, stageID, rngSeed);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionHelloMsg, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionHelloResp, type, cid);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionDataUpdate, type, lobbyData, matchData);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionJoinReject, type, result);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionJoinRequest, type, sidecarHash, username, port);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyAllReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReportResults, type, loserSide);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReset, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LobbySetSettings, type, editionSelect, roundCount, roundTime, trainingMode);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetChara, type, chara);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetEnv, type, rngSeed);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetStage, type, stageID);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleGgpoFrame, type, src, dest, data);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PunchReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PunchGo, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForwardMessage, type, src, dest, msg);

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