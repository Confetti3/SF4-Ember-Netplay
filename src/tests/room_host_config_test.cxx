// The room host's side of a public room's own delay: the supervisor's
// configuration line (server/ember-rooms/src/protocol.rs ChildConfig) names it
// only for a room that has it, the room host makes the room with that policy,
// and its status line reports it only then. The same line the supervisor's own
// tests build is parsed here.
#include "../../server/roomhost/RoomHost.hxx"
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_support.hxx"
#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

namespace {
// The line the supervisor writes for a room (ChildConfig): the request's fields,
// own_delay only for a room that asked for it, then the helper and the ports.
std::string Line(const std::string& ownDelay) {
	return std::string(R"({"room_id":"0123456789abcdef0123456789abcdef","name":"Friday night","capacity":8,)") +
		R"("build_id":"b1","creator":"emb_abc","bridge_id":"brg_abc","ticket_key":"a2V5","ticket_kid":"k1")" + ownDelay +
		R"(,"helper":"helper-path","port":45800,"coordination_port":45801})";
}

void TestParse() {
	roomhost::Config config;
	std::string error;
	// No key: the shared delay.
	CHECK(roomhost::ParseConfig(Line(""), config, error) && !config.ownDelay);
	CHECK(config.name == "Friday night" && config.capacity == 8 && config.port == 45800 && config.coordinationPort == 45801);
	// An explicit false is the shared delay too, and a parse starts from it.
	config.ownDelay = true;
	CHECK(roomhost::ParseConfig(Line(R"(,"own_delay":false)"), config, error) && !config.ownDelay);
	config.ownDelay = true;
	CHECK(roomhost::ParseConfig(Line(""), config, error) && !config.ownDelay);
	CHECK(roomhost::ParseConfig(Line(R"(,"own_delay":true)"), config, error) && config.ownDelay);
	// Anything but a boolean is not a configuration.
	for (const char* bad : {R"(,"own_delay":1)", R"(,"own_delay":"true")", R"(,"own_delay":null)", R"(,"own_delay":[true])"}) {
		roomhost::Config rejected;
		error.clear();
		CHECK(!roomhost::ParseConfig(Line(bad), rejected, error) && error == "own_delay");
	}
}

// From the configuration line to the room's snapshot and its status line.
void TestRoomFollowsTheConfiguration() {
	for (const bool own : {false, true}) {
		roomhost::Config config;
		std::string error;
		CHECK(roomhost::ParseConfig(Line(own ? R"(,"own_delay":true)" : ""), config, error));
		SessionServer server("room-host", "b1", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(new MockTransport()));
		CHECK(roomhost::EnableConfiguredRoom(server, config, 91));
		const auto* snapshot = server.RoomSnapshot();
		CHECK(snapshot && snapshot->serverOwned && snapshot->name == "Friday night" && snapshot->capacity == 8);
		CHECK(snapshot->delayPolicy == (own ? room::DelayPolicy::Own : room::DelayPolicy::Shared));
		// The room members get is the one that policy describes.
		CHECK(json(*snapshot).contains("delay") == own);
		// And so does the status line the bridge reads.
		const auto details = roomhost::DetailsOf(*snapshot);
		CHECK(details.ownDelay == own);
		const auto line = json::parse(roomhost::StatusLine({0, 0, "sf4e3:x", {}, details})).at("details");
		CHECK(line.contains("own_delay") == own);
		if (own) CHECK(line.at("own_delay") == true);
		// A second room cannot be made over it.
		CHECK(!roomhost::EnableConfiguredRoom(server, config, 91));
	}
}

// The room host's report as one step before RoomReporter: the status line and
// the helper's bans came out of a single Report whose revision gate was bypassed
// while a ban waited, a commit was in flight or the model had closed the room.
// Kept here to check that the split steps say exactly what it said, in the same
// order and on the same ticks.
struct OneStepReport {
	std::set<std::string> forwardedBans;
	std::map<std::string, std::uint64_t> pendingBans;
	std::uint64_t reportedSnapshotRevision = 0, reportedAppliedRevision = 0;
	roomhost::Status reported;
	bool haveReported = false, closing = false, failed = false;
	std::uint64_t closeStartedMs = 0;
	std::vector<std::string> lines, bans;
	void Tick(const SessionServer& server, std::uint64_t appliedRevision, const std::string& invitation, bool queueFull,
		std::uint64_t nowMs) {
		const auto* snapshot = server.RoomSnapshot();
		if (!snapshot) return;
		if (haveReported && snapshot->revision == reportedSnapshotRevision && appliedRevision == reportedAppliedRevision &&
			invitation == reported.invitation && pendingBans.empty() && !server.HasRecoveryCandidate() &&
			!server.PendingProposal() && (closing || !snapshot->closed)) return;
		reportedSnapshotRevision = snapshot->revision;
		reportedAppliedRevision = appliedRevision;
		roomhost::Status current;
		current.members = snapshot->members.size();
		for (const auto& table : snapshot->tables)
			if (table.phase == room::TablePhase::Playing || table.phase == room::TablePhase::Paused) ++current.tablesPlaying;
		current.invitation = invitation;
		auto committed = server.CommittedServerOwned();
		current.banned = std::move(committed.banned);
		current.details = roomhost::DetailsOf(*snapshot);
		current.games = std::move(committed.wonGames);
		for (const auto& account : current.banned) {
			if (forwardedBans.count(account)) continue;
			const auto seen = pendingBans.emplace(account, nowMs).first->second;
			if (nowMs - seen < 1000 || server.HasRecoveryCandidate() || server.PendingProposal()) continue;
			if (queueFull) continue;
			bans.push_back(account);
			forwardedBans.insert(account);
			pendingBans.erase(account);
		}
		if (snapshot->closed && !closing) { closing = true; closeStartedMs = nowMs; }
		if (haveReported && current == reported) return;
		const auto line = roomhost::StatusLine(current);
		if (line.size() > roomhost::MaximumStatusLineBytes) { failed = true; return; }
		lines.push_back(line);
		reported = std::move(current);
		haveReported = true;
	}
};

// The same tick through RoomReporter, in RoomHost::Report's order.
struct SplitReport {
	roomhost::RoomReporter reporter;
	bool closing = false, failed = false;
	std::uint64_t closeStartedMs = 0;
	std::vector<std::string> lines, bans;
	void Tick(const SessionServer& server, std::uint64_t appliedRevision, const std::string& invitation, bool queueFull,
		std::uint64_t nowMs) {
		const auto* snapshot = server.RoomSnapshot();
		if (!snapshot) return;
		reporter.ForwardBans(server, [&](const std::string& account) {
			if (queueFull) return false;
			bans.push_back(account);
			return true;
		}, nowMs);
		if (snapshot->closed && !closing) { closing = true; closeStartedMs = nowMs; }
		if (!reporter.Report(server, *snapshot, appliedRevision, invitation,
			[&](const std::string& line) { lines.push_back(line); }, nowMs)) failed = true;
	}
};

// Members join, the invitation renews, three members are kicked (two while the
// helper's queue is full, one whose notice runs out while a commit is in flight)
// and the room is closed, with reports ticking all through each candidate,
// proposal and commit. The split report emits the same lines and asks the helper
// for the same bans in the same order, on the same ticks, as the one-step report.
void TestReportStepsMatchTheOneStepReport() {
	auto* transport = new MockTransport();
	SessionServer server("room-host", "b1", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> roomId = {};
	roomId[0] = 71;
	server.EnableMatchAuthorization(roomId, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	roomhost::Config config;
	std::string error;
	CHECK(roomhost::ParseConfig(Line(""), config, error));
	CHECK(roomhost::EnableConfiguredRoom(server, config, 71));
	std::uint64_t request = 1, revision = 0, actionId = 0, now = 5000;
	constexpr std::uint64_t term = 5;
	server.SetAuthority(term, revision, true);
	std::string invitation = "sf4e3:first";
	bool queueFull = false;
	OneStepReport before;
	SplitReport after;
	const auto tick = [&](int count) {
		for (int i = 0; i < count; ++i, now += 16) {
			before.Tick(server, revision, invitation, queueFull, now);
			after.Tick(server, revision, invitation, queueFull, now);
			CHECK(after.lines == before.lines && after.bans == before.bans);
			CHECK(after.closing == before.closing && after.closeStartedMs == before.closeStartedMs);
			CHECK(after.failed == before.failed && !after.failed);
			CHECK(after.reporter.BansPending() == !before.pendingBans.empty());
		}
	};
	// A candidate the server holds is proposed and then committed, with reports
	// ticking at each stage.
	const auto commit = [&]() {
		CHECK(server.HasRecoveryCandidate());
		tick(4);
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		tick(4);
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
		tick(3);
	};
	const auto admit = [&](session::Connection connection, const std::string& account) {
		protocol::SessionJoinRequest join;
		join.username = "Member-" + std::to_string(connection); join.sidecarHash = "b1"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		server.SetConnectionAdmission(connection, {account});
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commit();
	};
	const auto kick = [&](session::Connection target) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = room::ActionKind::Kick; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision; action.actionId = ++actionId; action.table = 0;
		action.target = server.roomMembers.at(target);
		protocol::RoomActionMessage message; message.action = action;
		transport->Push(1, json(message));
		CHECK(server.Step() == 0);
		commit();
	};

	tick(3);
	admit(1, "ember-host");
	admit(2, "ember-zed");
	admit(3, "ember-bob");
	admit(4, "ember-amy");
	admit(5, "ember-cat");
	CHECK(server.RoomSnapshot()->members.size() == 5);
	invitation = "sf4e3:renewed";
	tick(2);
	// Two kicks a little apart; the helper's queue is full while both notices
	// run out, so both go on the same tick, in the committed list's order.
	kick(2);
	tick(20);
	kick(3);
	queueFull = true;
	tick(80);
	CHECK(before.bans.empty() && before.pendingBans.size() == 2);
	queueFull = false;
	tick(2);
	CHECK((before.bans == std::vector<std::string>{"ember-bob", "ember-zed"}));
	// A notice that runs out while a commit is in flight waits for the commit.
	kick(4);
	tick(50);
	transport->disconnected.push_back(5);
	CHECK(server.Step() == 0);
	CHECK(server.HasRecoveryCandidate());
	tick(30);
	CHECK(before.bans.size() == 2);
	commit();
	CHECK((before.bans == std::vector<std::string>{"ember-bob", "ember-zed", "ember-amy"}));
	// The room closes; the model's close is seen on the tick it commits.
	CHECK(server.CloseServerOwnedRoom());
	commit();
	CHECK(before.closing && server.RoomSnapshot()->closed);
	tick(5);
	// Every line differs from the one before it, and the last says what the room ended as.
	CHECK(before.lines.size() >= 8);
	for (std::size_t i = 1; i < before.lines.size(); ++i) CHECK(before.lines[i] != before.lines[i - 1]);
	const auto last = json::parse(before.lines.back());
	CHECK(last.at("invitation") == "sf4e3:renewed" && last.at("banned").size() == 3);
}

// A real Ember ID is 57 bytes: "emb1_" and 52 symbols of a-z2-7, the last a or q.
std::string EmberId(std::size_t index) {
	const std::string symbols = "abcdefghijklmnopqrstuvwxyz234567";
	std::string id = "emb1_";
	for (std::size_t i = 0; i < 51; ++i) id += symbols[(index * 7 + i * 13 + i / 5) % symbols.size()];
	id += index % 2 ? 'q' : 'a';
	return id;
}

// The largest status line the host can write is inside every limit the
// supervisor applies to it (server/roomhost/README.md, "Status line limits"):
// the line, its details object and its games array.
void TestTheLargestLineFitsTheSupervisorsLimits() {
	std::vector<std::string> banned;
	for (std::size_t i = 0; i < room::MaximumKickedAccounts; ++i) banned.push_back(EmberId(i));
	std::vector<WonGame> games;
	for (std::size_t i = 0; i < MaximumWonGames; ++i)
		games.push_back({static_cast<std::uint8_t>(room::TableCount - 1), UINT64_MAX - i, EmberId(i), EmberId(i + 1)});
	// Every byte of the names escapes to two, and every seat has a fighter.
	roomhost::RoomDetails details;
	details.name = std::string(64, '"');
	details.hostName = std::string(roomhost::MaximumHostNameBytes, '"');
	details.capacity = room::MaximumMembers;
	details.locked = true;
	details.fighters.assign(roomhost::MaximumDetailFighters, roomhost::NoFighter);
	details.setFormat = 10;
	details.rotation = 2;
	details.ownDelay = true;
	const std::string invitation = "sf4e3:" + std::string(4090, 'x');
	const auto line = roomhost::StatusLine({room::MaximumMembers, room::TableCount, invitation, banned, details, games});
	CHECK(line.size() > 40000 && line.size() < roomhost::MaximumStatusLineBytes);
	CHECK(roomhost::MaximumStatusLineBytes < roomhost::SupervisorLineBytes && roomhost::SupervisorLineBytes == 128 * 1024);
	const auto parsed = json::parse(line);
	CHECK(parsed.at("banned").size() == 512 && parsed.at("games").size() == 32);
	CHECK(parsed.at("details").dump().size() <= roomhost::SupervisorDetailsBytes);
	CHECK(parsed.at("games").dump().size() <= roomhost::SupervisorGamesBytes);
	CHECK(roomhost::SupervisorDetailsBytes == 2048 && roomhost::SupervisorGamesBytes == 6144);
}

// How long an idle room sleeps: until its next timer, but never less than a
// tick (no spin on a deadline that is due or a few ms away) and never more than
// QuietWaitMs, which is also how soon stdin closing is noticed.
void TestIdleWait() {
	const auto none = (std::numeric_limits<std::uint64_t>::max)();
	CHECK(roomhost::TickWaitMs == 16 && roomhost::QuietWaitMs == 250);
	CHECK(roomhost::IdleWaitMs(none) == roomhost::QuietWaitMs);
	CHECK(roomhost::IdleWaitMs(90000) == roomhost::QuietWaitMs);
	CHECK(roomhost::IdleWaitMs(251) == roomhost::QuietWaitMs && roomhost::IdleWaitMs(250) == 250);
	CHECK(roomhost::IdleWaitMs(100) == 100 && roomhost::IdleWaitMs(17) == 17);
	CHECK(roomhost::IdleWaitMs(16) == roomhost::TickWaitMs && roomhost::IdleWaitMs(5) == roomhost::TickWaitMs);
	CHECK(roomhost::IdleWaitMs(0) == roomhost::TickWaitMs);
}
}

int main() {
	TestParse();
	TestRoomFollowsTheConfiguration();
	TestIdleWait();
	TestReportStepsMatchTheOneStepReport();
	TestTheLargestLineFitsTheSupervisorsLimits();
	std::puts("Room host config test passed");
	return 0;
}
