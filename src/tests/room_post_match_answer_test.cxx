// A fighter's answer to the prompt over the WIN, told to the room as it is
// made: who may say it, for which game, how it travels, and that it binds
// nobody.
#include "../session/RoomModel.hxx"

#include <algorithm>
#include <string>

#include <cstdio>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e::room;

struct Room {
	RoomAuthority authority;
	MemberId host = 0, p1 = 0, p2 = 0;
	std::uint64_t nextAction = 1;

	Room() : authority("Post-match answer", 8, 41, Rules(), Charter()) {
		authority.AdvanceTime(1000);
		host = Join("Host", 0, true); p1 = Join("P1", 1); p2 = Join("P2", 2);
		EXPECT(Apply(p1, ActionKind::Queue).accepted && Apply(p2, ActionKind::Queue).accepted);
		EXPECT(Apply(p1, ActionKind::Ready).accepted && Apply(p2, ActionKind::Ready).accepted);
		EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	}
	MemberId Join(const char* name, int index, bool isHost = false) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(index)}, isHost);
		EXPECT(result.accepted);
		return result.accepted ? result.snapshot.members.back().id : 0;
	}
	Action Make(ActionKind kind) {
		Action action;
		action.kind = kind;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.tableRevision = authority.SnapshotView().tables[0].revision;
		action.actionId = nextAction++;
		return action;
	}
	Result Apply(MemberId member, ActionKind kind) { return authority.Apply(member, Make(kind)); }
	std::uint64_t Generation() const { return authority.SnapshotView().tables[0].matchGeneration; }
	Result Say(MemberId member, PostMatchWord word, std::uint64_t generation, std::uint32_t game = 0) {
		auto action = Make(ActionKind::PostMatchAnswer);
		action.postMatch = word; action.matchGeneration = generation; action.game = game;
		return authority.Apply(member, action);
	}
	const Member& member(MemberId id) const {
		const auto& members = authority.SnapshotView().members;
		return *std::find_if(members.begin(), members.end(), [id](const Member& value) { return value.id == id; });
	}
};

int main() {
	{
		// A fighter's answer is kept for the others, for its game, and is theirs alone.
		Room room;
		const auto generation = room.Generation();
		EXPECT(generation != 0 && room.member(room.p1).postMatch == PostMatchWord::None);
		EXPECT(room.Say(room.p1, PostMatchWord::Rematch, generation, 2).accepted);
		const auto& said = room.member(room.p1);
		EXPECT(said.postMatch == PostMatchWord::Rematch && said.postMatchGeneration == generation && said.postMatchGame == 2);
		EXPECT(room.member(room.p2).postMatch == PostMatchWord::None);
		// Changing it replaces it; none takes it back and keeps no game.
		EXPECT(room.Say(room.p1, PostMatchWord::Back, generation, 2).accepted && room.member(room.p1).postMatch == PostMatchWord::Back);
		EXPECT(room.Say(room.p1, PostMatchWord::None, generation, 2).accepted);
		EXPECT(room.member(room.p1).postMatch == PostMatchWord::None && room.member(room.p1).postMatchGeneration == 0 && room.member(room.p1).postMatchGame == 0);
		// Saying what is already so moves no revision.
		EXPECT(room.Say(room.p2, PostMatchWord::FighterSelect, generation).accepted);
		const auto revision = room.authority.SnapshotView().revision;
		EXPECT(room.Say(room.p2, PostMatchWord::FighterSelect, generation).accepted && room.authority.SnapshotView().revision == revision);
	}
	{
		// Only a seated fighter of the live match says it, for that match.
		Room room;
		const auto generation = room.Generation();
		const auto outsider = room.Say(room.host, PostMatchWord::Rematch, generation);
		EXPECT(!outsider.accepted && outsider.reason == RejectReason::NotSeated);
		const auto stale = room.Say(room.p1, PostMatchWord::Rematch, generation + 1);
		EXPECT(!stale.accepted && stale.reason == RejectReason::WrongGeneration);
		EXPECT(!room.Say(room.p1, PostMatchWord::Rematch, 0).accepted);
		EXPECT(!room.Say(9999, PostMatchWord::Rematch, generation).accepted);
		EXPECT(room.member(room.p1).postMatch == PostMatchWord::None);
	}
	{
		// It binds nothing: the seat and the table are as they were.
		Room room;
		const auto before = room.authority.SnapshotView().tables[0];
		EXPECT(room.Say(room.p1, PostMatchWord::Back, room.Generation()).accepted);
		const auto& after = room.authority.SnapshotView().tables[0];
		EXPECT(after.p1 == before.p1 && after.p2 == before.p2 && after.phase == before.phase && after.matchGeneration == before.matchGeneration);
		EXPECT(room.member(room.p1).status == MemberStatus::Playing);
	}
	{
		// On the wire: written only when set, in every member's own view, and read as none when absent.
		Room room;
		const nlohmann::json plain = room.authority.SnapshotFor(room.p2);
		for (const auto& member : plain.at("members")) EXPECT(!member.contains("post_match"));
		EXPECT(room.Say(room.p1, PostMatchWord::Rematch, room.Generation(), 1).accepted);
		const nlohmann::json marked = room.authority.SnapshotFor(room.p2);
		const Snapshot read = marked.get<Snapshot>();
		const auto* seen = FindMember(read, room.p1);
		EXPECT(seen && seen->postMatch == PostMatchWord::Rematch && seen->postMatchGeneration == room.Generation() && seen->postMatchGame == 1);
		const auto* other = FindMember(read, room.p2);
		EXPECT(other && other->postMatch == PostMatchWord::None);
		// A state from a build that did not know the answer reads as none.
		const Snapshot old = plain.get<Snapshot>();
		for (const auto& member : old.members) EXPECT(member.postMatch == PostMatchWord::None);
		// An answer out of range or without its game reads as none, or is refused whole.
		nlohmann::json torn = marked;
		for (auto& member : torn.at("members")) if (member.contains("post_match")) member.erase("post_match_generation");
		for (const auto& member : torn.get<Snapshot>().members) EXPECT(member.postMatch == PostMatchWord::None);
		// The action goes over the wire and back as itself.
		auto action = room.Make(ActionKind::PostMatchAnswer);
		action.postMatch = PostMatchWord::FighterSelect; action.matchGeneration = 7; action.game = 3;
		const Action back = nlohmann::json(action).get<Action>();
		EXPECT(back.kind == ActionKind::PostMatchAnswer && back.postMatch == PostMatchWord::FighterSelect && back.matchGeneration == 7 && back.game == 3);
		// Another kind carries no answer.
		const nlohmann::json ready = room.Make(ActionKind::Ready);
		EXPECT(!ready.contains("post_match"));
		// A kind past this one is refused, as it was before the answer existed.
		nlohmann::json unknown = nlohmann::json(action);
		unknown["kind"] = static_cast<int>(LastActionKind) + 1;
		bool refused = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { refused = true; }
		EXPECT(refused);
	}
	{
		// A fighter who leaves takes it with them.
		Room room;
		EXPECT(room.Say(room.p1, PostMatchWord::Rematch, room.Generation()).accepted);
		const auto left = room.authority.Leave(room.p1);
		EXPECT(left.accepted);
		const auto& members = room.authority.SnapshotView().members;
		EXPECT(std::none_of(members.begin(), members.end(), [&](const Member& m) { return m.id == room.p1; }));
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room post-match answer rules passed.\n");
	return 0;
}
