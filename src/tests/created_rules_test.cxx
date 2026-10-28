// The runtime's operation that sets the rules chosen on Create on a new public
// room's tables, against a real server-owned room authority.
#include "../netplay/CreatedRules.hxx"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "test_support.hxx"

using namespace sf4e;
using netplay::CreatedRules;

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
		EXPECT(authority.SetServerOwned());
		creator = Join("emb1-creator", 1);
		rules.Start(netplay::TableSetup::Same(Chosen()), now);
	}
	room::MemberId Join(const char* account, int peer) {
		room::MemberProfile profile;
		profile.admission.account = account;
		const auto joined = authority.Join(account, room::ConnectionRef{"host", std::to_string(peer)}, false, profile);
		EXPECT(joined.accepted);
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
		return std::all_of(tables.begin(), tables.begin() + room::ClassicTableCount, [](const room::Table& t) { return t.rules == Chosen(); });
	}
};
}

// One table at a time, each sent only after the last shows the chosen rules.
static void TestOneTableAtATime() {
	Room room;
	for (int table = 0; table < static_cast<int>(room::ClassicTableCount); ++table) {
		const auto sent = room.Run(500);
		EXPECT(sent.size() == 1 && sent.front().table == table);
		if (sent.size() == 1) EXPECT(room.Apply(sent.front()));
	}
	EXPECT(room.AllChosen());
	EXPECT(room.Run(5000).empty() && !room.rules.Active() && std::string(room.rules.Ended()) == "done");
}

// A seat taken before the action lands: refused as stale, sent again at the
// table's new revision. One lost while room control is fenced: sent again
// once control is back.
static void TestStaleAndFenced() {
	Room room;
	auto sent = room.Run(100);
	EXPECT(sent.size() == 1 && room.Apply(sent.front()));
	sent = room.Run(100);
	EXPECT(sent.size() == 1 && sent.front().table == 1);
	const auto guest = room.Join("emb1-guest", 2);
	EXPECT(room.authority.Apply(guest, room.Made(guest, 1, room::ActionKind::Queue)).accepted);
	EXPECT(!room.Apply(sent.front()));
	const auto again = room.Run(100);
	EXPECT(again.size() == 1 && again.front().table == 1 && again.front().tableRevision == room.Table(1).revision);
	EXPECT(room.Apply(again.front()));
	sent = room.Run(100);
	EXPECT(sent.size() == 1 && sent.front().table == 2);
	// Lost on its way; control is fenced for two seconds.
	room.canSend = false;
	EXPECT(room.Run(2000).empty());
	room.canSend = true;
	const auto resent = room.Run(100);
	EXPECT(resent.size() == 1 && resent.front().table == 2);
	EXPECT(room.Apply(resent.front()));
	sent = room.Run(100);
	EXPECT(sent.size() == 1 && sent.front().table == 3 && room.Apply(sent.front()));
	EXPECT(room.AllChosen() && room.Run(3000).empty());
}

// Nothing is sent after the deadline, or once the creator is no longer host.
static void TestDeadlineAndHostChange() {
	{
		Room room;
		EXPECT(room.Run(100).size() == 1);
		room.Run(CreatedRules::DeadlineMs);
		EXPECT(!room.rules.Active() && std::string(room.rules.Ended()) == "deadline");
		EXPECT(room.Run(5000).empty());
	}
	{
		Room room;
		auto sent = room.Run(100);
		EXPECT(sent.size() == 1 && room.Apply(sent.front()));
		const auto guest = room.Join("emb1-guest", 2);
		EXPECT(room.authority.TransferHost(room.creator, guest).accepted);
		EXPECT(room.Run(5000).empty() && !room.rules.Active() && std::string(room.rules.Ended()) == "room_changed");
	}
	{
		// Someone else moderates: nothing is sent at all.
		room::RoomAuthority authority("Open Mic", 8, 9, room::PublicRoomRules());
		EXPECT(authority.SetServerOwned());
		room::MemberProfile first, second;
		first.admission.account = "emb1-first"; second.admission.account = "emb1-creator";
		EXPECT(authority.Join("First", room::ConnectionRef{"host", "1"}, false, first).accepted);
		const auto joined = authority.Join("Creator", room::ConnectionRef{"host", "2"}, false, second);
		CreatedRules rules;
		rules.Start(netplay::TableSetup::Same(Chosen()), 0);
		room::Action action;
		EXPECT(!rules.Next(authority.SnapshotFor(joined.snapshot.members.back().id), true, 0, action));
		EXPECT(!rules.Active() && std::string(rules.Ended()) == "not_host");
	}
	{
		// Never admitted: it does not wait for ever.
		CreatedRules rules;
		rules.Start(netplay::TableSetup::Same(Chosen()), 0);
		room::Action action;
		EXPECT(!rules.Next(room::Snapshot(), true, CreatedRules::JoinMs - 1, action) && rules.Active());
		EXPECT(!rules.Next(room::Snapshot(), true, CreatedRules::JoinMs, action) && !rules.Active());
	}
}

