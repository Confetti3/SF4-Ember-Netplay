// A Classic room's table count: chosen when the room is created, changed by
// its host (SetTableCount), never past a table someone is at, and carried on
// the wire only when it is not the default four.
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
	std::uint64_t nextAction = 1;

	explicit Room(std::uint8_t tables = static_cast<std::uint8_t>(ClassicTableCount), RoomType type = ClassicRoom)
		: authority("Table count", 16, 41, Rules(), false, DelayPolicy::Shared, type, tables) {
		authority.AdvanceTime(1000);
		host = Join("Host", 0, true);
		p1 = Join("P1", 1); p2 = Join("P2", 2);
	}
	MemberId Join(const char* name, int index, bool isHost = false) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(index)}, isHost);
		EXPECT(result.accepted);
		return result.accepted ? result.snapshot.members.back().id : 0;
	}
	const Snapshot& View() const { return authority.SnapshotView(); }
	Action Make(ActionKind kind, std::uint8_t table = 0) {
		Action action;
		action.kind = kind;
		action.roomEpoch = View().roomEpoch;
		action.revision = View().revision;
		action.table = table;
		action.tableRevision = View().tables[table].revision;
		action.actionId = nextAction++;
		return action;
	}
	Result SetTables(MemberId member, int count) {
		auto action = Make(ActionKind::SetTableCount);
		action.tableCount = static_cast<std::uint8_t>(count);
		return authority.Apply(member, action);
	}
	Result At(MemberId member, ActionKind kind, std::uint8_t table) { return authority.Apply(member, Make(kind, table)); }
};

