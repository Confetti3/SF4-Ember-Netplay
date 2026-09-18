// Host-side CPU cost of one committed room mutation, without helpers or a
// network, so the numbers are not blurred by 16 helper processes competing for
// the same cores. A 16-member room with four seated tables and a full chat
// history (the largest realistic snapshot) repeats Ready/Unready at one table,
// running the same owner cycle the recovery bridge runs, and every member's
// import of the result. Not a CTest: run it and compare against
// docs/perf/ROOM_PERF_BASELINE.md.
//
//   RoomHostBench [--iterations N] [--legacy-clients]
#include "../session/sf4e__SessionServer.hxx"
#include "../session/SessionRecovery.hxx"
#include "../session/CheckpointDecodeWorker.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <nlohmann/json.hpp>

using namespace sf4e;
using nlohmann::json;
namespace protocol = sf4e::SessionProtocol;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Check failed at %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)

namespace {
// Keeps sizes only; parsing what the server sends is not host work.
class BenchTransport final : public session::ServerTransport {
public:
	std::vector<session::Message> incoming;
	std::size_t sentMessages = 0, sentBytes = 0;
	bool Listen(std::uint16_t) override { return true; }
	bool Attach(session::Connection) override { return true; }
	bool Poll(std::vector<session::Message>& messages, std::vector<session::Connection>&, std::size_t maximum) override {
		const auto count = (std::min)(incoming.size(), maximum);
		messages.insert(messages.end(), incoming.begin(), incoming.begin() + count);
		incoming.erase(incoming.begin(), incoming.begin() + count);
		return true;
	}
	bool PollRecoveryPrefix(std::vector<session::Message>& messages, std::vector<session::Connection>& closed,
		std::size_t maximum, const BatchPredicate&) override {
		return Poll(messages, closed, (std::min<std::size_t>)(maximum, 1));
	}
	bool Send(session::Connection, const std::string& payload) override { ++sentMessages; sentBytes += payload.size(); return true; }
	void Close() override {}
	void Push(session::Connection connection, const json& payload) { incoming.push_back({connection, 2, payload.dump(), ""}); }
};

struct Series {
	std::vector<double> values;
	void Add(double value) { values.push_back(value); }
	void Print(const char* name) {
		std::sort(values.begin(), values.end());
		const auto at = [&](double p) { return values.empty() ? 0.0 : values[(std::min)(values.size() - 1, static_cast<std::size_t>(p * values.size()))]; };
		std::printf("  %-18s p50=%6.2f p99=%6.2f max=%6.2f ms\n", name, at(0.5), at(0.99), values.empty() ? 0.0 : values.back());
	}
};
}

