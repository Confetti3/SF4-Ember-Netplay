// What SF4 Ember sends and saves, pinned byte for byte, so that moving its
// game-neutral code into a library shared with SFV Ember can show that none of
// it changed. Two parts:
//
// Private rooms are byte-identical to the revision before server-owned rooms
// (f95cef2), but for the eight tables room protocol 4 gives every room. A
// scripted private room runs from creation to a host transfer; after each step
// the SHA-256 of the room snapshot's JSON and of the authority's Checkpoint()
// JSON are compared with the values the protocol 4 sources produced for this
// same script. `--print` writes the table in the form kGolden below. The
// script uses only API that exists at f95cef2 and fixed times.
//
// The wire fixtures (src/tests/fixtures/wire/<name>.json) are built from fixed
// values: the session protocol's messages (MatchData, PreBattleSetChara,
// LobbyData, StateSnapshot in its battle snapshot, BattleHashV2 and the
// messages that carry them), the room model's JSON the script never writes
// (every optional key, Arcade and Practice rooms, a tournament binding,
// actions, events and results), the settings file the player's preferences
// are saved to, and the overlay's section of it. Each file holds the value
// indented by two spaces, so a change shows as a diff. The test compares that
// text, then the compact bytes DumpJson puts on the wire, and, for a type with
// a reader, reads the file back into its type and checks it writes the same
// bytes again. `--write` rewrites every file from the code as it is; a change
// of these bytes is a change of SF4 Ember's wire or saved format.
#include "../session/sf4e__SessionProtocol.hxx"
#include "../session/SessionRecovery.hxx"
#include "../common/JsonDump.hxx"
#include "../netplay/ProfileRecord.hxx"
#include "../sf4e/sf4e__OverlayPrefs.hxx"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using namespace sf4e::room;
using sf4e::session::recovery_detail::Sha256Portable;

