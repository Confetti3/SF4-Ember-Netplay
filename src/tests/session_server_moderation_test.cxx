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
	const auto line = roomhost::StatusLine(room::MaximumMembers, room::TableCount, invitation, banned);
	CHECK(line.size() < 60000 && line.size() < roomhost::MaximumStatusLineBytes);
	CHECK(line.size() > 512 * 57);
	const auto parsed = json::parse(line);
	CHECK(parsed.at("type") == "status" && parsed.at("banned").size() == 512 && parsed.at("invitation") == invitation);
	// An empty report is a short line with the same keys.
	const auto empty = json::parse(roomhost::StatusLine(0, 0, "sf4e3:x", {}));
	CHECK(empty.at("banned").is_array() && empty.at("banned").empty() && empty.at("members") == 0);
}

int main() {
	TestUncommittedKicksStayOutOfTheModerationView();
	TestFullStatusLineFitsTheBudget();
	std::puts("SessionServerModeration test passed");
	return 0;
}
