// The runtime's operation that sets the rules chosen on Create on a new public
// room's tables, against a real server-owned room authority.
#include "../netplay/CreatedRules.hxx"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace sf4e;
using netplay::CreatedRules;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

namespace {
room::Rules Chosen() {
	room::Rules rules;
	rules.format = room::SetFormat::Ft3;
	rules.rotation = room::RotationMode::LoserStays;
	return rules;
}

// A public room the service opened at the public default, the creator in it
// first and so its host, and the runtime's operation with a clock.
struct Room {
	room::RoomAuthority authority{"Open Mic", 8, 9, room::PublicRoomRules()};
	room::MemberId creator = 0;
	CreatedRules rules;
	std::uint64_t now = 1000;
	bool canSend = true;
	std::uint64_t nextActionId = 1;
	Room() {
		CHECK(authority.SetServerOwned());
		creator = Join("emb1-creator", 1);
		rules.Start(Chosen(), now);
	}
	room::MemberId Join(const char* account, int peer) {
		room::MemberProfile profile;
		profile.account = account;
		const auto joined = authority.Join(account, room::ConnectionRef{"host", std::to_string(peer)}, false, profile);
		CHECK(joined.accepted);
		return joined.accepted ? joined.snapshot.members.back().id : 0;
	}
	// One runtime tick: the action it sends, if any, as the client would queue it.
	bool Tick(room::Action& sent) {
		if (!rules.Next(authority.SnapshotFor(creator), canSend, now, sent)) return false;
		sent.protocolVersion = room::ProtocolVersion;
		sent.actionId = nextActionId++;
		rules.Sent(sent, now);
		return true;
	}
	// Ticks for `ms`, 16 ms apart, collecting what is sent.
	std::vector<room::Action> Run(std::uint64_t ms) {
		std::vector<room::Action> out;
		for (std::uint64_t end = now + ms; now < end; now += 16) {
			room::Action action;
			if (Tick(action)) out.push_back(action);
		}
		return out;
	}
	bool Apply(const room::Action& action) { return authority.Apply(creator, action).accepted; }
	room::Action Made(room::MemberId member, std::uint8_t table, room::ActionKind kind) {
		room::Action action;
		action.kind = kind;
		action.protocolVersion = room::ProtocolVersion;
		const auto& s = authority.SnapshotView();
		action.roomEpoch = s.roomEpoch; action.revision = s.revision;
		action.table = table; action.tableRevision = s.tables[table].revision;
		action.actionId = 1000000 + member * 1000 + s.revision;
		return action;
	}
	const room::Table& Table(int i) const { return authority.SnapshotView().tables[i]; }
	bool AllChosen() const {
		const auto& tables = authority.SnapshotView().tables;
		return std::all_of(tables.begin(), tables.end(), [](const room::Table& t) { return t.rules == Chosen(); });
	}
};
}

// One table at a time, each sent only after the last shows the chosen rules.
static void TestOneTableAtATime() {
	Room room;
	for (int table = 0; table < static_cast<int>(room::TableCount); ++table) {
		const auto sent = room.Run(500);
		CHECK(sent.size() == 1 && sent.front().table == table);
		if (sent.size() == 1) CHECK(room.Apply(sent.front()));
	}
	CHECK(room.AllChosen());
	CHECK(room.Run(5000).empty() && !room.rules.Active() && std::string(room.rules.Ended()) == "done");
}

// A seat taken before the action lands: refused as stale, sent again at the
// table's new revision. One lost while room control is fenced: sent again
// once control is back.
static void TestStaleAndFenced() {
	Room room;
	auto sent = room.Run(100);
	CHECK(sent.size() == 1 && room.Apply(sent.front()));
	sent = room.Run(100);
	CHECK(sent.size() == 1 && sent.front().table == 1);
	const auto guest = room.Join("emb1-guest", 2);
	CHECK(room.authority.Apply(guest, room.Made(guest, 1, room::ActionKind::Queue)).accepted);
	CHECK(!room.Apply(sent.front()));
	const auto again = room.Run(100);
	CHECK(again.size() == 1 && again.front().table == 1 && again.front().tableRevision == room.Table(1).revision);
	CHECK(room.Apply(again.front()));
	sent = room.Run(100);
	CHECK(sent.size() == 1 && sent.front().table == 2);
	// Lost on its way; control is fenced for two seconds.
	room.canSend = false;
	CHECK(room.Run(2000).empty());
	room.canSend = true;
	const auto resent = room.Run(100);
	CHECK(resent.size() == 1 && resent.front().table == 2);
	CHECK(room.Apply(resent.front()));
	sent = room.Run(100);
	CHECK(sent.size() == 1 && sent.front().table == 3 && room.Apply(sent.front()));
	CHECK(room.AllChosen() && room.Run(3000).empty());
}

