// Moderation of a server-owned room as the room host reads it: the banned list
// is the committed one, so a kick that is still an uncommitted candidate (and may
// be discarded) is never reported or forwarded. Also the status line's budget.
#include "../session/sf4e__SessionServer.hxx"
#include "../roomhost/RoomHostStatus.hxx"
#include <array>
#include <string>

#include "test_support.hxx"
#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

static void TestUncommittedKicksStayOutOfTheModerationView() {
	auto* transport = new MockTransport();
	SessionServer server("moderation", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> roomId = {};
	roomId[0] = 53;
	server.EnableMatchAuthorization(roomId, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	CHECK(server.EnableServerOwnedRooms("Moderation", 8, 53));
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 5;
	server.SetAuthority(term, revision, true);
	const auto commit = [&]() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	};
	const auto admit = [&](session::Connection connection, const std::string& account) {
		protocol::SessionJoinRequest join;
		join.username = "Member-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		server.SetConnectionAccount(connection, account);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commit();
	};
	// The kick is stepped, not committed: it is the open candidate.
	std::uint64_t actionId = 0;
	const auto kick = [&](session::Connection by, session::Connection target) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = room::ActionKind::Kick; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision; action.actionId = ++actionId; action.table = 0;
		action.target = server.roomMembers.at(target);
		protocol::RoomActionMessage message; message.action = action;
		transport->Push(by, json(message));
		CHECK(server.Step() == 0);
	};
	admit(1, "ember-host");
	admit(2, "ember-bad");
	CHECK(server.RoomSnapshot()->members.size() == 2 && server.BannedAccounts().empty());

	// A kick held as a candidate: the authority has banned the account, the
	// committed view has not, and the committed snapshot still has the member.
	kick(1, 2);
	CHECK(server.HasRecoveryCandidate());
	CHECK(server._roomAuthority->KickedAccounts() == std::vector<std::string>{"ember-bad"});
	CHECK(server.BannedAccounts().empty() && server.RoomSnapshot()->members.size() == 2);
	// Proposed but not committed is still not committed.
	CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
	CHECK(server.BannedAccounts().empty());
	// Discarded: nothing was ever exposed, and the account is no longer banned.
	server.DiscardProposal();
	CHECK(!server.HasRecoveryCandidate() && server.BannedAccounts().empty());
	CHECK(server._roomAuthority->KickedAccounts().empty() && server.RoomSnapshot()->members.size() == 2);

	// The same kick, committed: now it is exposed.
	kick(1, 2);
	CHECK(server.BannedAccounts().empty());
	commit();
	CHECK(server.BannedAccounts() == std::vector<std::string>{"ember-bad"});
	CHECK(server.RoomSnapshot()->members.size() == 1);
}

