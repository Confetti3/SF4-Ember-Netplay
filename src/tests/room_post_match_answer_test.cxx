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
		// Saying what is already so, or anything, moves no revision.
		EXPECT(room.Say(room.p2, PostMatchWord::FighterSelect, generation).accepted);
		const auto revision = room.authority.SnapshotView().revision;
		EXPECT(room.Say(room.p2, PostMatchWord::FighterSelect, generation).accepted && room.authority.SnapshotView().revision == revision);
	}
	{
		// It is presence, like Typing: an answer moves no room revision, so a
		// host's action made just before it is not stale when it arrives, and
		// every change of it still goes out in the snapshot that follows.
		Room room;
		const auto generation = room.Generation();
		auto lock = room.Make(ActionKind::Lock);
		lock.locked = true;
		const auto revision = room.authority.SnapshotView().revision;
		EXPECT(room.Say(room.p1, PostMatchWord::Rematch, generation, 1).accepted);
		EXPECT(room.Say(room.p1, PostMatchWord::Back, generation, 1).accepted && room.Say(room.p1, PostMatchWord::None, generation, 1).accepted);
		EXPECT(room.authority.SnapshotView().revision == revision);
		const auto locked = room.authority.Apply(room.host, lock);
		EXPECT(locked.accepted && room.authority.SnapshotView().locked);
	}
	{
		// A presence word is told by the runtime and compared with what the
		// room shows; the answer is shown only as said, for its game.
		EXPECT(IsPresence(ActionKind::SetTraining) && IsPresence(ActionKind::Typing) && IsPresence(ActionKind::PostMatchAnswer));
		EXPECT(!IsPresence(ActionKind::Ready) && !IsPresence(ActionKind::Lock) && !IsPresence(ActionKind::Chat));
		Member member;
		PostMatchSaid said;
		said.word = PostMatchWord::Rematch; said.table = 0; said.generation = 5; said.game = 2;
		EXPECT(said.Active() && !PostMatchSaid{}.Active());
		EXPECT(!PostMatchShown(member, said));
		member.postMatch = PostMatchWord::Rematch; member.postMatchGeneration = 5; member.postMatchGame = 2;
		EXPECT(PostMatchShown(member, said));
		member.postMatchGame = 1;
		EXPECT(!PostMatchShown(member, said));
		member.postMatchGame = 2; member.postMatchGeneration = 4;
		EXPECT(!PostMatchShown(member, said));
		// None is shown by no answer, and an old game's answer is not none.
		said.word = PostMatchWord::None;
		EXPECT(!PostMatchShown(member, said));
		EXPECT(PostMatchShown(Member{}, said));
	}
	{
		// The runtime's rule for saying it: at once, each second until the room shows it, never
		// when it already does, and not once the shell stops wanting one or the game is off.
		Member member;
		PostMatchSaid wanted, said;
		wanted.word = PostMatchWord::Back; wanted.generation = 7; wanted.game = 1;
		EXPECT(PostMatchDue(wanted, said, 0, 5000, true, &member));            // a new word is said at once
		said = wanted;
		EXPECT(!PostMatchDue(wanted, said, 5000, 5400, true, &member));        // just said: not yet
		EXPECT(!PostMatchDue(wanted, said, 5000, 5999, true, &member));
		EXPECT(PostMatchDue(wanted, said, 5000, 6000, true, &member));         // again each second
		EXPECT(PostMatchDue(wanted, said, 6000, 7000, true, &member));         // and again while unshown
		auto changed = wanted; changed.word = PostMatchWord::Rematch;
		EXPECT(PostMatchDue(changed, said, 6000, 6100, true, &member));        // a changed word does not wait
		member.postMatch = PostMatchWord::Back; member.postMatchGeneration = 7; member.postMatchGame = 1;
		EXPECT(!PostMatchDue(wanted, said, 0, 99999, true, &member));          // the room shows it: stop
		member.postMatch = PostMatchWord::None;
		EXPECT(!PostMatchDue(PostMatchSaid{}, said, 0, 99999, true, &member)); // the prompt closed: nothing wanted
		EXPECT(!PostMatchDue(wanted, said, 0, 99999, false, &member));         // the game is not on
		EXPECT(!PostMatchDue(wanted, said, 0, 99999, true, nullptr));          // no room member to compare
	}
	{
		// The room's own snapshot decides whether it is due at all: only while the room shows this PC a fighter of
		// the answer's table at its match. A set's rotation that unseats the player, or a table gone on to its next
		// match, takes no answer (the authority refuses it as NotSeated or WrongGeneration), so none is said.
		Room room;
		Snapshot view = room.authority.SnapshotView();
		view.localMember = room.p1;
		EXPECT(view.roomEpoch != 0);
		PostMatchSaid wanted;
		wanted.word = PostMatchWord::Rematch; wanted.table = 0; wanted.generation = room.Generation(); wanted.game = 3;
		const PostMatchSaid never;
		const auto* me = FindMember(view, room.p1);
		EXPECT(me && PostMatchTakes(view, *me, wanted));
		EXPECT(PostMatchDue(wanted, never, 0, 5000, true, view));              // seated at the match: said
		EXPECT(room.Say(room.p1, wanted.word, wanted.generation, wanted.game).accepted); // as the authority agrees
		// The rotation unseated them: the seat is another's, though the member still names the table.
		Snapshot rotated = view;
		rotated.tables[0].p1 = room.host;
		EXPECT(!PostMatchTakes(rotated, *FindMember(rotated, room.p1), wanted));
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, true, rotated));
		EXPECT(!PostMatchDue(wanted, wanted, 0, 99999, true, rotated));       // nor again each second
		// Moved to another table, or no table.
		Snapshot moved = view;
		for (auto& member : moved.members) if (member.id == room.p1) member.table = 1;
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, true, moved));
		for (auto& member : moved.members) if (member.id == room.p1) member.table = -1;
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, true, moved));
		// The table went on to its next match.
		Snapshot next = view;
		++next.tables[0].matchGeneration;
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, true, next));
		// Not in a room, or not a member of it.
		Snapshot none = view;
		none.roomEpoch = 0;
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, true, none));
		Snapshot stranger = view;
		stranger.localMember = 9999;
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, true, stranger));
		// The rest of the rule stands: not while the game is off, nor once the room shows it.
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, false, view));
		const Snapshot shown = room.authority.SnapshotView();
		Snapshot told = shown;
		told.localMember = room.p1;
		EXPECT(!PostMatchDue(wanted, never, 0, 5000, true, told));
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