// A table done is done for good: the host setting it back to the defaults
// is their edit, which ends the operation instead of sending it again.
static void TestConfirmedTablesStayDone() {
	{
		Room room;
		auto sent = room.Run(100);
		EXPECT(sent.size() == 1 && room.Apply(sent.front()));
		sent = room.Run(100);
		EXPECT(sent.size() == 1 && sent.front().table == 1);
		// Table 2's is still on its way when the host puts table 1 back.
		auto back = room.Made(room.creator, 0, room::ActionKind::SetRules);
		back.rules = room::PublicRoomRules();
		EXPECT(room.authority.Apply(room.creator, back).accepted && room.Table(0).rules == room::PublicRoomRules());
		EXPECT(room.Run(5000).empty() && !room.rules.Active() && std::string(room.rules.Ended()) == "edited");
		EXPECT(room.Table(0).rules == room::PublicRoomRules());
	}
	{
		// The host's own SetRules, seen as they send it, ends it at once.
		Room room;
		EXPECT(room.Run(100).size() == 1);
		room.rules.ObservePlayer(room.Made(room.creator, 2, room::ActionKind::SetRules));
		EXPECT(!room.rules.Active() && room.Run(5000).empty());
	}
	{
		// A pending table set by hand to something else ends it too.
		Room room;
		EXPECT(room.Run(100).size() == 1);
		auto own = room.Made(room.creator, 3, room::ActionKind::SetRules);
		own.rules.format = room::SetFormat::Ft5;
		EXPECT(room.authority.Apply(room.creator, own).accepted);
		EXPECT(room.Run(5000).empty() && room.Table(3).rules.format == room::SetFormat::Ft5);
	}
	{
		// Rules equal to the default need nothing sent.
		Room room;
		room.rules.Start(netplay::TableSetup::Same(room::PublicRoomRules()), room.now);
		EXPECT(room.Run(1000).empty() && std::string(room.rules.Ended()) == "done");
	}
}

// A template sets each table apart, its name sent once its rules show.
static void TestEachTableAndItsName() {
	auto setup = netplay::TableSetup::Same(Chosen());
	setup.rules[2].format = room::SetFormat::Ft5;
	setup.names[0] = "Beginners"; setup.names[2] = "FT5";
	Room room;
	room.rules.Start(setup, room.now);
	int names = 0;
	for (int step = 0; step < 8 && room.rules.Active(); ++step) {
		const auto sent = room.Run(500);
		CHECK(sent.size() <= 1);
		if (sent.empty()) continue;
		names += sent.front().kind == room::ActionKind::NameTable;
		// A name follows its own table's rules, before the next table starts.
		if (sent.front().kind == room::ActionKind::NameTable) CHECK(room.Table(sent.front().table).rules == setup.rules[sent.front().table]);
		CHECK(room.Apply(sent.front()));
	}
	room.Run(100);
	CHECK(names == 2 && std::string(room.rules.Ended()) == "done");
	for (int i = 0; i < static_cast<int>(room::TableCount); ++i) CHECK(room.Table(i).rules == setup.rules[i] && room.Table(i).name == setup.names[i]);
	// The host naming a table by hand ends it, as their own rules do.
	Room renamed;
	renamed.rules.Start(setup, renamed.now);
	CHECK(renamed.Run(100).size() == 1);
	renamed.rules.ObservePlayer(renamed.Made(renamed.creator, 1, room::ActionKind::NameTable));
	CHECK(!renamed.rules.Active() && std::string(renamed.rules.Ended()) == "edited");
}

// An Arcade room's type and rules go in one SetRoomType, sent again until the
// room shows the type, and the operation is then done.
static void TestRoomType() {
	Room room;
	room.rules.Start(netplay::TableSetup::Same(Chosen()), room.now, room::KingRoom);
	room::Action sent;
	CHECK(room.Tick(sent) && sent.kind == room::ActionKind::SetRoomType && sent.roomType == room::KingRoom);
	CHECK(room.Run(CreatedRules::RetryMs - 16).empty());
	CHECK(room.authority.Apply(room.creator, sent).accepted);
	CHECK(room.Run(32).empty() && !room.rules.Active() && std::string(room.rules.Ended()) == "done");
	const auto& shown = room.authority.SnapshotView();
	CHECK(shown.arcade == room::KingRoom && shown.tables[7].rules == Chosen());
}

int main() {
	TestRoomType();
	TestOneTableAtATime();
	TestEachTableAndItsName();
	TestStaleAndFenced();
	TestDeadlineAndHostChange();
	TestConfirmedTablesStayDone();
	if (TEST_FAILURES()) return 1;
	std::puts("CreatedRules test passed");
	return 0;
}
