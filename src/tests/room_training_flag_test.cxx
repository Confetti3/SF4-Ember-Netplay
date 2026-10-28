// A member's word that they are in the game's Training mode: who may say it,
// what it changes, how it travels, and when it ends. And a Practice room,
// whose every game is a Practice match, and a delay policy: both fixed when the
// room is created.
#include "../session/RoomModel.hxx"

#include <algorithm>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e::room;

struct Room {
	RoomAuthority authority;
	MemberId host = 0, p1 = 0, p2 = 0;

	explicit Room(bool practice = false, DelayPolicy policy = DelayPolicy::Shared) : authority("Training flag", 8, 41, Rules(), practice, policy) {
		authority.AdvanceTime(1000);
		host = Join("Host", 0, true);
		p1 = Join("P1", 1); p2 = Join("P2", 2);
	}
	MemberId Join(const char* name, int index, bool isHost = false) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(index)}, isHost);
		EXPECT(result.accepted);
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
		action.inTraining = training;
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
		EXPECT(!room.member(room.p1).training);
		const auto before = room.authority.SnapshotView().revision;
		EXPECT(room.Say(room.p1, true).accepted && room.member(room.p1).training);
		const auto said = room.authority.SnapshotView().revision;
		EXPECT(said > before);
		EXPECT(room.Say(room.p1, true).accepted && room.authority.SnapshotView().revision == said);
		// It is theirs alone: nobody else is marked, and nothing else about them moves.
		EXPECT(!room.member(room.p2).training && !room.member(room.host).training);
		EXPECT(room.member(room.p1).status == MemberStatus::Idle && room.member(room.p1).table == -1);
		EXPECT(room.Say(room.p1, false).accepted && !room.member(room.p1).training);
		// Somebody who is not in the room cannot say it.
		EXPECT(!room.Say(9999, true).accepted);
		EXPECT(room.Say(9999, true).reason == RejectReason::UnknownMember);
	}
	{
		// It binds nothing: a queued and seated member in Training still readies, and the table starts.
		Room room;
		EXPECT(room.Apply(room.p1, ActionKind::Queue).accepted && room.Apply(room.p2, ActionKind::Queue).accepted);
		EXPECT(room.Say(room.p1, true).accepted);
		EXPECT(room.member(room.p1).status == MemberStatus::Seated && room.member(room.p1).training);
		EXPECT(room.Apply(room.p1, ActionKind::Ready).accepted && room.Apply(room.p2, ActionKind::Ready).accepted);
		EXPECT(room.member(room.p1).training);
		// A game of theirs starting ends it without their word.
		EXPECT(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
		EXPECT(room.member(room.p1).status == MemberStatus::Playing && !room.member(room.p1).training);
		// And the game ending does not bring it back.
		const auto generation = room.authority.SnapshotView().tables[0].matchGeneration;
		EXPECT(room.authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
		EXPECT(!room.member(room.p1).training);
	}
	{
		// On the wire: written only when set, read as off when absent, and in every member's own view.
		Room room;
		nlohmann::json plain = room.authority.SnapshotFor(room.p2);
		for (const auto& member : plain.at("members")) EXPECT(!member.contains("training"));
		EXPECT(room.Say(room.p1, true).accepted);
		nlohmann::json marked = room.authority.SnapshotFor(room.p2);
		int written = 0;
		for (const auto& member : marked.at("members")) written += member.contains("training") && member.at("training").get<bool>();
		EXPECT(written == 1);
		const Snapshot read = marked.get<Snapshot>();
		const auto* seen = FindMember(read, room.p1);
		EXPECT(seen && seen->training);
		// A state written before the flag existed reads as before.
		const Snapshot old = plain.get<Snapshot>();
		for (const auto& member : old.members) EXPECT(!member.training);
		// The action goes over the wire and back as itself.
		auto action = room.Make(room.p1, ActionKind::SetTraining);
		action.inTraining = true;
		const nlohmann::json wire = action;
		// Peers that predate the field read and write the word as "locked".
		EXPECT(wire.at("locked").get<bool>());
		const Action back = wire.get<Action>();
		EXPECT(back.kind == ActionKind::SetTraining && back.inTraining && !back.locked && back.actionId == action.actionId);
		nlohmann::json older = wire;
		older["locked"] = false;
		EXPECT(!older.get<Action>().inTraining);
		// Another kind's lock is not mistaken for Training.
		auto lock = room.Make(room.p1, ActionKind::LockSpectating);
		lock.locked = true;
		const Action lockBack = nlohmann::json(lock).get<Action>();
		EXPECT(lockBack.locked && !lockBack.inTraining);
		// A kind past the last known one is still refused.
		nlohmann::json unknown = action;
		unknown["kind"] = static_cast<int>(ActionKind::SetTraining) + 1;
		bool refused = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { refused = true; }
		EXPECT(refused);
	}
	{
		// A member who leaves takes it with them; one who rejoins starts without it.
		Room room;
		EXPECT(room.Say(room.p1, true).accepted);
		EXPECT(room.authority.Leave(room.p1).accepted);
		const auto again = room.Join("P1", 1);
		EXPECT(again && !room.member(again).training);
	}
	{
		// A Practice room: chosen when it is created, in every member's view, and kept
		// through a checkpoint. A room created without it says nothing on the wire.
		Room plain;
		EXPECT(!plain.authority.SnapshotView().practice);
		EXPECT(!nlohmann::json(plain.authority.SnapshotFor(plain.p2)).contains("practice"));
		EXPECT(!plain.authority.Checkpoint().at("snapshot").contains("practice"));
		Room room(true);
		const nlohmann::json view = room.authority.SnapshotFor(room.p2);
		EXPECT(view.at("practice").get<bool>() && view.get<Snapshot>().practice);
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && restored.SnapshotView().practice);
		nlohmann::json bad = view;
		bad["practice"] = 1;
		bool refused = false;
		try { (void)bad.get<Snapshot>(); } catch (const std::exception&) { refused = true; }
		EXPECT(refused);
		// Nothing played or done in the room changes it: the host's rules, a game, the host leaving.
		auto set = room.Make(room.host, ActionKind::SetRules);
		set.rules = room.authority.SnapshotView().tables[0].rules;
		set.rules.roundTime = 60;
		EXPECT(room.authority.Apply(room.host, set).accepted && room.authority.SnapshotView().practice);
		EXPECT(room.Apply(room.p1, ActionKind::Queue).accepted && room.Apply(room.p2, ActionKind::Queue).accepted);
		EXPECT(room.Apply(room.p1, ActionKind::Ready).accepted && room.Apply(room.p2, ActionKind::Ready).accepted);
		EXPECT(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
		const auto generation = room.authority.SnapshotView().tables[0].matchGeneration;
		EXPECT(room.authority.EndMatch(0, generation, MatchResult::Abort).accepted && room.authority.SnapshotView().practice);
		EXPECT(room.authority.Leave(room.host).accepted && room.authority.SnapshotView().practice);
	}
	{
		// A delay policy: Shared unless the room is created Own, written on the wire
		// only when Own, read as Shared when absent, and kept through a checkpoint.
		Room plain;
		EXPECT(plain.authority.SnapshotView().delayPolicy == DelayPolicy::Shared);
		EXPECT(!nlohmann::json(plain.authority.SnapshotFor(plain.p2)).contains("delay"));
		EXPECT(!plain.authority.Checkpoint().at("snapshot").contains("delay"));
		Room room(false, DelayPolicy::Own);
		const nlohmann::json view = room.authority.SnapshotFor(room.p2);
		EXPECT(view.at("delay") == "own" && view.get<Snapshot>().delayPolicy == DelayPolicy::Own);
		// A state written before the field existed reads as Shared, and so does an explicit "shared".
		nlohmann::json older = view;
		older.erase("delay");
		EXPECT(older.get<Snapshot>().delayPolicy == DelayPolicy::Shared);
		older["delay"] = "shared";
		EXPECT(older.get<Snapshot>().delayPolicy == DelayPolicy::Shared);
		for (const nlohmann::json& value : {nlohmann::json(1), nlohmann::json(true), nlohmann::json("mine"), nlohmann::json(nullptr), nlohmann::json::array()}) {
			nlohmann::json bad = view;
			bad["delay"] = value;
			bool refused = false;
			try { (void)bad.get<Snapshot>(); } catch (const std::exception&) { refused = true; }
			EXPECT(refused);
		}
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && restored.SnapshotView().delayPolicy == DelayPolicy::Own);
		// It is independent of Practice, and a public room (server owned) can be Own.
		Room both(true, DelayPolicy::Own);
		EXPECT(both.authority.SnapshotView().practice && both.authority.SnapshotView().delayPolicy == DelayPolicy::Own);
		// Nothing played or done in the room changes it: the host's rules, a game, the host leaving.
		auto set = room.Make(room.host, ActionKind::SetRules);
		set.rules = room.authority.SnapshotView().tables[0].rules;
		set.rules.roundTime = 60;
		EXPECT(room.authority.Apply(room.host, set).accepted && room.authority.SnapshotView().delayPolicy == DelayPolicy::Own);
		EXPECT(room.Apply(room.p1, ActionKind::Queue).accepted && room.Apply(room.p2, ActionKind::Queue).accepted);
		EXPECT(room.Apply(room.p1, ActionKind::Ready).accepted && room.Apply(room.p2, ActionKind::Ready).accepted);
		EXPECT(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
		const auto generation = room.authority.SnapshotView().tables[0].matchGeneration;
		EXPECT(room.authority.EndMatch(0, generation, MatchResult::Abort).accepted && room.authority.SnapshotView().delayPolicy == DelayPolicy::Own);
		EXPECT(room.authority.Leave(room.host).accepted && room.authority.SnapshotView().delayPolicy == DelayPolicy::Own);
	}
	{
		// Practice match is no table rule: the "training" an older build's rules
		// still carry is not read, so its SetRules makes no table, and no room,
		// a Practice one, and the room's rules never write it.
		Room room;
		auto set = room.Make(room.host, ActionKind::SetRules);
		set.rules = room.authority.SnapshotView().tables[0].rules;
		set.rules.roundTime = 9999;
		nlohmann::json older = set;
		older["rules"]["training"] = true;
		older["rules"]["time_before_practice"] = 60;
		EXPECT(room.authority.Apply(room.host, older.get<Action>()).accepted);
		EXPECT(room.authority.SnapshotView().tables[0].rules == set.rules && !room.authority.SnapshotView().practice);
		const nlohmann::json view = room.authority.SnapshotFor(room.p2);
		EXPECT(!view.at("tables").at(0).at("rules").contains("training") && !view.contains("practice"));
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room training flag rules passed.\n");
	return 0;
}
