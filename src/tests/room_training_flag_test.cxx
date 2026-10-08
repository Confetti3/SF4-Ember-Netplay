// A member's word that they are in the game's Training mode: who may say it,
// what it changes, how it travels, and when it ends.
#include "../session/RoomModel.hxx"

#include <algorithm>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

using namespace sf4e::room;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

struct Room {
	RoomAuthority authority{"Training flag", 8, 41};
	MemberId host = 0, p1 = 0, p2 = 0;

	Room() {
		authority.AdvanceTime(1000);
		host = Join("Host", 0, true);
		p1 = Join("P1", 1); p2 = Join("P2", 2);
	}
	MemberId Join(const char* name, int index, bool isHost = false) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(index)}, isHost);
		CHECK(result.accepted);
		return result.accepted ? result.snapshot.members.back().id : 0;
	}
	// Every action a client sends carries a higher id than its last.
	std::uint64_t nextAction = 1;
	Action Make(MemberId member, ActionKind kind) {
		Action action;
		action.kind = kind;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.tableRevision = authority.SnapshotView().tables[0].revision;
		action.actionId = nextAction++;
		return action;
	}
	Result Apply(MemberId member, ActionKind kind) { return authority.Apply(member, Make(member, kind)); }
	Result Say(MemberId member, bool training) {
		auto action = Make(member, ActionKind::SetTraining);
		action.locked = training;
		return authority.Apply(member, action);
	}
	const Member& member(MemberId id) const {
		const auto& members = authority.SnapshotView().members;
		return *std::find_if(members.begin(), members.end(), [id](const Member& value) { return value.id == id; });
	}
};

