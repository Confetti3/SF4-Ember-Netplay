// The room snapshot each recipient is sent, and its wire form: the fields the
// authority stamps per recipient, what a checkpoint keeps of them, and how a
// snapshot from an older or newer host reads.
#include "room_authority_support.hxx"
#include <exception>
// Each recipient's snapshot says which table's game its frozen roster holds it
// in: always sent, zero where it is in none, kept across a checkpoint, and read
// back from the wire. A snapshot without it reads as unsaid, not as "in none".
static void TestLocalMatchGenerations() {
	RoomAuthority authority("Rosters", 8, 313);
	const auto host = Join(authority, 0, true);
	const auto p1 = Join(authority, 1), p2 = Join(authority, 2), queuer = Join(authority, 3);
	for (const auto member : {p1, p2, queuer}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.SnapshotFor(queuer).localMatchGenerationsSent && authority.SnapshotFor(queuer).localMatchGenerations[0] == 0);
	EXPECT(!authority.SnapshotView().localMatchGenerationsSent);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	for (const auto member : {p1, p2, queuer}) EXPECT(authority.SnapshotFor(member).localMatchGenerations[0] == generation);
	EXPECT(authority.SnapshotFor(host).localMatchGenerationsSent && authority.SnapshotFor(host).localMatchGenerations[0] == 0);
	RoomAuthority replica("Replica");
	EXPECT(replica.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	EXPECT(replica.SnapshotFor(queuer).localMatchGenerations == authority.SnapshotFor(queuer).localMatchGenerations);
	EXPECT(!replica.SnapshotView().localMatchGenerationsSent && !replica.Checkpoint()["snapshot"].contains("local_match_generations"));
	Snapshot parsed;
	nlohmann::json(authority.SnapshotFor(queuer)).get_to(parsed);
	EXPECT(parsed.localMatchGenerationsSent && parsed.localMatchGenerations == authority.SnapshotFor(queuer).localMatchGenerations);
	auto older = nlohmann::json(authority.SnapshotFor(queuer));
	older.erase("local_match_generations");
	older.get_to(parsed);
	EXPECT(!parsed.localMatchGenerationsSent && parsed.localMatchGenerations[0] == 0);
	auto malformed = nlohmann::json(authority.SnapshotFor(queuer));
	malformed["local_match_generations"] = nlohmann::json::array({1});
	bool rejected = false;
	try { malformed.get_to(parsed); } catch (const std::exception&) { rejected = true; }
	EXPECT(rejected);
	EXPECT(authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
	EXPECT(authority.SnapshotFor(queuer).localMatchGenerations[0] == 0);
}

// A snapshot built without its chat is the full snapshot with the chat left
// empty, field for field, for every recipient and in every phase; a field
// added to Snapshot and missed by the chat-less copy fails here.
static void TestSnapshotWithoutChat() {
	RoomAuthority authority("Without chat", 8, 313);
	const auto host = Join(authority, 0, true);
	const auto p1 = Join(authority, 1), p2 = Join(authority, 2), queuer = Join(authority, 3);
	const auto same = [&](RoomAuthority& room) {
		for (const auto member : {host, p1, p2, queuer, MemberId(9999)}) {
			auto expected = nlohmann::json(room.SnapshotFor(member));
			expected["chat"] = nlohmann::json::array();
			EXPECT(nlohmann::json(room.SnapshotFor(member, false)) == expected);
			EXPECT(nlohmann::json(room.SnapshotFor(member, true)) == nlohmann::json(room.SnapshotFor(member)));
			EXPECT(room.SnapshotFor(member, false).chat.empty());
		}
	};
	same(authority);
	for (const auto member : {p1, p2, queuer}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	Action chat = TableAction(authority, p1, 0, ActionKind::Chat);
	chat.text = "hello";
	EXPECT(authority.Apply(p1, chat).accepted);
	EXPECT(!authority.SnapshotFor(host).chat.empty());
	same(authority);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	same(authority);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	same(authority);
	// A King of the hill room's type and cabinets, with someone seated.
	RoomAuthority king("King", 16, 314, Rules(), false, DelayPolicy::Shared, KingRoom);
	EXPECT(Join(king, 0, true) == host);
	Action sit = TableAction(king, host, 2, ActionKind::Queue);
	sit.seat = 1;
	EXPECT(king.Apply(host, sit).accepted && nlohmann::json(king.SnapshotView()).contains("arcade"));
	same(king);
}

// A member's costume, colour and Ultra come with their fighter. A new look on
// the same fighter moves the room's revision and takes back no Ready; a value
// out of range reads as not shared. The wire carries them only once shared,
// a checkpoint keeps them, and a member from an older room host, without
// them, reads as not shared. The reader skips keys it does not know, which is
// how a client older than these fields reads a newer host.
static void TestMemberLook() {
	RoomAuthority authority("Look", 8, 315);
	Join(authority, 0, true);
	const auto p1 = Join(authority, 1), p2 = Join(authority, 2);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.SetMemberChara(p1, {0}) && authority.SetMemberChara(p2, {1}));
	const auto member = [](const RoomAuthority& room, MemberId id) { return *FindMember(room.SnapshotView(), id); };
	EXPECT(member(authority, p2).costume == -1 && member(authority, p2).ultra == -1);
	EXPECT(!nlohmann::json(member(authority, p2)).contains("costume") && !nlohmann::json(member(authority, p2)).contains("ultra"));
	EXPECT(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
	const auto tableRevision = authority.SnapshotView().tables[0].revision, roomRevision = authority.SnapshotView().revision;
	bool withdrew = true;
	EXPECT(authority.SetMemberChara(p2, {1, 14, 2, 5, 1}, &withdrew) && !withdrew);
	EXPECT(authority.SnapshotView().tables[0].ready[0] && authority.SnapshotView().tables[0].revision == tableRevision);
	EXPECT(authority.SnapshotView().revision > roomRevision);
	EXPECT(!authority.SetMemberChara(p2, {1, 14, 2, 5, 1}));
	EXPECT(authority.SetMemberChara(p2, {1, 14, 2, 5, 3}) && member(authority, p2).ultra == -1);
	EXPECT(authority.SetMemberChara(p2, {1, 14, 2, 5, 1}));
	const auto wire = nlohmann::json(member(authority, p2));
	EXPECT(wire.at("costume") == 2 && wire.at("color") == 5 && wire.at("ultra") == 1);
	Member parsed;
	wire.get_to(parsed);
	EXPECT(parsed.fighter == 1 && parsed.costume == 2 && parsed.color == 5 && parsed.ultra == 1);
	auto older = wire;
	for (const char* key : {"costume", "color", "ultra"}) older.erase(key);
	older.get_to(parsed);
	EXPECT(parsed.fighter == 1 && parsed.costume == -1 && parsed.color == -1 && parsed.ultra == -1);
	auto newer = wire;
	newer["a_later_field"] = 7;
	newer.get_to(parsed);
	EXPECT(parsed.ultra == 1);
	auto malformed = wire;
	malformed["ultra"] = 3;
	bool rejected = false;
	try { malformed.get_to(parsed); } catch (const std::exception&) { rejected = true; }
	EXPECT(rejected);
	RoomAuthority replica("Replica");
	EXPECT(replica.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	EXPECT(member(replica, p2).costume == 2 && member(replica, p2).color == 5 && member(replica, p2).ultra == 1);
	// A new fighter is still a new matchup, whatever its look.
	EXPECT(authority.SetMemberChara(p2, {3, 14, 0, 0, 0}, &withdrew) && withdrew);
	EXPECT(!authority.SnapshotView().tables[0].ready[0] && member(authority, p2).costume == 0);
}

int main() {
	TestLocalMatchGenerations();
	TestSnapshotWithoutChat();
	TestMemberLook();
	if (TEST_FAILURES()) return 1;
	std::puts("RoomSnapshot test passed");
	return 0;
}