namespace {
struct Step {
	const char* name;
	bool accepted;
	const char* snapshot;
	const char* checkpoint;
};

// From the protocol 4 sources: the same script built against that revision's room model.
const Step kGolden[] = {
	{"create", true,
		"37efd500e41bd6fef88854c5a74e0e2edd36c4ae571f84e2971ff5dedb559f4e",
		"9ab97a2dcb1aa49bdefc1ad789ab41cac2f26b4521a5604f7e55437d2ea22245"},
	{"join host", true,
		"863028c1041f228688c3c2968160a75fcee3a3442649da4d6e804b721333ae90",
		"6ac2ecda8d45a30b09d1d15ed975a4373151c416a666d0b1dcc484ce9c49794d"},
	{"join a", true,
		"200046a139f4a7f3d7b89b1d4a617b2c29b3787c684b06d30fa3671d3e3704ed",
		"c35f183dff1cfc2ae9d97e53da9b1d3bbd631074ba21918250a3c52da0322f37"},
	{"join b", true,
		"4c0db70f467a8f8cf5626c27df91aee3055610061d0a10f3f173beb9352f6c52",
		"963470c632b24a69144c240f6abb8a2b56116685da94b7832e34dbd283731af8"},
	{"join c", true,
		"cb62cfa37da4cb7124fd33651231fbbee18159f123c6a2d1284b76f572313ef2",
		"676300bad022090d3d11654b3bafc74fa4a4a6df159e2b2495b5b77e46bcc834"},
	{"queue a", true,
		"2d45569d03ffced2c48736a50b90ec733074fef14d10ee6c79c6d18ead21841c",
		"8910203ae3346a3cc255b93d37eec28431ff0b3ee90d9f8804b2c957a15e01d5"},
	{"queue b", true,
		"eff8973bca7c5945b6abb1afe17224c0b364b866d007eb0815358eeece0eb189",
		"9b177843bc956fd3d58397956a4be67223fc98381d806005d7e99a2048d85dbf"},
	{"ready a", true,
		"d89543c594864932504c01341314f6219667bb623936fba759e86cac85698cd8",
		"d64dd87dc5d770c84b0890c1d5166eaffa00b46948e4d7362759beb6ed986995"},
	{"ready b", true,
		"7d9eaa1633da24426b28bd15cd9877cc1c42ce234853c5a8aef0d3374319be3f",
		"0290f28c04d4245744149d402ba3a0c1c6f46ee7dcf68227488fd21d7431fefe"},
	{"begin match", true,
		"6b163805e86a85fccc8b4ea1ea00fd3de923327a25980cc55b8529ca7eeaf8b0",
		"de4b8f4cd7bec27b9c849982efb78978c7e58dbdfb9a1a86594584f54761b601"},
	{"advance time", true,
		"6b163805e86a85fccc8b4ea1ea00fd3de923327a25980cc55b8529ca7eeaf8b0",
		"a98c3adadced2c233e6ebfa228bdaec8fd9b85696f994c07e155c97cfea6e5ca"},
	{"result a", true,
		"feb2476ceec69d9f70374d0aa6968133a74ee9ac5d3610f9558d84a3d8ed4e8d",
		"499176e27e6dfb731b6065cd689e217414527b5fab57afa0d159d0c746f1d712"},
	{"result b", true,
		"b0f7f0c35ddf2fb4c9d284100c907de7195b27a119b78fb90878aa85b29c2186",
		"c5f6ebf6935c3b12bbde8e18b311767c88b71cff22c61b053311905ae03700e8"},
	{"end match", false,
		"b0f7f0c35ddf2fb4c9d284100c907de7195b27a119b78fb90878aa85b29c2186",
		"c5f6ebf6935c3b12bbde8e18b311767c88b71cff22c61b053311905ae03700e8"},
	{"acknowledge a", true,
		"23a9c17898be07487114c85f802857b405f5537792078a22b622ea804f8f3923",
		"a4b38d7aa95ee508e1bed98f5a7eca7be548a23804f99c8c8db7c3cbe70f4383"},
	{"acknowledge b", true,
		"35c733e5d42b6cfab36c1811866f48ae885998aebc7ec3bd61e2bc6b0117eee0",
		"46da744f08686a86e8178f573a541a1e7d82c0057ea74aaee690c9358cee33e8"},
	{"chat", true,
		"4549de8bc39e2b6eaec75ba6ecd60e92fc8c0e09b140b43a1d30f48d4e8bdfc8",
		"9b9e938b6c17d557e2e914ad39d1fc49d71b4708e7685ee8669daf657a0fc437"},
	{"kick c", true,
		"14129abe3a1d542d4bcf88df57abb982d1b81db8782089d0062a322aa5b47da7",
		"6280d9d095d9dd0bbb8c6ac33294b8d00c8d189a74506ee691c04dd649f24fc7"},
	{"rejoin kicked", false,
		"14129abe3a1d542d4bcf88df57abb982d1b81db8782089d0062a322aa5b47da7",
		"6280d9d095d9dd0bbb8c6ac33294b8d00c8d189a74506ee691c04dd649f24fc7"},
	{"leave b", true,
		"770bab34a2ff08d4ffed81f6939080fed70d79cbc56616e4b1a680fa71fbeaf5",
		"918738f5664440904784dc9603f1ce588b47f423392d12293e98f1181799885e"},
	{"transfer host", true,
		"4c1591a530b953607234b9a9ca734770524b69e54ffc26c2db7385a49f3f2c16",
		"07a25664fecad6485ae1551ed2b995ea48510f525f5b634b62b930b0fa149745"},
	{"leave host", true,
		"1b707234ff298ff6b3ed5ea62fbb49a7cbdeb37e816e0fda7dbb3234a9f0e87b",
		"17599b30551dfb4ea234a0661bf071858730f9599d306edc54b490def83cd664"},
};

struct Line { std::string name; bool accepted; std::string snapshot, checkpoint; };

std::vector<Line> Run() {
	std::vector<Line> lines;
	RoomAuthority authority("Golden", 8, 11);
	const auto record = [&](const char* name, bool accepted) {
		lines.push_back({name, accepted, Sha256Portable(nlohmann::json(authority.SnapshotCopy()).dump()),
			Sha256Portable(authority.Checkpoint().dump())});
	};
	const auto peer = [](int index) { return ConnectionRef{"host", std::to_string(index)}; };
	const auto act = [&](MemberId member, ActionKind kind) {
		Action action;
		action.kind = kind;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.table = 0;
		action.tableRevision = authority.SnapshotView().tables[0].revision;
		action.actionId = member * 1000 + authority.SnapshotView().revision + 1;
		return action;
	};
	record("create", true);
	const auto join = [&](const char* name, int index, bool host) {
		const auto result = authority.Join(name, peer(index), host);
		record(name, result.accepted);
		return result.accepted ? result.snapshot.members.back().id : MemberId(0);
	};
	const auto host = join("join host", 0, true);
	const auto a = join("join a", 1, false);
	const auto b = join("join b", 2, false);
	const auto c = join("join c", 3, false);
	const auto apply = [&](const char* name, MemberId member, const Action& action) {
		record(name, authority.Apply(member, action).accepted);
	};
	apply("queue a", a, act(a, ActionKind::Queue));
	apply("queue b", b, act(b, ActionKind::Queue));
	apply("ready a", a, act(a, ActionKind::Ready));
	apply("ready b", b, act(b, ActionKind::Ready));
	authority.AdvanceTime(1000);
	const auto begun = authority.BeginMatch(0, a, b);
	record("begin match", begun.accepted);
	authority.AdvanceTime(2000);
	record("advance time", true);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	auto report = act(a, ActionKind::RecordResult);
	report.matchGeneration = generation; report.result = MatchResult::P1Win;
	apply("result a", a, report);
	report = act(b, ActionKind::RecordResult);
	report.matchGeneration = generation; report.result = MatchResult::P1Win;
	apply("result b", b, report);
	record("end match", authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
	for (const auto member : {a, b}) {
		auto ack = act(member, ActionKind::AcknowledgeTerminal);
		ack.matchGeneration = generation;
		apply(member == a ? "acknowledge a" : "acknowledge b", member, ack);
	}
	authority.AdvanceTime(5000);
	auto chat = act(c, ActionKind::Chat);
	chat.text = "hello table";
	apply("chat", c, chat);
	auto kick = act(host, ActionKind::Kick);
	kick.target = c;
	apply("kick c", host, kick);
	record("rejoin kicked", authority.Join("c again", peer(3)).accepted);
	record("leave b", authority.Leave(b).accepted);
	record("transfer host", authority.TransferHost(host, a).accepted);
	record("leave host", authority.Leave(host).accepted);
	return lines;
}

// The wire fixtures.
namespace wire {
namespace protocol = sf4e::SessionProtocol;
namespace netplay = sf4e::netplay;
namespace prefs = sf4e::OverlayPrefs;
using Json = nlohmann::json;
using Chara = sf4e::wire::ConfirmedCharaConditions;

struct Fixture {
	std::string name;
	Json value;
	// The value read back from its JSON into its type and written again; empty
	// for a value with no reader of its own.
	std::function<Json(const Json&)> reread;
	// Set when the value could not be produced.
	std::string error;
};

template<class T> std::function<Json(const Json&)> Reread() {
	return [](const Json& json) { return Json(json.get<T>()); };
}

// Two 64-digit endpoint IDs, as a member's authenticated endpoint and latency route carry them.
std::string Endpoint(char digit) { return std::string(64, digit); }

Chara MakeChara(unsigned char fighter, unsigned char costume, unsigned char color, unsigned char ultra, unsigned char edition) {
	Chara chara{};
	chara.charaID = fighter; chara.costume = costume; chara.color = color; chara._unused = 0;
	chara.personalAction = 3; chara.winQuote = 4; chara.ultraCombo = ultra; chara.handicap = 0; chara.unc_edition = edition;
	return chara;
}

protocol::MatchData FilledMatch() {
	protocol::MatchData match;
	match.readyMessageNum = {{12, 13}};
	match.chara = {{MakeChara(12, 2, 7, 1, 14), MakeChara(31, 0, 3, 2, 3)}};
	match.stageID = 2;
	match.rngSeed = 0x9E3779B9;
	match.inputDelay = {{3, 4}};
	match.lowerInputLag = true;
	return match;
}

protocol::LobbyData FilledLobby() {
	protocol::MemberData first;
	first.connId = {"peer", "1"};
	first.name = "Player 1";
	first.ip = "203.0.113.7";
	first.port = 27015;
	first.flags = protocol::MF_BATTLE_LOADED;
	first.roomMember = 1;
	first.authenticatedEndpoint = Endpoint('a');
	first.incarnation = 2;
	first.latency = sf4e::LatencyRoute::Parsed(Endpoint('a'), "euc1");
	protocol::MemberData second;
	second.connId = {"peer", "2"};
	second.name = "Player 2";
	second.roomMember = 2;
	second.incarnation = 1;
	protocol::LobbyData lobby;
	lobby.id = {"room", "k3y"};
	lobby.editionSelect = false;
	lobby.roundCount = 5;
	lobby.roundTime = {0, 60};
	lobby.trainingMode = true;
	lobby.members = {first, second};
	return lobby;
}

protocol::StateSnapshot::CharaStateSnapshot MakeCharaState(int side, short vit) {
	protocol::StateSnapshot::CharaStateSnapshot chara{};
	chara.status = 0x21 + side;
	// Exact binary fractions, and one that is not, as the game's positions are.
	chara.rootPos[0] = side ? 1.5f : -1.5f; chara.rootPos[1] = -2.25f; chara.rootPos[2] = 0.f; chara.rootPos[3] = 0.1f;
	chara.side = side;
	chara.vit = {0, vit}; chara.vitmax = {0, 1000};
	chara.revenge = {32768, 150}; chara.revengemax = {0, 400};
	chara.recoverable = {16384, 75}; chara.recoverablemax = {0, 1000};
	chara.super = {0, 250}; chara.supermax = {0, 1000};
	chara.sctimeamt = {0, 0}; chara.sctimemax = {0, 0};
	chara.uctime = {0, 0}; chara.uctimemax = {0, 0};
	chara.damage = {8192, 120}; chara.combodamage = {0, 310};
	return chara;
}

Rules MakeRules(SetFormat format, RotationMode rotation, bool editionSelect, std::uint8_t rounds, std::uint16_t time,
	bool instantRematch, bool winnerKeepsFighter) {
	Rules rules;
	rules.format = format; rules.rotation = rotation; rules.editionSelect = editionSelect;
	rules.roundCount = rounds; rules.roundTime = time; rules.instantRematch = instantRematch;
	rules.winnerKeepsFighter = winnerKeepsFighter;
	return rules;
}

Member MakeMember(MemberId id, const char* name, MemberStatus status, int table, int seat) {
	Member member;
	member.id = id;
	member.name = name;
	member.connection = {"peer", std::to_string(id)};
	member.status = status;
	member.table = static_cast<std::int8_t>(table);
	member.seat = static_cast<std::int8_t>(seat);
	member.joinOrder = id;
	return member;
}

// Every member key, every optional table key, and the room's own optional keys
// but the ones only Arcade and Practice rooms write.
Snapshot BoundRoom() {
	Snapshot room;
	room.roomEpoch = 77;
	room.revision = 41;
	room.name = "Player 1's room";
	room.capacity = 12;
	room.serverOwned = true;
	room.delayPolicy = DelayPolicy::Own;
	room.tableCount = 2;
	room.host = 1;
	room.localMember = 2;
	auto first = MakeMember(1, "Player 1", MemberStatus::Ready, 0, 0);
	first.host = true;
	first.incarnation = 3;
	first.fighter = 12; first.edition = 3; first.costume = 2; first.color = 7; first.ultra = 1;
	first.mainFighter = 12;
	first.link = sf4e::NetworkLink::Wired;
	first.nat = sf4e::NatClass::Open;
	first.steamId = sf4e::SteamId::Parsed("76561197960287930");
	first.country = sf4e::CountryCode::Parsed("GB");
	first.badge = sf4e::SkillBadge::OfClass(4, true);
	first.selectedDelay = 3; first.frozenDelay = 3; first.delayLocked = true;
	first.idleSeconds = 42; first.typingMs = 1200;
	first.setsWon = 2; first.setsLost = 1;
	auto second = MakeMember(2, "Player 2", MemberStatus::Ready, 0, 1);
	second.fighter = 31; second.edition = 14;
	second.mainFighter = 7;
	second.link = sf4e::NetworkLink::Wireless;
	second.nat = sf4e::NatClass::Strict;
	second.badge = sf4e::SkillBadge::Placing();
	second.selectedDelay = 4; second.frozenDelay = 4; second.delayLocked = true;
	auto third = MakeMember(3, "Player 3", MemberStatus::Queued, 0, -1);
	third.training = true;
	third.badge = sf4e::SkillBadge::Developer();
	third.country = sf4e::CountryCode::Parsed("BR");
	auto fourth = MakeMember(4, "Player 4", MemberStatus::Watching, 0, -1);
	fourth.spectatorLocked = true;
	auto fifth = MakeMember(5, "Player 5", MemberStatus::WatchingNext, 1, -1);
	room.members = {first, second, third, fourth, fifth};
	for (std::size_t i = 0; i < TableCount; ++i) room.tables[i].id = static_cast<std::uint8_t>(i);
	auto& table = room.tables[0];
	table.rules = MakeRules(SetFormat::Ft3, RotationMode::LoserStays, false, 5, 60, false, true);
	table.name = "Table one";
	table.phase = TablePhase::Ready;
	table.revision = 7;
	table.matchGeneration = 4;
	table.p1 = 1; table.p2 = 2;
	table.queue = {3};
	table.spectators = {4};
	table.endingWatchers = {4};
	table.ready[0] = table.ready[1] = true;
	table.inputDelay[0] = 3; table.inputDelay[1] = 4;
	table.lowerLag[0] = true;
	table.score[0] = 1; table.score[1] = 2;
	table.lastSet.generation = 3; table.lastSet.p1 = 1; table.lastSet.p2 = 2;
	table.lastSet.score[0] = 3; table.lastSet.score[1] = 1; table.lastSet.winnerSeat = 0;
	table.streakHolder = 1; table.streak = 2;
	table.keeper = 1; table.keptFighter = 12; table.keptEdition = 3;
	table.spectatorHold = true; table.holdRemainingMs = 2500;
	table.readyRemainingMs = 30000;
	table.permitGeneration = 5;
	table.permits = {{"per_alpha", "per_beta"}};
	table.permitWindows = {{120000, 90000}};
	auto& next = room.tables[1];
	next.rules = MakeRules(SetFormat::Ft10, RotationMode::BothRotate, true, 1, 9999, true, false);
	next.phase = TablePhase::Playing;
	next.revision = 2;
	next.matchGeneration = 1;
	next.watchingNext = {5};
	room.terminalPending[1] = true;
	room.localTerminalPending = true;
	room.localTerminalGenerations[1] = 1;
	room.localMatchGenerationsSent = true;
	room.localMatchGenerations[0] = 4;
	room.chat = {{1, 1, "gg"}, {2, 3, "Caf\xC3\xA9 rematch?"}};
	auto& binding = room.tournament;
	binding.matchId = "match-1";
	binding.assignmentGeneration = 3;
	binding.bindingRevision = 2;
	binding.gamesToWin = 3;
	binding.fighters = {{{Endpoint('a'), "player1"}, {Endpoint('b'), "player2"}}};
	binding.startingWins = {{1, 0}};
	binding.watchers = {TournamentWatcher{Endpoint('c'), "player3"}};
	return room;
}

// A King of the hill room: its cabinets, streaks and top slot, and an Arcade pair deciding on a rematch.
Snapshot KingOfTheHillRoom() {
	Snapshot room;
	room.roomEpoch = 78;
	room.revision = 9;
	room.name = "King of the hill";
	room.capacity = 16;
	room.serverOwned = true;
	room.arcade = sf4e::room::KingRoom;
	room.host = 1;
	room.localMember = 3;
	room.members = {MakeMember(1, "Player 1", MemberStatus::Seated, 0, 0), MakeMember(2, "Player 2", MemberStatus::Seated, 0, 1),
		MakeMember(3, "Player 3", MemberStatus::Seated, 1, 0)};
	room.cabinets[0] = 1; room.cabinets[1] = 2; room.cabinets[2] = 3;
	room.streaks[0] = 3;
	room.topSlot = 0;
	for (std::size_t i = 0; i < TableCount; ++i) {
		room.tables[i].id = static_cast<std::uint8_t>(i);
		room.tables[i].rules = MakeRules(SetFormat::Ft2, RotationMode::WinnerStays, true, 3, 99, true, true);
	}
	auto& pair = room.tables[0];
	pair.phase = TablePhase::Waiting;
	pair.p1 = 1; pair.p2 = 2;
	pair.deciding = true;
	pair.rematch[0] = true;
	auto& alone = room.tables[1];
	alone.phase = TablePhase::Waiting;
	alone.p1 = 3;
	alone.waitingSince = 1234;
	return room;
}

// A private Practice room: Shared delay, its tables at the default count.
Snapshot PracticeRoom() {
	Snapshot room;
	room.roomEpoch = 79;
	room.revision = 1;
	room.name = "Practice";
	room.capacity = 4;
	room.practice = true;
	room.host = 1;
	room.localMember = 1;
	auto member = MakeMember(1, "Player 1", MemberStatus::Idle, -1, -1);
	member.host = true;
	room.members = {member};
	for (std::size_t i = 0; i < TableCount; ++i) room.tables[i].id = static_cast<std::uint8_t>(i);
	return room;
}

Action MakeAction(ActionKind kind) {
	Action action;
	action.kind = kind;
	action.roomEpoch = 77;
	action.revision = 41;
	action.tableRevision = 7;
	action.actionId = 2042;
	action.table = 0;
	return action;
}

std::vector<Fixture> ProtocolFixtures() {
	std::vector<Fixture> fixtures;
	const auto add = [&](const char* name, Json value, std::function<Json(const Json&)> reread) {
		fixtures.push_back({name, std::move(value), std::move(reread), {}});
	};
	add("protocol_match_data_default", Json(protocol::MatchData()), Reread<protocol::MatchData>());
	add("protocol_match_data", Json(FilledMatch()), Reread<protocol::MatchData>());
	protocol::PreBattleSetChara setChara;
	setChara.chara = MakeChara(12, 2, 7, 1, 14);
	add("protocol_prebattle_setchara", Json(setChara), Reread<protocol::PreBattleSetChara>());
	add("protocol_lobby_data_null", Json(protocol::LobbyData::NULL_LOBBY), Reread<protocol::LobbyData>());
	add("protocol_lobby_data", Json(FilledLobby()), Reread<protocol::LobbyData>());
	protocol::LobbySetSettings settings;
	settings.editionSelect = false; settings.roundCount = 5; settings.roundTime = {0, 60};
	add("protocol_lobby_setsettings", Json(settings), Reread<protocol::LobbySetSettings>());
	protocol::SessionDataUpdate update;
	update.lobbyData = FilledLobby();
	update.matchData = FilledMatch();
	update.matchGeneration = 9; update.authorityTerm = 2; update.authorityRevision = 41;
	add("protocol_session_dataupdate", Json(update), Reread<protocol::SessionDataUpdate>());
	protocol::BattleSnapshot snapshot;
	snapshot.snapshot.frameIdx = 3600;
	snapshot.snapshot.chara[0] = MakeCharaState(0, 640);
	snapshot.snapshot.chara[1] = MakeCharaState(1, 1000);
	add("protocol_battle_snapshot", Json(snapshot), Reread<protocol::BattleSnapshot>());
	add("protocol_battle_hash_default", Json(protocol::BattleHashV2()), Reread<protocol::BattleHashV2>());
	protocol::BattleHashV2 hash;
	hash.frameIdx = 3540;
	hash.overall = 0xFFFFFFFFFFFFFFFFull;
	hash.flow = 0x0123456789ABCDEFull;
	hash.chara0 = 0x8000000000000001ull;
	hash.chara1 = 42;
	hash.fromPlayer = true;
	add("protocol_battle_hash", Json(hash), Reread<protocol::BattleHashV2>());
	protocol::SessionJoinRequest join;
	join.sidecarHash = std::string(64, 'f');
	join.username = "Player 1";
	join.port = 27015;
	join.customRooms = true;
	join.roomProtocol = ProtocolVersion;
	join.mainFighter = 12;
	join.link = static_cast<int>(sf4e::NetworkLink::Wired);
	join.nat = static_cast<int>(sf4e::NatClass::Open);
	join.roomChatDelta = true;
	add("protocol_join_request_minimal", Json(join), Reread<protocol::SessionJoinRequest>());
	join.steamId = "76561197960287930";
	join.country = "GB";
	join.observer = true;
	join.latency = sf4e::LatencyRoute::Parsed(Endpoint('a'), "use1");
	add("protocol_join_request", Json(join), Reread<protocol::SessionJoinRequest>());
	protocol::RoomSnapshotMessage roomSnapshot;
	roomSnapshot.snapshot = BoundRoom();
	roomSnapshot.chatUnchanged = true;
	add("protocol_room_snapshot_chat_unchanged", Json(roomSnapshot), Reread<protocol::RoomSnapshotMessage>());
	protocol::RoomResultMessage roomResult;
	roomResult.result.accepted = true;
	roomResult.result.snapshot = PracticeRoom();
	roomResult.actionId = 2042;
	add("protocol_room_result", Json(roomResult), Reread<protocol::RoomResultMessage>());
	return fixtures;
}

std::vector<Fixture> RoomFixtures() {
	std::vector<Fixture> fixtures;
	const auto add = [&](const char* name, Json value, std::function<Json(const Json&)> reread) {
		fixtures.push_back({name, std::move(value), std::move(reread), {}});
	};
	// A default snapshot has no room epoch, which no reader takes.
	add("room_snapshot_default", Json(Snapshot()), nullptr);
	add("room_snapshot_bound", Json(BoundRoom()), Reread<Snapshot>());
	add("room_snapshot_king", Json(KingOfTheHillRoom()), Reread<Snapshot>());
	add("room_snapshot_practice", Json(PracticeRoom()), Reread<Snapshot>());

	auto ready = MakeAction(ActionKind::Ready);
	ready.seat = 0;
	ready.inputDelay = 4;
	ready.lowerLag = true;
	add("room_action_ready", Json(ready), Reread<Action>());
	auto result = MakeAction(ActionKind::RecordResult);
	result.matchGeneration = 6; result.result = MatchResult::P2Win; result.game = 2; result.continued = true;
	add("room_action_record_result", Json(result), Reread<Action>());
	auto permit = MakeAction(ActionKind::PermitReady);
	permit.matchGeneration = 6; permit.text = "per_alpha"; permit.startWindowMs = 120000;
	add("room_action_permit_ready", Json(permit), Reread<Action>());
	// SetTraining and Typing send their flag as "locked".
	auto training = MakeAction(ActionKind::SetTraining);
	training.inTraining = true;
	add("room_action_set_training", Json(training), Reread<Action>());
	auto typing = MakeAction(ActionKind::Typing);
	typing.typing = true;
	add("room_action_typing", Json(typing), Reread<Action>());
	auto lock = MakeAction(ActionKind::Lock);
	lock.locked = true;
	add("room_action_lock", Json(lock), Reread<Action>());
	auto unwatch = MakeAction(ActionKind::Unwatch);
	unwatch.matchGeneration = 6; unwatch.keepWatching = true;
	add("room_action_unwatch", Json(unwatch), Reread<Action>());
	auto roomType = MakeAction(ActionKind::SetRoomType);
	roomType.roomType = sf4e::room::KingRoom;
	roomType.rules = MakeRules(SetFormat::Ft2, RotationMode::WinnerStays, true, 3, 99, true, true);
	add("room_action_set_room_type", Json(roomType), Reread<Action>());
	auto tables = MakeAction(ActionKind::SetTableCount);
	tables.tableCount = 6;
	add("room_action_set_table_count", Json(tables), Reread<Action>());
	auto chat = MakeAction(ActionKind::Chat);
	chat.text = "Caf\xC3\xA9 rematch?";
	add("room_action_chat", Json(chat), Reread<Action>());

	Event ended;
	ended.kind = Event::Kind::MatchEnded;
	ended.table = 2; ended.matchGeneration = 5; ended.member = 3; ended.result = MatchResult::P2Win; ended.terminalReplay = true;
	add("room_event", Json(ended), Reread<Event>());
	Result refused;
	refused.reason = RejectReason::StaleTable;
	refused.terminalReplay = true;
	refused.snapshot = PracticeRoom();
	refused.events = {ended, Event()};
	add("room_result", Json(refused), Reread<Result>());
	return fixtures;
}

netplay::PlayerPreferences FilledPreferences() {
	netplay::PlayerPreferences value;
	value.displayName = "Player 1";
	value.mainFighter = 12;
	value.record.wins = 10; value.record.losses = 4;
	netplay::RandomRoomId roomId{};
	for (std::size_t i = 0; i < roomId.size(); ++i) roomId[i] = static_cast<std::uint8_t>(0x10 + i);
	value.record.recent = {netplay::ProfileResultKeyV2(roomId, 1, 7), netplay::ProfileResultKeyV2(roomId, 1, 8, 2)};
	value.inputDelay = 3; value.autoInputDelay = false; value.autoDelayStyle = 1;
	value.showMatchHud = false; value.matchHudSize = 2; value.matchHudRaised = true; value.showShortcutHint = false;
	value.matchHudAnchor = 5; value.pictureSpot = 7;
	value.wornTitle = "veteran"; value.wornIcon = "flame";
	value.roomColumn = 44; value.roomSplit = 62;
	value.matchHudLayout = 0; value.matchHudNameOffset = -12;
	value.readySound = false; value.classEmblems = false; value.tvMode = true; value.welcomed = false; value.trainingAutoReady = true;
	value.pictureMode = netplay::PictureBig;
	value.roomShowMembers = false; value.roomTableGrid = true; value.roomCharacterPictures = false; value.chatFilter = false;
	value.matchFrameMeter = true; value.readySoundVolume = 70;
	value.backgroundPlay = true; value.endlessLobbyMusic = false; value.lowerInputLag = true; value.lowerKeyboardLag = true;
	value.fasterDisplay = false; value.borderless = true;
	value.buttonPrompts = sf4e::gamepads::ButtonPrompts::PlayStation;
	value.controllerLayer = false;
	value.controllerMappings = {{"03000000de280000ff11000000000000", "a:b0,b:b1,x:b2,y:b3"}};
	sf4e::gamepads::StoredLayout layout;
	layout.identity = {"03000000de280000ff11000000000000", "SN-0001", "\\\\?\\hid#vid_28de&pid_11ff", "Pad 1"};
	layout.buttons = {0x10, 0x20, 0x400, 0x40, 0x80, 0x800, 0x430, 0x8C0, 0, 0};
	layout.edited = 1767225600;
	layout.bound = {0, 1, 4, 5};
	layout.extras = {-1, 10, 11, 2, -1, -1, -1};
	value.buttonLayouts = {layout};
	value.recordWatched = false;
	value.exportNames = false; value.exportSet = false; value.exportMeter = true;
	value.shareReplays = true; value.shareReplaysSince = 1767225600;
	value.shareSteamProfile = false;
	value.country = sf4e::CountryPreference::Of(sf4e::CountryCode::Parsed("GB"));
	value.discordPresence = false;
	value.sendProblemReports = true;
	value.interfaceScale = 1.25f;
	value.speech.enabled = true; value.speech.rate = -3; value.speech.volume = 80;
	value.speech.details = value.speech.hints = value.speech.position = true; value.speech.chime = false;
	value.speech.chimeVolume = 40; value.speech.introHeard = true; value.speech.tick = false;
	value.speech.describeUsed = 2; value.speech.remind = netplay::SpeechPreferences::Always;
	value.lobby.editionSelect = false; value.lobby.roundCount = 5; value.lobby.roundTime = 60;
	value.roomName = "Practice hall";
	value.roomCapacity = 8;
	value.roomPublic = true;
	value.roomDelayPolicy = DelayPolicy::Own;
	value.roomTableCount = 2;
	value.tableRules = MakeRules(SetFormat::Ft5, RotationMode::BothRotate, false, 1, 300, false, true);
	value.publicTableRules = MakeRules(SetFormat::Ft3, RotationMode::WinnerStays, true, 3, 99, true, false);
	netplay::RoomTemplate saved;
	saved.name = "Evening sets";
	saved.roomName = "First to ten";
	saved.capacity = 6;
	saved.practice = true;
	saved.delayPolicy = DelayPolicy::Own;
	saved.tableCount = 3;
	saved.tables = netplay::TableSetup::Same(MakeRules(SetFormat::Ft10, RotationMode::WinnerStays, true, 3, 99, true, false));
	saved.tables.names[0] = "Main";
	saved.tables.names[2] = "Side";
	value.roomTemplates = {saved};
	return value;
}

// The settings document one save of `preferences` writes into an empty
// settings folder, through the overlay's own save (OverlayPrefs::QueuePlayerPreferences).
Fixture SavedPreferences(const char* name, const netplay::PlayerPreferences& preferences, const std::filesystem::path& folder) {
	Fixture fixture{name, Json(), nullptr, {}};
	if (!preferences.Valid()) { fixture.error = "the fixture's preferences are not valid"; return fixture; }
	std::error_code ignored;
	std::filesystem::remove_all(folder, ignored);
	std::filesystem::create_directories(folder, ignored);
	prefs::UseDirectoryForTest(folder.wstring());
	prefs::StartPersistence();
	const auto revision = prefs::QueuePlayerPreferences(preferences);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (revision && !prefs::PlayerPreferencesSaved(revision) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	const bool saved = revision && prefs::PlayerPreferencesSaved(revision);
	const auto persistenceError = prefs::PersistenceError();
	prefs::StopPersistence();
	if (!saved) { fixture.error = "the preferences were not saved: " + persistenceError; return fixture; }
	std::ifstream file(folder / "settings.json", std::ios::binary);
	std::stringstream bytes;
	bytes << file.rdbuf();
	try { fixture.value = Json::parse(bytes.str()); }
	catch (const std::exception& error) { fixture.error = std::string("settings.json does not parse: ") + error.what(); }
	return fixture;
}

prefs::Data FilledOverlay() {
	prefs::Data data;
	data.lobby.charaID = 12; data.lobby.costume = 2; data.lobby.color = 7; data.lobby.personalAction = 3;
	data.lobby.winQuote = 4; data.lobby.ultraCombo = 1; data.lobby.handicap = 0; data.lobby.unc_edition = 3;
	data.lobbyRandom = true;
	data.fighters[12] = data.lobby;
	data.fighters[12].unc_edition = 14;
	data.fighters[31].costume = 5; data.fighters[31].color = 9; data.fighters[31].ultraCombo = 2;
	data.trainingDummy.charaID = 31; data.trainingDummy.color = 1; data.trainingDummy.ultraCombo = 2;
	data.stageID = 2;
	data.randomPool = sf4e::selection::RandomPool(std::vector<std::int64_t>{1, 3});
	data.lobbyRoundCountIdx = 3; data.lobbyRoundTimeIdx = 0; data.lobbyEditionSelect = false;
	data.deviceIdx = 1; data.deviceType = 2;
	data.deviceIdentity = sf4e::gamepads::Identity{"03000000de280000ff11000000000000", "SN-0001", "\\\\?\\hid#vid_28de&pid_11ff", "Pad 1"};
	data.controllerLayer = false;
	return data;
}

std::vector<Fixture> PreferenceFixtures(const std::filesystem::path& scratch) {
	std::vector<Fixture> fixtures;
	fixtures.push_back(SavedPreferences("player_preferences_default", netplay::PlayerPreferences(), scratch / "default"));
	fixtures.push_back(SavedPreferences("player_preferences", FilledPreferences(), scratch / "filled"));
	std::error_code ignored;
	std::filesystem::remove_all(scratch, ignored);
	const auto overlay = [](const Json& json) { prefs::Data data; prefs::FromJson(json, data); return prefs::ToJson(data); };
	fixtures.push_back({"overlay_prefs_default", prefs::ToJson(prefs::Data()), overlay, {}});
	fixtures.push_back({"overlay_prefs", prefs::ToJson(FilledOverlay()), overlay, {}});
	return fixtures;
}

std::vector<Fixture> All() {
	auto fixtures = ProtocolFixtures();
	for (auto& fixture : RoomFixtures()) fixtures.push_back(std::move(fixture));
	const auto scratch = std::filesystem::temp_directory_path() / ("sf4e-wire-golden-" + std::to_string(GetCurrentProcessId()));
	for (auto& fixture : PreferenceFixtures(scratch)) fixtures.push_back(std::move(fixture));
	return fixtures;
}

// The form a fixture file holds.
std::string Indented(const Json& value) { return value.dump(2) + "\n"; }

std::filesystem::path FixturePath(const std::string& name) {
	return std::filesystem::path(SF4E_WIRE_FIXTURES) / (name + ".json");
}

// The file's text with any carriage returns a Windows checkout added; JSON
// keeps a carriage return in a string escaped, so none is part of a value.
bool ReadFixture(const std::string& name, std::string& text) {
	std::ifstream file(FixturePath(name), std::ios::binary);
	if (!file) return false;
	std::stringstream bytes;
	bytes << file.rdbuf();
	text.clear();
	for (const char c : bytes.str()) if (c != '\r') text.push_back(c);
	return true;
}

// The first line where `expected` and `actual` differ, with both versions.
void ShowDifference(const std::string& expected, const std::string& actual) {
	std::istringstream left(expected), right(actual);
	std::string a, b;
	for (int line = 1;; ++line) {
		const bool moreLeft = static_cast<bool>(std::getline(left, a)), moreRight = static_cast<bool>(std::getline(right, b));
		if (!moreLeft && !moreRight) return;
		if (!moreLeft || !moreRight || a != b) {
			std::printf("  line %d\n    expected: %s\n    actual:   %s\n", line, moreLeft ? a.c_str() : "(end)", moreRight ? b.c_str() : "(end)");
			return;
		}
	}
}

int Write(const std::vector<Fixture>& fixtures) {
	int failures = 0;
	std::filesystem::create_directories(FixturePath("x").parent_path());
	for (const auto& fixture : fixtures) {
		if (!fixture.error.empty()) { std::printf("FAIL wire %s: %s\n", fixture.name.c_str(), fixture.error.c_str()); ++failures; continue; }
		std::ofstream file(FixturePath(fixture.name), std::ios::binary | std::ios::trunc);
		file << Indented(fixture.value);
		if (!file) { std::printf("FAIL wire %s: cannot write %s\n", fixture.name.c_str(), FixturePath(fixture.name).string().c_str()); ++failures; }
	}
	if (!failures) std::printf("Wrote %zu wire fixtures to %s\n", fixtures.size(), FixturePath("x").parent_path().string().c_str());
	return failures ? 1 : 0;
}

int Check(const std::vector<Fixture>& fixtures) {
	int failures = 0;
	for (const auto& fixture : fixtures) {
		const char* name = fixture.name.c_str();
		if (!fixture.error.empty()) { std::printf("FAIL wire %s: %s\n", name, fixture.error.c_str()); ++failures; continue; }
		std::string expected;
		if (!ReadFixture(fixture.name, expected)) { std::printf("FAIL wire %s: no fixture file %s\n", name, FixturePath(fixture.name).string().c_str()); ++failures; continue; }
		const auto actual = Indented(fixture.value);
		if (actual != expected) {
			std::printf("FAIL wire %s: the JSON differs from its fixture\n", name);
			ShowDifference(expected, actual);
			++failures;
			continue;
		}
		// The compact bytes sent and saved. They follow from the indented
		// text, so this holds whenever the comparison above does; it is here
		// so the bytes themselves are what the test names.
		Json stored;
		try { stored = Json::parse(expected); }
		catch (const std::exception& error) { std::printf("FAIL wire %s: the fixture does not parse: %s\n", name, error.what()); ++failures; continue; }
		const auto bytes = sf4e::DumpJson(fixture.value);
		if (sf4e::DumpJson(stored) != bytes) { std::printf("FAIL wire %s: the compact bytes differ\n", name); ++failures; continue; }
		if (!fixture.reread) continue;
		try {
			const auto again = sf4e::DumpJson(fixture.reread(stored));
			if (again != bytes) {
				std::printf("FAIL wire %s: read back and written again, the bytes differ\n", name);
				ShowDifference(Indented(stored), Indented(Json::parse(again)));
				++failures;
			}
		} catch (const std::exception& error) {
			std::printf("FAIL wire %s: the fixture does not read back: %s\n", name, error.what());
			++failures;
		}
	}
	if (failures) std::printf("%d wire fixture failures; if the change is intended, run with --write and review the diff\n", failures);
	return failures;
}
}
}

int main(int argc, char** argv) {
	if (argc > 1 && !std::strcmp(argv[1], "--write")) return wire::Write(wire::All());
	const auto lines = Run();
	if (argc > 1 && !std::strcmp(argv[1], "--print")) {
		for (const auto& line : lines)
			std::printf("\t{\"%s\", %s,\n\t\t\"%s\",\n\t\t\"%s\"},\n", line.name.c_str(), line.accepted ? "true" : "false",
				line.snapshot.c_str(), line.checkpoint.c_str());
		return 0;
	}
	int failures = 0;
	const auto count = sizeof(kGolden) / sizeof(kGolden[0]);
	if (lines.size() != count) { std::printf("FAIL: %zu steps, %zu expected\n", lines.size(), count); ++failures; }
	for (std::size_t i = 0; i < lines.size() && i < count; ++i) {
		const auto& golden = kGolden[i];
		if (lines[i].name != golden.name || lines[i].accepted != golden.accepted) {
			std::printf("FAIL step %zu (%s): outcome %d, expected %s %d\n", i, lines[i].name.c_str(), lines[i].accepted, golden.name, golden.accepted);
			++failures;
		}
		if (lines[i].snapshot != golden.snapshot) { std::printf("FAIL step %zu (%s): snapshot JSON differs\n", i, golden.name); ++failures; }
		if (lines[i].checkpoint != golden.checkpoint) { std::printf("FAIL step %zu (%s): checkpoint JSON differs\n", i, golden.name); ++failures; }
	}
	failures += wire::Check(wire::All());
	if (failures) return 1;
	std::puts("RoomPrivateGolden test passed");
	return 0;
}