int wmain(int argc, wchar_t** argv) {
	int iterations = 200;
	bool legacyClients = false; // clients without roomChatDelta get full chat in every snapshot
	for (int i = 1; i < argc; ++i) {
		if (std::wstring(argv[i]) == L"--iterations" && i + 1 < argc) iterations = std::stoi(argv[++i]);
		else if (std::wstring(argv[i]) == L"--legacy-clients") legacyClients = true;
		else { std::fprintf(stderr, "usage: RoomHostBench [--iterations N] [--legacy-clients]\n"); return 2; }
	}
	auto* transport = new BenchTransport();
	SessionServer server("room-host-bench", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> roomId = {}; roomId[0] = 42;
	const auto identity = [](session::Connection connection) {
		std::string value(64, '0'); value[62] = "0123456789abcdef"[connection >> 4 & 15]; value[63] = "0123456789abcdef"[connection & 15]; return value;
	};
	server.EnableMatchAuthorization(roomId, identity);
	server.EnableCustomRooms("Host bench", 16, 77);
	constexpr std::uint64_t term = 3;
	std::uint64_t request = 1, revision = 0;
	server.SetAuthority(term, revision, true);
	// One owner cycle: the bridge proposes the private candidate, the helper
	// commits it, and the owner applies it. Each member then imports it.
	std::vector<std::string> committed;
	const auto commit = [&]() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(proposal && !proposal->encoded.empty());
		committed.push_back(proposal->encoded);
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	};
	std::map<session::Connection, std::uint64_t> actionIds;
	const auto act = [&](session::Connection connection, room::ActionKind kind, std::uint8_t table, const std::string& text = "") {
		const auto& snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind; value.roomEpoch = snapshot.roomEpoch; value.revision = snapshot.revision;
		value.table = table; value.tableRevision = snapshot.tables[table].revision; value.actionId = ++actionIds[connection];
		value.text = text; value.inputDelay = 2;
		protocol::RoomActionMessage message; message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
		commit();
	};
	for (session::Connection connection = 1; connection <= 16; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "Member " + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion; join.roomChatDelta = !legacyClients;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commit();
	}
	CHECK(server.RoomSnapshot()->members.size() == 16);
	for (session::Connection connection = 1; connection <= 16; ++connection)
		act(connection, room::ActionKind::Queue, static_cast<std::uint8_t>((connection - 1) / 4));
	// Fill chat to its 100-message cap with near-maximum messages.
	std::uint64_t now = 1000;
	for (int message = 0; message < 100; ++message) {
		server.AdvanceCustomRoom(now += 1100);
		act(static_cast<session::Connection>(message % 16 + 1), room::ActionKind::Chat, 0, std::string(240, static_cast<char>('a' + message % 26)));
	}
	const auto& table1 = server.RoomSnapshot()->tables[1];
	const auto fighter = [&](room::MemberId id) {
		for (const auto& entry : server.roomMembers) if (entry.second == id) return entry.first;
		return session::Connection(0);
	};
	const auto readyMember = fighter(table1.p1);
	CHECK(readyMember);

	diag::SetEnabled(true);
	Series step, checkpointBuild, broadcast, journal, propose, commitTotal, commitSend, compact, importDecode, importApply;
	std::size_t proposalBytes = 0, sentBytes = 0, sentMessages = 0;
	for (int i = 0; i < iterations; ++i) {
		auto& d = diag::G();
		committed.clear();
		const auto sentBefore = transport->sentBytes, messagesBefore = transport->sentMessages;
		// Owner: Step() handles the action and journals every recipient's snapshot.
		d.OnOuterFrame(0.0);
		const auto kind = i % 2 == 0 ? room::ActionKind::Ready : room::ActionKind::Unready;
		const auto& snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind; value.roomEpoch = snapshot.roomEpoch; value.revision = snapshot.revision;
		value.table = 1; value.tableRevision = snapshot.tables[1].revision; value.actionId = ++actionIds[readyMember]; value.inputDelay = 2;
		protocol::RoomActionMessage message; message.action = value;
		transport->Push(readyMember, json(message));
		auto t0 = diag::NowMs();
		CHECK(server.Step() == 0);
		step.Add(diag::NowMs() - t0);
		broadcast.Add(d.frameMs[diag::OP_ROOM_BROADCAST]);
		journal.Add(d.frameMs[diag::OP_ROOM_JOURNAL]);
		// Owner: propose (candidate checkpoint + one encode), then apply the commit.
		d.OnOuterFrame(0.0);
		t0 = diag::NowMs();
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		propose.Add(diag::NowMs() - t0);
		checkpointBuild.Add(d.frameMs[diag::OP_ROOM_CHECKPOINT_BUILD]);
		const auto proposal = server.PendingProposal();
		proposalBytes = proposal->encoded.size();
		const auto encoded = proposal->encoded;
		d.OnOuterFrame(0.0);
		t0 = diag::NowMs();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		commitTotal.Add(diag::NowMs() - t0);
		commitSend.Add(d.frameMs[diag::OP_ROOM_COMMIT_SEND]);
		compact.Add(d.frameMs[diag::OP_ROOM_COMPACT]);
		++request; ++revision;
		sentBytes = transport->sentBytes - sentBefore; sentMessages = transport->sentMessages - messagesBefore;
		// Every other member: decode (off the game thread since this change) and
		// import on the game thread.
		t0 = diag::NowMs();
		auto decoded = session::CheckpointDecodeWorker::Decode(1, encoded);
		importDecode.Add(diag::NowMs() - t0);
		CHECK(decoded.ok);
		SessionServer replica("room-host-bench", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(new BenchTransport()));
		replica.EnableMatchAuthorization(roomId, identity);
		t0 = diag::NowMs();
		CHECK(replica.RestoreRecoveryCheckpoint(decoded.proposal.checkpoint, decoded.journal,
			session::AuthorityStamp{term, revision, false}));
		importApply.Add(diag::NowMs() - t0);
	}
	std::printf("RoomHostBench: 16 members, 4 tables, 100 chat messages, %d Ready/Unready commits\n", iterations);
	std::printf("  proposal %zu bytes; live send %zu messages / %zu bytes per commit\n", proposalBytes, sentMessages, sentBytes);
	std::printf(" owner (host game thread):\n");
	step.Print("step"); broadcast.Print("  broadcast"); journal.Print("  journal");
	propose.Print("propose"); checkpointBuild.Print("  checkpoint_build");
	commitTotal.Print("apply_commit"); commitSend.Print("  commit_send"); compact.Print("  compact");
	std::printf(" every other member:\n");
	importDecode.Print("decode (worker)"); importApply.Print("import (game)");
	return 0;
}
