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
	for (const auto member : {p1, p2, queuer}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	CHECK(authority.SnapshotFor(queuer).localMatchGenerationsSent && authority.SnapshotFor(queuer).localMatchGenerations[0] == 0);
	CHECK(!authority.SnapshotView().localMatchGenerationsSent);
	for (const auto member : {p1, p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	for (const auto member : {p1, p2, queuer}) CHECK(authority.SnapshotFor(member).localMatchGenerations[0] == generation);
	CHECK(authority.SnapshotFor(host).localMatchGenerationsSent && authority.SnapshotFor(host).localMatchGenerations[0] == 0);
	RoomAuthority replica("Replica");
	CHECK(replica.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	CHECK(replica.SnapshotFor(queuer).localMatchGenerations == authority.SnapshotFor(queuer).localMatchGenerations);
	CHECK(!replica.SnapshotView().localMatchGenerationsSent && !replica.Checkpoint()["snapshot"].contains("local_match_generations"));
	Snapshot parsed;
	nlohmann::json(authority.SnapshotFor(queuer)).get_to(parsed);
	CHECK(parsed.localMatchGenerationsSent && parsed.localMatchGenerations == authority.SnapshotFor(queuer).localMatchGenerations);
	auto older = nlohmann::json(authority.SnapshotFor(queuer));
	older.erase("local_match_generations");
	older.get_to(parsed);
	CHECK(!parsed.localMatchGenerationsSent && parsed.localMatchGenerations[0] == 0);
	auto malformed = nlohmann::json(authority.SnapshotFor(queuer));
	malformed["local_match_generations"] = nlohmann::json::array({1});
	bool rejected = false;
	try { malformed.get_to(parsed); } catch (const std::exception&) { rejected = true; }
	CHECK(rejected);
	CHECK(authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
	CHECK(authority.SnapshotFor(queuer).localMatchGenerations[0] == 0);
}

int main() {
	TestLocalMatchGenerations();
	if (failures) return 1;
	std::puts("RoomSnapshot test passed");
	return 0;
}