// The room host ends a server-owned room at once when its last member leaves
// with a Leave action, and only once that Leave has committed. A last member
// who dropped keeps the room for the supervisor's grace, as does a Leave that
// leaves somebody inside. A private room never reports it.
static void TestOnlyACleanLastLeaveEndsTheRoom() {
	struct Room {
		MockTransport* transport = new MockTransport();
		SessionServer server{"leave", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
		std::uint64_t request = 1, revision = 0, actionId = 0;
		const std::uint64_t term = 5;
		explicit Room(bool serverOwned) {
			if (serverOwned) CHECK(server.EnableServerOwnedRooms("Leave", 8, 61));
			else server.EnableCustomRooms("Leave", 8, 61);
			server.SetAuthority(term, revision, true);
		}
		void Commit() {
			CHECK(server.HasRecoveryCandidate());
			CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
			const auto proposal = server.PendingProposal();
			CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
			++request; ++revision;
		}
		void Admit(session::Connection connection) {
			protocol::SessionJoinRequest join;
			join.username = "Member-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			server.SetConnectionAccount(connection, "ember-" + std::to_string(connection));
			transport->Push(connection, json(hello));
			CHECK(server.Step() == 0);
			Commit();
		}
		// Stepped, not committed: the Leave is the open candidate.
		void Leave(session::Connection connection) {
			const auto snapshot = *server.RoomSnapshot();
			room::Action action;
			action.kind = room::ActionKind::Leave; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
			action.actionId = ++actionId;
			protocol::RoomActionMessage message; message.action = action;
			transport->Push(connection, json(message));
			CHECK(server.Step() == 0);
		}
		void Drop(session::Connection connection) {
			transport->disconnected.push_back(connection);
			CHECK(server.Step() == 0);
		}
	};

	// The last member leaves: not while the Leave is a candidate, then once it commits.
	{
		Room room(true);
		room.Admit(1);
		CHECK(!room.server.ServerOwnedRoomLeftEmpty());
		room.Leave(1);
		CHECK(room.server.HasRecoveryCandidate() && !room.server.ServerOwnedRoomLeftEmpty());
		room.Commit();
		CHECK(room.server.RoomSnapshot()->members.empty() && room.server.ServerOwnedRoomLeftEmpty());
		// Its connection closing afterwards is not a drop: the member is already gone.
		room.Drop(1);
		if (room.server.HasRecoveryCandidate()) room.Commit();
		CHECK(room.server.ServerOwnedRoomLeftEmpty());
		// Once the room host has closed the room there is nothing left to end.
		CHECK(room.server.CloseServerOwnedRoom());
		room.Commit();
		CHECK(room.server.RoomSnapshot()->closed && !room.server.ServerOwnedRoomLeftEmpty());
	}
	// A discarded Leave never ends the room.
	{
		Room room(true);
		room.Admit(1);
		room.Leave(1);
		CHECK(room.server.ProposeCheckpoint(room.request, room.term, room.revision, nullptr));
		room.server.DiscardProposal();
		CHECK(room.server.RoomSnapshot()->members.size() == 1 && !room.server.ServerOwnedRoomLeftEmpty());
		// The member then drops: the room waits for them.
		room.Drop(1);
		room.Commit();
		CHECK(room.server.RoomSnapshot()->members.empty() && !room.server.ServerOwnedRoomLeftEmpty());
	}
	// The connection is gone before its Leave is read: a drop, so the grace.
	{
		Room room(true);
		room.Admit(1);
		room.transport->disconnected.push_back(1);
		room.Leave(1);
		room.Commit();
		CHECK(room.server.RoomSnapshot()->members.empty() && !room.server.ServerOwnedRoomLeftEmpty());
	}
	// The last member drops: the room waits out its grace.
	{
		Room room(true);
		room.Admit(1);
		room.Drop(1);
		room.Commit();
		CHECK(room.server.RoomSnapshot()->members.empty() && !room.server.ServerOwnedRoomLeftEmpty());
	}
	// A Leave with somebody still inside, then that member drops: the grace again.
	{
		Room room(true);
		room.Admit(1);
		room.Admit(2);
		room.Leave(1);
		room.Commit();
		CHECK(room.server.RoomSnapshot()->members.size() == 1 && !room.server.ServerOwnedRoomLeftEmpty());
		room.Drop(2);
		room.Commit();
		CHECK(room.server.RoomSnapshot()->members.empty() && !room.server.ServerOwnedRoomLeftEmpty());
	}
	// One member drops, the last one leaves: nobody is left to wait for the room.
	{
		Room room(true);
		room.Admit(1);
		room.Admit(2);
		room.Drop(1);
		room.Commit();
		room.Leave(2);
		room.Commit();
		CHECK(room.server.RoomSnapshot()->members.empty() && room.server.ServerOwnedRoomLeftEmpty());
	}
	// A private room is never server-owned.
	{
		Room room(false);
		room.Admit(1);
		room.Admit(2);
		room.Leave(2);
		room.Commit();
		CHECK(!room.server.ServerOwnedRoomLeftEmpty());
	}
}

// A real Ember ID is 57 bytes: "emb1_" and 52 symbols of a-z2-7, the last a or q.
static std::string EmberId(std::size_t index) {
	const std::string symbols = "abcdefghijklmnopqrstuvwxyz234567";
	std::string id = "emb1_";
	for (std::size_t i = 0; i < 51; ++i) id += symbols[(index * 7 + i * 13 + i / 5) % symbols.size()];
	id += index % 2 ? 'q' : 'a';
	return id;
}

// The supervisor's line limit is 64 KiB. A full ban list of maximum-length
// Ember IDs and a generous invitation must stay under the host's own budget.
static void TestFullStatusLineFitsTheBudget() {
	std::vector<std::string> banned;
	for (std::size_t i = 0; i < room::MaximumKickedAccounts; ++i) banned.push_back(EmberId(i));
	CHECK(banned.front().size() == 57 && banned.back().size() == 57);
	const std::string invitation = "sf4e3:" + std::string(4096, 'x');
	const auto line = roomhost::StatusLine(room::MaximumMembers, room::TableCount, invitation, banned, {});
	CHECK(line.size() < 60000 && line.size() < roomhost::MaximumStatusLineBytes);
	CHECK(line.size() > 512 * 57);
	const auto parsed = json::parse(line);
	CHECK(parsed.at("type") == "status" && parsed.at("banned").size() == 512 && parsed.at("invitation") == invitation);
	// An empty report is a short line with the same keys.
	const auto empty = json::parse(roomhost::StatusLine(0, 0, "sf4e3:x", {}, {}));
	CHECK(empty.at("banned").is_array() && empty.at("banned").empty() && empty.at("members") == 0);
}

static room::Member Joined(room::MemberId id, const std::string& name, std::uint64_t order, int main) {
	room::Member member;
	member.id = id;
	member.name = name;
	member.joinOrder = order;
	member.mainFighter = main;
	return member;
}

// The details object the bridge lists: exact keys, the moderator first, the
// host name omitted without a moderator, and a line that never exceeds the budget.
static void TestStatusLineDetails() {
	roomhost::RoomDetails details;
	details.name = "Late night";
	details.hostName = "Kate";
	details.capacity = 8;
	details.locked = true;
	details.fighters = {3, 255, 12};
	details.setFormat = 3;
	details.rotation = 2;
	const auto line = roomhost::StatusLine(3, 1, "sf4e3:x", {}, details);
	CHECK(json::parse(line).at("details") == json::parse(
		R"({"name":"Late night","capacity":8,"locked":true,"host_name":"Kate","fighters":[3,255,12],"set_format":3,"rotation":2})"));
	details.hostName.clear();
	const auto bare = json::parse(roomhost::StatusLine(3, 1, "sf4e3:x", {}, details)).at("details");
	CHECK(!bare.contains("host_name") && bare.size() == 6);
	// A room name that is not UTF-8 is replaced instead of throwing.
	details.name = std::string("bad\xff");
	CHECK(json::parse(roomhost::StatusLine(0, 0, "sf4e3:x", {}, details)).at("details").at("name").is_string());
	CHECK(roomhost::StatusLine(0, 0, "sf4e3:x", {}, details) != line);
}

static void TestHostDisplayName() {
	using roomhost::HostDisplayName;
	CHECK(HostDisplayName("Kate") == "Kate");
	CHECK(HostDisplayName("  Kate\t") == "Kate");
	CHECK(HostDisplayName("Ka\nte") == "Ka te");
	CHECK(HostDisplayName("Ka\xC2\x85te") == "Ka te");
	// U+3000 and U+00A0 are whitespace to the bridge's trim.
	CHECK(HostDisplayName("\xE3\x80\x80Kate\xC2\xA0") == "Kate");
	CHECK(HostDisplayName("").empty() && HostDisplayName(" \t ").empty());
	CHECK(HostDisplayName(std::string(32, 'a')) == std::string(32, 'a'));
	CHECK(HostDisplayName(std::string(40, 'a')) == std::string(32, 'a'));
	// 11 three-byte characters are 33 bytes: the cut keeps 10, never half of one.
	std::string han;
	for (int i = 0; i < 11; ++i) han += "\xE6\x97\xA5";
	CHECK(HostDisplayName(han).size() == 30);
	// A cut that lands after a space drops the space.
	CHECK(HostDisplayName(std::string(31, 'a') + " b") == std::string(31, 'a'));
	// Malformed UTF-8 never survives.
	CHECK(HostDisplayName("a\xFFz") == "a z");
	CHECK(HostDisplayName("a\xE6\x97") == "a");
}

static void TestDetailsOfSnapshot() {
	room::Snapshot snapshot;
	snapshot.name = "Room";
	snapshot.capacity = 6;
	snapshot.locked = true;
	snapshot.host = 7;
	snapshot.members = {Joined(3, "Late", 5, 20), Joined(7, "Mod", 9, 1), Joined(4, "Early", 2, -1), Joined(5, "Odd", 4, 44)};
	snapshot.tables[0].rules.format = room::SetFormat::Ft5;
	snapshot.tables[0].rules.rotation = room::RotationMode::BothRotate;
	auto details = roomhost::DetailsOf(snapshot);
	CHECK(details.name == "Room" && details.capacity == 6 && details.locked && details.hostName == "Mod");
	// Moderator first, then join order; -1 and an id past the roster read as none.
	CHECK((details.fighters == std::vector<int>{1, 255, 255, 20}));
	CHECK(details.setFormat == 5 && details.rotation == 2);
	snapshot.tables[0].rules.format = room::SetFormat::Unlimited;
	snapshot.tables[0].rules.rotation = room::RotationMode::LoserStays;
	details = roomhost::DetailsOf(snapshot);
	CHECK(details.setFormat == 0 && details.rotation == 1);
	// No moderator: no host name, and the order is join order alone.
	snapshot.host = 0;
	details = roomhost::DetailsOf(snapshot);
	CHECK(details.hostName.empty() && (details.fighters == std::vector<int>{255, 255, 20, 1}));
	// At most 16 faces.
	snapshot.members.clear();
	for (int i = 0; i < 20; ++i) snapshot.members.push_back(Joined(10 + i, "m" + std::to_string(i), i, i));
	snapshot.host = 10;
	CHECK(roomhost::DetailsOf(snapshot).fighters.size() == 16);
}

int main() {
	TestUncommittedKicksStayOutOfTheModerationView();
	TestOnlyACleanLastLeaveEndsTheRoom();
	TestFullStatusLineFitsTheBudget();
	TestStatusLineDetails();
	TestHostDisplayName();
	TestDetailsOfSnapshot();
	std::puts("SessionServerModeration test passed");
	return 0;
}