// Nothing is sent after the deadline, or once the creator is no longer host.
static void TestDeadlineAndHostChange() {
	{
		Room room;
		CHECK(room.Run(100).size() == 1);
		room.Run(CreatedRules::DeadlineMs);
		CHECK(!room.rules.Active() && std::string(room.rules.Ended()) == "deadline");
		CHECK(room.Run(5000).empty());
	}
	{
		Room room;
		auto sent = room.Run(100);
		CHECK(sent.size() == 1 && room.Apply(sent.front()));
		const auto guest = room.Join("emb1-guest", 2);
		CHECK(room.authority.TransferHost(room.creator, guest).accepted);
		CHECK(room.Run(5000).empty() && !room.rules.Active() && std::string(room.rules.Ended()) == "room_changed");
	}
	{
		// Someone else moderates: nothing is sent at all.
		room::RoomAuthority authority("Open Mic", 8, 9, room::PublicRoomRules());
		CHECK(authority.SetServerOwned());
		room::MemberProfile first, second;
		first.account = "emb1-first"; second.account = "emb1-creator";
		CHECK(authority.Join("First", room::ConnectionRef{"host", "1"}, false, first).accepted);
		const auto joined = authority.Join("Creator", room::ConnectionRef{"host", "2"}, false, second);
		CreatedRules rules;
		rules.Start(Chosen(), 0);
		room::Action action;
		CHECK(!rules.Next(authority.SnapshotFor(joined.snapshot.members.back().id), true, 0, action));
		CHECK(!rules.Active() && std::string(rules.Ended()) == "not_host");
	}
	{
		// Never admitted: it does not wait for ever.
		CreatedRules rules;
		rules.Start(Chosen(), 0);
		room::Action action;
		CHECK(!rules.Next(room::Snapshot(), true, CreatedRules::JoinMs - 1, action) && rules.Active());
		CHECK(!rules.Next(room::Snapshot(), true, CreatedRules::JoinMs, action) && !rules.Active());
	}
}

// A table done is done for good: the host setting it back to the defaults
// is their edit, which ends the operation instead of sending it again.
static void TestConfirmedTablesStayDone() {
	{
		Room room;
		auto sent = room.Run(100);
		CHECK(sent.size() == 1 && room.Apply(sent.front()));
		sent = room.Run(100);
		CHECK(sent.size() == 1 && sent.front().table == 1);
		// Table 2's is still on its way when the host puts table 1 back.
		auto back = room.Made(room.creator, 0, room::ActionKind::SetRules);
		back.rules = room::PublicRoomRules();
		CHECK(room.authority.Apply(room.creator, back).accepted && room.Table(0).rules == room::PublicRoomRules());
		CHECK(room.Run(5000).empty() && !room.rules.Active() && std::string(room.rules.Ended()) == "edited");
		CHECK(room.Table(0).rules == room::PublicRoomRules());
	}
	{
		// The host's own SetRules, seen as they send it, ends it at once.
		Room room;
		CHECK(room.Run(100).size() == 1);
		room.rules.ObservePlayer(room.Made(room.creator, 2, room::ActionKind::SetRules));
		CHECK(!room.rules.Active() && room.Run(5000).empty());
	}
	{
		// A pending table set by hand to something else ends it too.
		Room room;
		CHECK(room.Run(100).size() == 1);
		auto own = room.Made(room.creator, 3, room::ActionKind::SetRules);
		own.rules.format = room::SetFormat::Ft5;
		CHECK(room.authority.Apply(room.creator, own).accepted);
		CHECK(room.Run(5000).empty() && room.Table(3).rules.format == room::SetFormat::Ft5);
	}
	{
		// Rules equal to the default need nothing sent.
		Room room;
		room.rules.Start(room::PublicRoomRules(), room.now);
		CHECK(room.Run(1000).empty() && std::string(room.rules.Ended()) == "done");
	}
}

int main() {
	TestOneTableAtATime();
	TestStaleAndFenced();
	TestDeadlineAndHostChange();
	TestConfirmedTablesStayDone();
	if (failures) return 1;
	std::puts("CreatedRules test passed");
	return 0;
}