int main() {
	{
		// A member says it and takes it back; saying what is already so changes nothing.
		Room room;
		CHECK(!room.member(room.p1).training);
		const auto before = room.authority.SnapshotView().revision;
		CHECK(room.Say(room.p1, true).accepted && room.member(room.p1).training);
		const auto said = room.authority.SnapshotView().revision;
		CHECK(said > before);
		CHECK(room.Say(room.p1, true).accepted && room.authority.SnapshotView().revision == said);
		// It is theirs alone: nobody else is marked, and nothing else about them moves.
		CHECK(!room.member(room.p2).training && !room.member(room.host).training);
		CHECK(room.member(room.p1).status == MemberStatus::Idle && room.member(room.p1).table == -1);
		CHECK(room.Say(room.p1, false).accepted && !room.member(room.p1).training);
		// Somebody who is not in the room cannot say it.
		CHECK(!room.Say(9999, true).accepted);
		CHECK(room.Say(9999, true).reason == RejectReason::UnknownMember);
	}
	{
		// It binds nothing: a queued and seated member in Training still readies, and the table starts.
		Room room;
		CHECK(room.Apply(room.p1, ActionKind::Queue).accepted && room.Apply(room.p2, ActionKind::Queue).accepted);
		CHECK(room.Say(room.p1, true).accepted);
		CHECK(room.member(room.p1).status == MemberStatus::Seated && room.member(room.p1).training);
		CHECK(room.Apply(room.p1, ActionKind::Ready).accepted && room.Apply(room.p2, ActionKind::Ready).accepted);
		CHECK(room.member(room.p1).training);
		// A game of theirs starting ends it without their word.
		CHECK(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
		CHECK(room.member(room.p1).status == MemberStatus::Playing && !room.member(room.p1).training);
		// And the game ending does not bring it back.
		const auto generation = room.authority.SnapshotView().tables[0].matchGeneration;
		CHECK(room.authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
		CHECK(!room.member(room.p1).training);
	}
	{
		// On the wire: written only when set, read as off when absent, and in every member's own view.
		Room room;
		nlohmann::json plain = room.authority.SnapshotFor(room.p2);
		for (const auto& member : plain.at("members")) CHECK(!member.contains("training"));
		CHECK(room.Say(room.p1, true).accepted);
		nlohmann::json marked = room.authority.SnapshotFor(room.p2);
		int written = 0;
		for (const auto& member : marked.at("members")) written += member.contains("training") && member.at("training").get<bool>();
		CHECK(written == 1);
		const Snapshot read = marked.get<Snapshot>();
		const auto* seen = FindMember(read, room.p1);
		CHECK(seen && seen->training);
		// A state written before the flag existed reads as before.
		const Snapshot old = plain.get<Snapshot>();
		for (const auto& member : old.members) CHECK(!member.training);
		// The action goes over the wire and back as itself.
		auto action = room.Make(room.p1, ActionKind::SetTraining);
		action.locked = true;
		const Action back = nlohmann::json(action).get<Action>();
		CHECK(back.kind == ActionKind::SetTraining && back.locked && back.actionId == action.actionId);
		// A kind past the last known one is still refused.
		nlohmann::json unknown = action;
		unknown["kind"] = static_cast<int>(ActionKind::SetTraining) + 1;
		bool refused = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { refused = true; }
		CHECK(refused);
	}
	{
		// A member who leaves takes it with them; one who rejoins starts without it.
		Room room;
		CHECK(room.Say(room.p1, true).accepted);
		CHECK(room.authority.Leave(room.p1).accepted);
		const auto again = room.Join("P1", 1);
		CHECK(again && !room.member(again).training);
	}
	{
		// The table rule: the host sets it with the other rules, every member's view carries it,
		// it is written only when set, and rules written before it existed read as before.
		Room room;
		CHECK(!room.authority.SnapshotView().tables[0].rules.training);
		nlohmann::json plain = room.authority.SnapshotFor(room.p2);
		CHECK(!plain.at("tables").at(0).at("rules").contains("training"));
		auto set = room.Make(room.host, ActionKind::SetRules);
		set.rules = room.authority.SnapshotView().tables[0].rules;
		set.rules.training = true; set.rules.roundTime = 9999;
		CHECK(!room.authority.Apply(room.p1, set).accepted);
		set = room.Make(room.host, ActionKind::SetRules);
		set.rules = room.authority.SnapshotView().tables[0].rules;
		set.rules.training = true; set.rules.roundTime = 9999;
		CHECK(room.authority.Apply(room.host, set).accepted);
		CHECK(room.authority.SnapshotView().tables[0].rules.training && !room.authority.SnapshotView().tables[1].rules.training);
		nlohmann::json marked = room.authority.SnapshotFor(room.p2);
		CHECK(marked.at("tables").at(0).at("rules").at("training").get<bool>());
		const Snapshot read = marked.get<Snapshot>();
		CHECK(read.tables[0].rules.training && read.tables[0].rules.roundTime == 9999 && !read.tables[1].rules.training);
		CHECK(!plain.get<Snapshot>().tables[0].rules.training);
		// The action carries it over the wire, and a value that is no boolean is refused.
		const Action back = nlohmann::json(set).get<Action>();
		CHECK(back.rules.training && back.rules == set.rules);
		Rules other = set.rules; other.training = false;
		CHECK(!(other == set.rules));
		nlohmann::json bad = nlohmann::json(set.rules);
		bad["training"] = 1;
		bool refused = false;
		try { (void)bad.get<Rules>(); } catch (const std::exception&) { refused = true; }
		CHECK(refused);
		// It still plays as a table: both ready and the game begins under the rule.
		CHECK(room.Apply(room.p1, ActionKind::Queue).accepted && room.Apply(room.p2, ActionKind::Queue).accepted);
		CHECK(room.Apply(room.p1, ActionKind::Ready).accepted && room.Apply(room.p2, ActionKind::Ready).accepted);
		CHECK(room.authority.BeginMatch(0, room.p1, room.p2).accepted && room.authority.SnapshotView().tables[0].rules.training);
	}
	if (failures) { std::printf("%d failures\n", failures); return 1; }
	std::printf("Room training flag rules passed.\n");
	return 0;
}