int main() {
	{
		// A room opens at four tables unless it was created with another count;
		// a count out of range opens at four.
		Room room;
		EXPECT(room.View().tableCount == ClassicTableCount && TablesInPlay(room.View()) == ClassicTableCount);
		EXPECT(TablesInPlay(Room(6).View()) == 6 && TablesInPlay(Room(1).View()) == 1 && TablesInPlay(Room(8).View()) == 8);
		EXPECT(TablesInPlay(Room(0).View()) == ClassicTableCount && TablesInPlay(Room(9).View()) == ClassicTableCount);
		// Created with fewer tables, the others refuse a seat.
		Room two(2);
		EXPECT(two.At(two.p1, ActionKind::Queue, 1).accepted);
		EXPECT(two.At(two.p2, ActionKind::Queue, 2).reason == RejectReason::UnknownTable);
	}
	{
		// The host grows the room to any count up to TableCount, and the new tables take seats.
		Room room;
		const auto before = room.View().revision;
		const auto grown = room.SetTables(room.host, 8);
		EXPECT(grown.accepted && room.View().tableCount == 8 && TablesInPlay(grown.snapshot) == 8 && room.View().revision > before);
		EXPECT(room.At(room.p1, ActionKind::Queue, 7).accepted && PlaceOf(room.View(), room.p1).table == 7);
		// The same count again changes nothing.
		const auto same = room.View().revision;
		EXPECT(room.SetTables(room.host, 8).accepted && room.View().revision == same);
		// Out of range is refused, and so is anyone but the host.
		EXPECT(room.SetTables(room.host, 0).reason == RejectReason::InvalidCapacity);
		EXPECT(room.SetTables(room.host, 9).reason == RejectReason::InvalidCapacity);
		EXPECT(room.SetTables(room.p2, 6).reason == RejectReason::NotHost && room.View().tableCount == 8);
		// A host's view that is out of date is stale, as a capacity change is.
		auto stale = room.Make(ActionKind::SetTableCount);
		stale.tableCount = 6; stale.revision -= 1;
		EXPECT(room.authority.Apply(room.host, stale).reason == RejectReason::StaleRoom);
	}
	{
		// A table someone sits, queues or watches at stays; it goes once it is empty.
		Room room(8);
		EXPECT(room.At(room.p1, ActionKind::Queue, 6).accepted);
		EXPECT(room.SetTables(room.host, 4).reason == RejectReason::WrongPhase && room.View().tableCount == 8);
		// Taking away only tables past the occupied one is fine.
		EXPECT(room.SetTables(room.host, 7).accepted && room.View().tableCount == 7);
		EXPECT(room.At(room.p1, ActionKind::Unqueue, 6).accepted);
		EXPECT(room.At(room.p2, ActionKind::Watch, 5).accepted);
		EXPECT(room.SetTables(room.host, 5).reason == RejectReason::WrongPhase);
		EXPECT(room.At(room.p2, ActionKind::Unwatch, 5).accepted);
		// Two fighters with a game under way keep their table too.
		EXPECT(room.At(room.p1, ActionKind::Queue, 4).accepted && room.At(room.p2, ActionKind::Queue, 4).accepted);
		EXPECT(room.At(room.p1, ActionKind::Ready, 4).accepted && room.At(room.p2, ActionKind::Ready, 4).accepted);
		const auto begun = room.authority.BeginMatch(4, room.p1, room.p2);
		EXPECT(begun.accepted && room.View().tables[4].phase == TablePhase::Playing);
		EXPECT(room.SetTables(room.host, 2).reason == RejectReason::WrongPhase);
		const auto generation = room.View().tables[4].matchGeneration;
		EXPECT(room.authority.EndMatch(4, generation, MatchResult::Abort).accepted);
		EXPECT(room.At(room.p1, ActionKind::Unqueue, 4).accepted && room.At(room.p2, ActionKind::Unqueue, 4).accepted);
		// Until its fighters acknowledge the game's end, the table still holds it.
		EXPECT(room.SetTables(room.host, 2).reason == RejectReason::TerminalLedgerFull);
		for (const auto fighter : {room.p1, room.p2}) {
			auto ack = room.Make(ActionKind::AcknowledgeTerminal, 4);
			ack.matchGeneration = generation;
			EXPECT(room.authority.Apply(fighter, ack).accepted);
		}
		// Empty at last: the room shrinks, and a table taken away refuses a seat.
		const auto shrunk = room.SetTables(room.host, 2);
		EXPECT_MSG(shrunk.accepted, "reason %d", static_cast<int>(shrunk.reason));
		EXPECT(room.View().tableCount == 2);
		EXPECT(room.At(room.p1, ActionKind::Queue, 3).reason == RejectReason::UnknownTable);
		EXPECT(room.At(room.p1, ActionKind::Queue, 1).accepted);
		// A table keeps its rules and name while it is away.
		EXPECT(room.SetTables(room.host, 8).accepted && room.View().tables[7].rules == Rules());
	}
	{
		// An Arcade or King of the hill room plays at every table and has no count to change.
		for (const RoomType type : {ArcadeRoom, KingRoom}) {
			Room room(static_cast<std::uint8_t>(ClassicTableCount), type);
			EXPECT(TablesInPlay(room.View()) == TableCount && !TableCountChangeable(room.View()));
			const auto revision = room.View().revision;
			EXPECT(room.SetTables(room.host, 2).reason == RejectReason::Unauthorized && TablesInPlay(room.View()) == TableCount);
			EXPECT(room.View().revision == revision && room.View().tableCount == ClassicTableCount);
		}
	}
	{
		// A public (server-owned) Classic room keeps its four tables: its acting
		// host's SetTableCount is refused and changes nothing.
		RoomAuthority authority("Public", 16, 41);
		EXPECT(authority.SetServerOwned());
		MemberProfile profile; profile.admission.account = "ember-a";
		const auto joined = authority.Join("Host", ConnectionRef{"room", "a"}, false, profile);
		EXPECT(joined.accepted && authority.SnapshotView().host == joined.snapshot.members.back().id);
		EXPECT(!TableCountChangeable(authority.SnapshotView()));
		Action action;
		action.kind = ActionKind::SetTableCount; action.tableCount = 6; action.actionId = 1;
		action.roomEpoch = authority.SnapshotView().roomEpoch; action.revision = authority.SnapshotView().revision;
		const auto revision = authority.SnapshotView().revision;
		EXPECT(authority.Apply(authority.SnapshotView().host, action).reason == RejectReason::Unauthorized);
		EXPECT(authority.SnapshotView().revision == revision && authority.SnapshotView().tableCount == ClassicTableCount);
	}
	{
		// A room bound to a tournament match keeps its tables too, whoever holds host powers.
		RoomAuthority authority("Match", 8, 41);
		const std::string a(64, 'a'), b(64, 'b');
		const auto joined = authority.Join("A", ConnectionRef{"room", a}, true);
		EXPECT(joined.accepted);
		TournamentBinding binding;
		binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.assignmentGeneration = 1; binding.bindingRevision = 1; binding.gamesToWin = 2;
		binding.fighters[0] = {a, "emb1_aaaa"}; binding.fighters[1] = {b, "emb1_bbbb"};
		EXPECT(authority.BindTournament(binding).accepted && !TableCountChangeable(authority.SnapshotView()));
		const auto host = ActingHost(authority.SnapshotView());
		EXPECT(host != 0);
		Action action;
		action.kind = ActionKind::SetTableCount; action.tableCount = 6; action.actionId = 1;
		action.roomEpoch = authority.SnapshotView().roomEpoch; action.revision = authority.SnapshotView().revision;
		const auto revision = authority.SnapshotView().revision;
		EXPECT(authority.Apply(host, action).reason == RejectReason::Unauthorized);
		EXPECT(authority.SnapshotView().revision == revision && authority.SnapshotView().tableCount == ClassicTableCount);
	}
	{
		// A queued member watches the game it waits out. Leaving the queue then
		// makes it an ending watcher, gone from the table when the game ends but
		// still owing its own acknowledgement. The table stays until it comes,
		// even after the fighters have acknowledged and left.
		Room room(8);
		const auto spectator = room.Join("S", 3);
		EXPECT(room.At(room.p1, ActionKind::Queue, 5).accepted && room.At(room.p2, ActionKind::Queue, 5).accepted);
		EXPECT(room.At(spectator, ActionKind::Queue, 5).accepted);
		EXPECT(room.At(room.p1, ActionKind::Ready, 5).accepted && room.At(room.p2, ActionKind::Ready, 5).accepted);
		EXPECT(room.authority.BeginMatch(5, room.p1, room.p2).accepted);
		const auto generation = room.View().tables[5].matchGeneration;
		const auto roster = room.authority.MatchRoster(5);
		EXPECT(std::find(roster.begin(), roster.end(), spectator) != roster.end());
		EXPECT(room.At(spectator, ActionKind::Unqueue, 5).accepted);
		const auto& ending = room.View().tables[5].endingWatchers;
		EXPECT(std::find(ending.begin(), ending.end(), spectator) != ending.end());
		EXPECT(room.authority.EndMatch(5, generation, MatchResult::Abort).accepted);
		EXPECT(room.At(room.p1, ActionKind::Unqueue, 5).accepted && room.At(room.p2, ActionKind::Unqueue, 5).accepted);
		const auto acknowledge = [&](MemberId member) {
			auto ack = room.Make(ActionKind::AcknowledgeTerminal, 5);
			ack.matchGeneration = generation;
			return room.authority.Apply(member, ack);
		};
		EXPECT(acknowledge(room.p1).accepted && acknowledge(room.p2).accepted);
		EXPECT(TableEmpty(room.View().tables[5]));
		const auto revision = room.View().revision;
		EXPECT(room.SetTables(room.host, 4).reason == RejectReason::TerminalLedgerFull);
		EXPECT(room.View().tableCount == 8 && room.View().revision == revision);
		// Its acknowledgement still reaches the receipt, which frees it to play
		// elsewhere, and then the table can go.
		EXPECT(room.At(spectator, ActionKind::Watch, 0).reason == RejectReason::TerminalLedgerFull);
		EXPECT(acknowledge(spectator).accepted);
		EXPECT(room.At(spectator, ActionKind::Watch, 0).accepted);
		EXPECT(room.SetTables(room.host, 4).accepted && room.View().tableCount == 4);
	}
	{
		// The wire: a room at four tables has no table_count, so an older client
		// reads it as before; any other count travels and reads back.
		Room room;
		const nlohmann::json plain = room.authority.SnapshotFor(room.p1);
		EXPECT(!plain.contains("table_count"));
		EXPECT(plain.get<Snapshot>().tableCount == ClassicTableCount);
		EXPECT(room.SetTables(room.host, 6).accepted);
		const nlohmann::json six = room.authority.SnapshotFor(room.p1);
		EXPECT(six.at("table_count") == 6);
		const auto read = six.get<Snapshot>();
		EXPECT(read.tableCount == 6 && TablesInPlay(read) == 6);
		// The chat-free snapshot carries it too.
		EXPECT(room.authority.SnapshotFor(room.p1, false).tableCount == 6);
		// A count the wire cannot hold is refused.
		for (const int bad : {0, 9, -1}) {
			auto broken = six;
			broken["table_count"] = bad;
			bool threw = false;
			try { (void)broken.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
			EXPECT(threw);
		}
		// An older host leaves it out: four tables.
		auto older = six;
		older.erase("table_count");
		EXPECT(older.get<Snapshot>().tableCount == ClassicTableCount);
		// The action: table_count only on SetTableCount.
		auto action = room.Make(ActionKind::SetTableCount);
		action.tableCount = 3;
		const nlohmann::json sent = action;
		EXPECT(sent.at("table_count") == 3 && sent.get<Action>().tableCount == 3 && sent.get<Action>().kind == ActionKind::SetTableCount);
		const nlohmann::json queue = room.Make(ActionKind::Queue);
		EXPECT(!queue.contains("table_count") && queue.get<Action>().tableCount == 0);
		// An older authority knows no kind past its last and refuses the action whole.
		auto unknown = sent;
		unknown["kind"] = static_cast<int>(LastActionKind) + 1;
		bool threw = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// A checkpoint keeps the count.
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && restored.SnapshotView().tableCount == 6);
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room table count rules passed.\n");
	return 0;
}
