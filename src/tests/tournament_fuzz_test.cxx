// Seeded fuzzing of what tournament play reads from outside: the helper's
// answers, browser links, the state machine under any order of answers, and
// a bound room under any action from any member. Each part runs a fixed
// number of rounds from a fixed seed, so a failure reproduces; SF4E_FUZZ_SEED
// and SF4E_FUZZ_ROUNDS widen a local run.
#include "../common/TournamentLink.hxx"
#include "../netplay/TournamentPlay.hxx"
#include "../session/TournamentAnswers.hxx"
#include "../session/RoomModel.hxx"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace sf4e;
using nlohmann::json;
using netplay::tournament::Output;
using Kind = Output::Kind;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); if (++failures > 20) std::exit(1); } } while (false)

// SplitMix64: small, fast, and the same everywhere.
struct Random {
	std::uint64_t state;
	explicit Random(std::uint64_t seed) : state(seed) {}
	std::uint64_t Next() {
		std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	std::uint64_t Below(std::uint64_t bound) { return bound ? Next() % bound : 0; }
	bool Chance(unsigned percent) { return Below(100) < percent; }
	template <class T> const T& Pick(const std::vector<T>& items) { return items[Below(items.size())]; }
};

static std::uint64_t Seed() {
	const char* text = std::getenv("SF4E_FUZZ_SEED");
	return text ? std::strtoull(text, nullptr, 10) : 0x5EED7041ull;
}
static int Rounds(int fallback) {
	const char* text = std::getenv("SF4E_FUZZ_ROUNDS");
	const int rounds = text ? std::atoi(text) : 0;
	return rounds > 0 ? rounds : fallback;
}

static const std::string Hex32 = "0123456789abcdef0123456789abcdef";
static const std::string EndpointA(64, 'a'), EndpointB(64, 'b'), EndpointC(64, 'c');

// Strings that sit near the edges the decoders check.
static std::string Text(Random& random) {
	static const std::vector<std::string> pool = {
		"", "0", "00", "1", "18446744073709551615", "18446744073709551616", "-1", "1e3", " 1", "host", "wait", "room",
		"pending", "permitted", Hex32, Hex32 + "0", "0123456789ABCDEF0123456789ABCDEF", "per_x", "per_", "emt_x",
		"emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12", EndpointA, EndpointA.substr(1), "sf4e3:x", std::string(5000, 'x'),
		"ember-room-v1", "organizer-reported-v1", "completed", "\xff\xfe", std::string(1, '\0'),
	};
	if (random.Chance(70)) return random.Pick(pool);
	std::string text(random.Below(80), ' ');
	for (auto& c : text) c = static_cast<char>(random.Below(256));
	return text;
}

static json Value(Random& random, int depth = 0) {
	switch (random.Below(depth > 3 ? 6 : 9)) {
	case 0: return nullptr;
	case 1: return random.Chance(50);
	case 2: return static_cast<std::int64_t>(random.Next()) >> random.Below(64);
	case 3: return random.Below(10);
	case 4: return static_cast<double>(random.Below(1000)) / 7.0;
	case 5: return Text(random);
	case 6: { json array = json::array(); for (std::uint64_t i = random.Below(4); i; --i) array.push_back(Value(random, depth + 1)); return array; }
	default: {
		static const std::vector<std::string> keys = {"role", "lease_id", "fence", "retry_after", "room_id", "invitation", "binding",
			"state", "permit_id", "match_generation", "assignments", "match_id", "slot", "games_to_win", "wins", "opponent",
			"fingerprint", "native_rules_profile", "provider", "round_label", "assignment_generation", "binding_revision",
			"fighters", "endpoint_id", "ember_id", "local_slot", "op", "request_id", "ok", "reason", "data"};
		json object = json::object();
		for (std::uint64_t i = random.Below(8); i; --i) object[random.Pick(keys)] = Value(random, depth + 1);
		return object;
	}
	}
}

// A well-formed answer, so mutations start close to what decodes.
static json GoodRoom() {
	return {{"role", "room"}, {"room_id", Hex32}, {"invitation", "sf4e3:x"}, {"binding", {
		{"room_id", Hex32}, {"match_id", "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12"}, {"assignment_generation", "1"},
		{"binding_revision", "1"}, {"games_to_win", 2}, {"local_slot", 0},
		{"fighters", json::array({{{"endpoint_id", EndpointA}, {"ember_id", "emb1_a"}}, {{"endpoint_id", EndpointB}, {"ember_id", "emb1_b"}}})}}}};
}

static void Mutate(Random& random, json& value, int depth = 0) {
	if (value.is_object() && !value.empty() && depth < 4 && random.Chance(70)) {
		auto it = value.begin();
		std::advance(it, static_cast<long>(random.Below(value.size())));
		if (random.Chance(20)) { value.erase(it); return; }
		Mutate(random, *it, depth + 1);
		return;
	}
	if (value.is_array() && !value.empty() && depth < 4 && random.Chance(70)) {
		Mutate(random, value[random.Below(value.size())], depth + 1);
		return;
	}
	value = Value(random, 2);
}

static bool IsHex(const std::string& text, std::size_t length) {
	return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Whatever the helper sends, a decoder either refuses it or hands over only
// values the state machine may rely on.
static void FuzzAnswers(Random& random) {
	for (int round = 0, rounds = Rounds(20000); round < rounds; ++round) {
		json data = random.Chance(50) ? GoodRoom() : Value(random);
		for (std::uint64_t i = random.Below(4); i; --i) Mutate(random, data);
		if (const auto claim = session::DecodeClaimReply(data)) {
			if (claim->role == netplay::tournament::ClaimReply::Role::Room) {
				CHECK(IsHex(claim->roomId, 32) && !claim->invitation.empty() && claim->invitation.size() <= 4096);
				if (claim->binding) {
					CHECK(claim->binding->roomId == claim->roomId && claim->binding->room.Valid());
					CHECK(claim->binding->localSlot == 0 || claim->binding->localSlot == 1);
				}
			}
			if (claim->role == netplay::tournament::ClaimReply::Role::Wait) CHECK(claim->retryAfterMs <= 60000);
			if (claim->role == netplay::tournament::ClaimReply::Role::Host) CHECK(!claim->fence.empty());
		}
		if (const auto prepare = session::DecodePrepareReply(data)) {
			if (prepare->permitted) CHECK(prepare->generation && room::ValidPermitId(prepare->permitId));
			else CHECK(prepare->retryAfterMs <= 60000);
		}
		if (const auto list = session::DecodeAssignments(data)) {
			CHECK(list->size() <= 50);
			for (const auto& item : *list) CHECK((item.slot == 0 || item.slot == 1) && item.gamesToWin <= 5 && item.wins[0] <= 9 && item.wins[1] <= 9);
		}
		json event = {{"op", random.Chance(80) ? "match_claim" : Text(random)}, {"request_id", Value(random)}, {"ok", Value(random)}, {"data", data}};
		if (random.Chance(50)) event["request_id"] = random.Below(100);
		if (random.Chance(50)) event["ok"] = random.Chance(50);
		if (const auto answer = session::ReadTournamentAnswer(event)) {
			CHECK(session::IsTournamentPlayOp(answer->op) && answer->reason.size() <= 64);
		}
	}
}

// A browser or pasted link parses to a canonical bridge and match, or not at all.
static void FuzzLinks(Random& random) {
	const std::string bridge = "brg_0dbc0598-2312-4ce3-9df8-e160330565e6", match = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	const std::string goods[] = { "ember://tournament/open?bridge=" + bridge + "&match=" + match,
		std::string(tournament_link::PagePrefix()) + bridge + "/" + match };
	for (const auto& good : goods) CHECK(tournament_link::ParsePasted(good).Valid());
	int accepted = 0;
	for (int round = 0, rounds = Rounds(50000); round < rounds; ++round) {
		std::string text = goods[random.Below(2)];
		for (std::uint64_t i = 1 + random.Below(3); i; --i) {
			const auto at = random.Below(text.size() + 1);
			switch (random.Below(4)) {
			case 0: if (!text.empty() && at < text.size()) text.erase(at, 1 + random.Below(3)); break;
			case 1: text.insert(at, 1, static_cast<char>(random.Below(256))); break;
			case 2: if (at < text.size()) text[at] = static_cast<char>(random.Below(256)); break;
			default: text.insert(at, random.Pick(std::vector<std::string>{"&", "=", "?", "/", "%2F", "#", " ", "\"", "bridge=", "match=", "&match=" + match})); break;
			}
		}
		const auto parsed = tournament_link::ParsePasted(text);
		if (!parsed.Valid()) continue;
		++accepted;
		CHECK(tournament_link::IsBridgeId(parsed.bridgeId) && tournament_link::IsMatchId(parsed.matchId));
		// What was accepted is exactly a link the bridge could have written.
		const auto again = tournament_link::ParseLink("ember://tournament/open?bridge=" + parsed.bridgeId + "&match=" + parsed.matchId);
		CHECK(again.bridgeId == parsed.bridgeId && again.matchId == parsed.matchId);
		CHECK(text.size() <= tournament_link::MaximumUriLength);
	}
	CHECK(accepted > 0);
}

// The state machine under any order of answers: at most one claim, prepare
// and publish in flight; a bind only to the leader of the bound room; a
// report only for a game with a permit, once; and Forget only after every
// report was answered.
static void FuzzPlay(Random& random) {
	// The rounds reach the parts worth checking.
	int binds = 0, sentReports = 0, forgets = 0;
	for (int game = 0, games = Rounds(400); game < games; ++game) {
		netplay::tournament::TournamentPlay play;
		play.Start("brg_x", "emt_x", 0);
		std::uint64_t now = 0;
		int claims = 0, prepares = 0, publishes = 0, reports = 0;
		std::vector<std::uint64_t> permitted;
		std::vector<std::uint64_t> reported;
		room::Snapshot snapshot;
		netplay::tournament::Binding binding;
		binding.roomId = Hex32;
		binding.localSlot = static_cast<int>(random.Below(2));
		binding.room.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.room.assignmentGeneration = 1;
		binding.room.bindingRevision = 1;
		binding.room.gamesToWin = 2;
		binding.room.fighters[0] = {EndpointA, "emb1_a"};
		binding.room.fighters[1] = {EndpointB, "emb1_b"};
		for (int step = 0; step < 300; ++step) {
			now += random.Below(4000);
			netplay::tournament::RoomView view;
			view.joined = random.Chance(70);
			view.opening = !view.joined && random.Chance(30);
			view.roomId = view.joined ? (random.Chance(80) ? Hex32 : std::string(32, 'f')) : std::string();
			view.invitation = view.joined ? "sf4e3:" + std::to_string(random.Below(3)) : std::string();
			view.authorityWritable = random.Chance(50);
			if (random.Chance(30)) snapshot.tournament = binding.room;
			if (random.Chance(10)) snapshot.tournament = {};
			auto& table = snapshot.tables[room::TournamentTable];
			table.phase = static_cast<room::TablePhase>(random.Below(5));
			table.permitGeneration = random.Chance(50) ? 1 + random.Below(6) : 0;
			table.matchGeneration = random.Chance(50) ? 1 + random.Below(6) : 0;
			table.permits[0] = random.Chance(50) ? "per_a" : "";
			table.permits[1] = random.Chance(50) ? "per_a" : "";
			view.snapshot = view.joined ? &snapshot : nullptr;
			for (const auto& out : play.Tick(now, view)) {
				switch (out.kind) {
				case Kind::Claim: ++claims; CHECK(claims <= 1); break;
				case Kind::Prepare: ++prepares; CHECK(prepares <= 1); break;
				case Kind::Publish: ++publishes; CHECK(publishes <= 1); CHECK(out.roomId == view.roomId); break;
				case Kind::Bind: ++binds; CHECK(view.authorityWritable && view.roomId == binding.roomId); break;
				case Kind::Report:
					++reports;
					++sentReports;
					CHECK(std::find(permitted.begin(), permitted.end(), out.generation) != permitted.end());
					CHECK(std::find(reported.begin(), reported.end(), out.generation) == reported.end());
					reported.push_back(out.generation);
					break;
				case Kind::Forget: ++forgets; CHECK(reports == 0); break;
				default: break;
				}
			}
			// Answer some of what is in flight, in any order, as the helper may.
			if (claims && random.Chance(60)) {
				--claims;
				if (random.Chance(20)) play.OnFailure(Kind::Claim, random.Chance(10) ? "stale_revision" : "bridge_unreachable", now);
				else {
					netplay::tournament::ClaimReply reply;
					reply.role = static_cast<netplay::tournament::ClaimReply::Role>(random.Below(3));
					reply.leaseId = "lse_1";
					reply.fence = "1";
					reply.roomId = Hex32;
					reply.invitation = "sf4e3:0";
					if (random.Chance(60)) reply.binding = binding;
					play.OnRoom(Kind::Claim, reply, now);
				}
			}
			if (publishes && random.Chance(60)) {
				--publishes;
				if (random.Chance(30)) play.OnFailure(Kind::Publish, random.Chance(50) ? "lease_conflict" : "bridge_unreachable", now);
				else {
					netplay::tournament::ClaimReply reply;
					reply.role = netplay::tournament::ClaimReply::Role::Room;
					reply.roomId = Hex32;
					reply.invitation = "sf4e3:0";
					reply.binding = binding;
					play.OnRoom(Kind::Publish, reply, now);
				}
			}
			if (prepares && random.Chance(60)) {
				--prepares;
				netplay::tournament::PrepareReply reply;
				reply.permitted = random.Chance(60);
				reply.generation = table.permitGeneration ? table.permitGeneration : 1;
				reply.permitId = "per_a";
				if (reply.permitted) permitted.push_back(reply.generation);
				if (random.Chance(10)) { play.OnFailure(Kind::Prepare, "stale_revision", now); if (reply.permitted) permitted.pop_back(); }
				else play.OnPrepare(reply, now);
			}
			if (reports && random.Chance(50)) {
				--reports;
				if (random.Chance(20)) play.OnFailure(Kind::Report, "report_not_saved", now);
				else play.OnReported();
			}
			if (random.Chance(10) && table.matchGeneration)
				play.OnTerminal(table.matchGeneration, static_cast<room::MatchResult>(random.Below(5)), random.Below(900), random.Below(900));
			if (random.Chance(1)) play.Stop();
			if (random.Chance(1)) play.Abandon("helper_lost");
		}
	}
	CHECK(binds > 0 && sentReports > 0 && forgets > 0);
}

static room::MemberId JoinAs(room::RoomAuthority& authority, const std::string& endpoint, bool host = false) {
	const auto result = authority.Join("M" + endpoint.substr(0, 2), room::ConnectionRef{"room", endpoint}, host);
	if (!result.accepted) return 0;
	for (const auto& member : result.snapshot.members) if (member.connection.user == endpoint) return member.id;
	return 0;
}

// A bound room under any action from any member, and any binding offered:
// the bound table only ever seats the bound endpoints, nobody else stays in
// the room, a permit hold exists only while the table is Ready, and the room
// survives a checkpoint unchanged.
static void FuzzBoundRoom(Random& random) {
	const std::vector<std::string> endpoints = {EndpointA, EndpointB, EndpointC};
	int started = 0, held = 0, rebound = 0;
	for (int game = 0, games = Rounds(150); game < games; ++game) {
		room::RoomAuthority authority("Match", 8, 1);
		std::uint64_t now = 1000;
		authority.AdvanceTime(now);
		JoinAs(authority, EndpointA, true);
		room::TournamentBinding binding;
		binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.assignmentGeneration = 1;
		binding.bindingRevision = 1;
		binding.gamesToWin = 2;
		binding.fighters[0] = {EndpointA, "emb1_a"};
		binding.fighters[1] = {EndpointB, "emb1_b"};
		CHECK(authority.BindTournament(binding).accepted);
		for (int step = 0; step < 200; ++step) {
			const auto& view = authority.SnapshotView();
			switch (random.Below(9)) {
			case 0: case 8: JoinAs(authority, random.Pick(endpoints)); break;
			// The last member leaving closes the room; a closed room teaches nothing.
			case 1: if (view.members.size() > 1) authority.Leave(view.members[random.Below(view.members.size())].id); break;
			case 2: {
				auto offered = binding;
				offered.bindingRevision = random.Below(4);
				if (random.Chance(30)) offered.fighters[random.Below(2)].endpoint = random.Pick(endpoints);
				if (random.Chance(10)) offered.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a99";
				if (authority.BindTournament(offered).accepted) { binding = authority.SnapshotView().tournament; ++rebound; }
				break;
			}
			case 3: now += random.Chance(10) ? random.Below(room::PermitHoldMs * 2) : random.Below(5000); authority.AdvanceTime(now); break;
			default: {
				if (view.members.empty()) break;
				const auto member = view.members[random.Below(view.members.size())].id;
				// Weighted toward what moves a game along, so games do start.
				static const std::vector<room::ActionKind> kinds = {room::ActionKind::Ready, room::ActionKind::Ready,
					room::ActionKind::Ready, room::ActionKind::PermitReady, room::ActionKind::PermitReady,
					room::ActionKind::PermitReady, room::ActionKind::Unready, room::ActionKind::Queue, room::ActionKind::Unqueue, room::ActionKind::Watch,
					room::ActionKind::SetRules, room::ActionKind::Kick, room::ActionKind::AcknowledgeTerminal};
				room::Action action;
				action.kind = random.Pick(kinds);
				action.roomEpoch = view.roomEpoch;
				action.revision = view.revision;
				action.table = random.Chance(80) ? room::TournamentTable : static_cast<std::uint8_t>(random.Below(room::TableCount));
				action.tableRevision = view.tables[action.table].revision;
				action.actionId = 1 + random.Next() % 1000000;
				action.matchGeneration = random.Chance(70) ? view.tables[action.table].permitGeneration : random.Below(8);
				if (action.kind == room::ActionKind::AcknowledgeTerminal) action.matchGeneration = view.tables[action.table].matchGeneration;
				action.text = random.Chance(85) ? std::string(random.Chance(85) ? "per_a" : "per_b") : Text(random).substr(0, 70);
				action.target = view.members[random.Below(view.members.size())].id;
				// While a start is held, mostly a fighter handing over a permit.
				const auto& bound = view.tables[room::TournamentTable];
				auto actor = member;
				if (bound.permitGeneration && bound.p1 && bound.p2 && random.Chance(60)) {
					actor = random.Chance(50) ? bound.p1 : bound.p2;
					action.kind = room::ActionKind::PermitReady;
					action.table = room::TournamentTable;
					action.matchGeneration = bound.permitGeneration;
					action.text = random.Chance(90) ? "per_a" : "per_b";
				}
				action.rules.format = room::SetFormat::Ft5;
				authority.Apply(actor, action);
				break;
			}
			}
			// The runtime starts a game as soon as the room says it may.
			if (random.Chance(50)) {
				const auto& table = authority.SnapshotView().tables[room::TournamentTable];
				if (table.p1 && table.p2 && table.phase == room::TablePhase::Ready && !room::PermitPending(table) &&
					authority.BeginMatch(room::TournamentTable, table.p1, table.p2).accepted && ++started)
					authority.EndMatch(room::TournamentTable, authority.SnapshotView().tables[room::TournamentTable].matchGeneration,
						static_cast<room::MatchResult>(random.Below(5)));
			}
			const auto& after = authority.SnapshotView();
			if (!after.tournament.Active()) continue;
			for (const auto& member : after.members)
				CHECK(member.connection.user == after.tournament.fighters[0].endpoint || member.connection.user == after.tournament.fighters[1].endpoint);
			const auto& table = after.tables[room::TournamentTable];
			for (int seat = 0; seat < 2; ++seat) {
				const auto id = seat ? table.p2 : table.p1;
				if (!id) continue;
				const auto found = std::find_if(after.members.begin(), after.members.end(), [&](const room::Member& m) { return m.id == id; });
				CHECK(found != after.members.end() && found->connection.user == after.tournament.fighters[seat].endpoint);
			}
			CHECK(table.queue.empty());
			CHECK(table.rules.format == static_cast<room::SetFormat>(after.tournament.gamesToWin));
			if (table.permitGeneration) { ++held; CHECK(table.phase == room::TablePhase::Ready); }
			if (random.Chance(5)) {
				room::RoomAuthority restored("Other", 8, 1);
				CHECK(restored.RestoreCheckpoint(authority.Checkpoint()));
				CHECK(restored.Checkpoint() == authority.Checkpoint());
			}
		}
	}
	CHECK(started > 0 && held > 0 && rebound > 0);
	std::printf("bound room: %d games started, %d steps with a permit hold, %d bindings applied\n", started, held, rebound);
}

int main() {
	// Every play answer reaches the runtime, a match link's too, and not the
	// Ember ID screens.
	for (const char* op : {"match_claim", "room_publish", "game_prepare", "game_report", "match_leave", "assignment_list"})
		CHECK(session::ReadTournamentAnswer({{"op", op}, {"request_id", 7u}, {"ok", true}, {"data", {{"match_id", "emt_x"}}}}).has_value());
	CHECK(!session::ReadTournamentAnswer({{"op", "link_list"}, {"request_id", 7u}, {"ok", true}}).has_value());
	Random random(Seed());
	FuzzAnswers(random);
	FuzzLinks(random);
	FuzzPlay(random);
	FuzzBoundRoom(random);
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("tournament fuzz tests passed\n");
	return failures ? 1 : 0;
}
