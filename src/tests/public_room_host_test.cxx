// End to end: sf4e-room-host as a child process hosting a public room, real
// helpers as clients, tickets minted by the ember-protocol room_ticket example
// in place of a bridge. Needs the public Iroh relays, like the other network
// fixtures. Two members run a connection check and start a match (the helpers'
// game link reaches Ready; no GGPO session is created), then report its result.
//
//   PublicRoomHostTest <sf4-net.exe> <sf4e-room-host.exe> <room_ticket.exe> [--kill-host | --full-room | --refusal-negatives | --remote <invitation>]
//
// --kill-host: a short second scenario. One member joins, the room host is
// killed, and the member's side is printed until its room ends.
// --full-room: a capacity-16 room filled by 16 helpers (the room host's own
// endpoint is a 17th coordination node), then a 17th refused. Not part of the
// default run: it starts 17 helper processes.
// --refusal-negatives: a join against a host that is gone must not be taken for
// a refusal (the helper's reason is host_unreachable or relay_unreachable, never
// refused). A host that accepts the connection and never answers cannot be
// staged here; the Rust test public_room::tests::a_host_that_never_answers_is_a_timeout_and_not_a_refusal
// covers it.
// --remote <invitation>: no room host is started here; two members join a room
// host running elsewhere (a Linux sf4e-room-host, src/roomhost/README.md) by
// its invitation, run the connection check and a match, and leave. That host
// must be configured with this fixture's room id, bridge id, build id, ticket
// key and kid (`room_ticket key 111...1`, 64 ones) and creator (Alice's Ember
// ID, `room_ticket ember-id aaa...a`). Its status lines are read on its side.
//
// A join is only a refusal when the helper says so: join_failed with the reason
// `refused`, which a public host sends when it turns a well-formed proof away
// (bad or foreign ticket, banned account, room full). A proof the host cannot
// read gets a silent close and no reason; an unreachable host, a handshake that
// times out and a control that keeps dropping have reasons of their own, and
// none of them passes as a refusal. A refusal decided after the host has
// accepted the control (a ban or a second endpoint landing in that window)
// arrives as a control close and, after the helper's give-up, as `control_lost`.
#include "../session/IrohMatchSession.hxx"
#include "iroh_integration_fixture.hxx"
#include "../platform/HelperProcess.hxx"
#include <nlohmann/json.hpp>
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

#include "public_room_support.hxx"
using namespace sf4e;
using namespace sf4e::test::publicroom;
using nlohmann::json;

namespace {
std::uint64_t globalTimeoutMs = 240000;

// The room host child: stdin and stdout piped as the supervisor pipes them,
// stderr left on this process's, and a job so it never outlives the test.
class RoomHostChild {
public:
	~RoomHostChild() {
		if (process_) { TerminateProcess(process_, 9); WaitForSingleObject(process_, 2000); }
		// Ending the child closes the pipe the reader is blocked on.
		if (reader_.joinable()) reader_.join();
		for (HANDLE handle : {stdinWrite_, stdoutRead_, process_, job_}) if (handle) CloseHandle(handle);
	}
	bool Start(const std::wstring& executable, const std::string& configLine) {
		SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
		HANDLE stdinRead = nullptr, stdoutWrite = nullptr;
		if (!CreatePipe(&stdinRead, &stdinWrite_, &inherit, 0) || !CreatePipe(&stdoutRead_, &stdoutWrite, &inherit, 0)) return false;
		SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);
		job_ = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = stdinRead;
		startup.hStdOutput = stdoutWrite;
		startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		std::wstring command = L"\"" + executable + L"\"";
		PROCESS_INFORMATION child = {};
		const bool created = CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, TRUE,
			CREATE_SUSPENDED, nullptr, nullptr, &startup, &child) != FALSE;
		CloseHandle(stdinRead); CloseHandle(stdoutWrite);
		if (!created) return false;
		process_ = child.hProcess;
		AssignProcessToJobObject(job_, process_);
		ResumeThread(child.hThread);
		CloseHandle(child.hThread);
		reader_ = std::thread([this]() { Read(); });
		const std::string line = configLine + "\n";
		DWORD written = 0;
		return WriteFile(stdinWrite_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size();
	}
	void Kill() { TerminateProcess(process_, 9); }
	void CloseStdin() { if (stdinWrite_) { CloseHandle(stdinWrite_); stdinWrite_ = nullptr; } }
	bool Running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
	// The exit code once the child has ended within waitMs.
	std::optional<DWORD> WaitExit(DWORD waitMs) {
		DWORD code = 0;
		if (WaitForSingleObject(process_, waitMs) != WAIT_OBJECT_0 || !GetExitCodeProcess(process_, &code)) return std::nullopt;
		return code;
	}
	// A host elsewhere (--remote): no child, and the only thing known of its
	// status is the invitation it was reached by.
	void UseRemote(const std::string& invitation) {
		std::lock_guard<std::mutex> lock(mutex_);
		hosted_ = {{"type", "hosted"}, {"invitation", invitation}};
		status_ = {{"type", "status"}, {"invitation", invitation}};
	}
	json Hosted() { std::lock_guard<std::mutex> lock(mutex_); return hosted_; }
	json Status() { std::lock_guard<std::mutex> lock(mutex_); return status_; }
	json Closed() { std::lock_guard<std::mutex> lock(mutex_); return closed_; }
	bool ProtocolError() { std::lock_guard<std::mutex> lock(mutex_); return protocolError_; }

private:
	void Read() {
		std::string pending;
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(stdoutRead_, buffer, sizeof(buffer), &read, nullptr) && read) {
			pending.append(buffer, read);
			for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
				auto line = pending.substr(0, end);
				pending.erase(0, end + 1);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (!line.empty()) Take(line);
			}
		}
	}
	// Everything on stdout must be one of the protocol's objects, in order.
	void Take(const std::string& line) {
		std::lock_guard<std::mutex> lock(mutex_);
		const auto value = json::parse(line, nullptr, false);
		const auto type = value.is_object() ? value.value("type", std::string()) : std::string();
		std::cout << "  room host: " << (type == "status" ? value.dump().substr(0, 60) +
			"... banned=" + value.value("banned", json::array()).dump() : type.empty() ? line : type) << std::endl;
		if (type == "hosted" && hosted_.is_null() && status_.is_null()) hosted_ = value;
		else if (type == "status" && !hosted_.is_null() && !value.value("invitation", std::string()).empty()) status_ = value;
		else if (type == "closed") closed_ = value;
		else protocolError_ = true;
	}
	HANDLE stdinWrite_ = nullptr, stdoutRead_ = nullptr, process_ = nullptr, job_ = nullptr;
	std::thread reader_;
	std::mutex mutex_;
	json hosted_, status_, closed_;
	bool protocolError_ = false;
};

int clientErrors = 0;

// One player: its own helper process, room and session client, wired like the
// game (a passive server replica beside the client).
struct Player {
	std::string label, seed, emberId;
	platform::HelperProcess process;
	platform::HelperClient helper;
	test::IrohIntegrationPeer peer;
	std::unique_ptr<session::IrohMatchSession> match;
	bool failed = false, matchFailed = false;

	Player(std::string name, std::string keySeed) : label(std::move(name)), seed(std::move(keySeed)) {
		emberId = Tool("ember-id " + seed).at(0);
		peer.name = label;
	}
	session::IrohRoom& Room() { return *peer.room; }
	const room::Snapshot& View() const { return peer.client->GetRoomSnapshot(); }
	bool Joined() const {
		if (!peer.client) return false;
		const auto& view = View();
		return view.localMember && room::FindMember(view, view.localMember) != nullptr;
	}
	bool IsHost() const { return Joined() && View().host == View().localMember; }
	// One tick in the application's order. A failure is recorded, not fatal:
	// some steps expect a client to be turned away.
	void Pump() {
		if (!peer.room) return;
		if (!peer.configured) { peer.room->Poll(); return; }
		if (failed) return;
		if (!peer.recovery.Tick(*peer.server, *peer.room)) {
			std::cerr << "  " << label << ": recovery import failed: " << peer.recovery.Error() << '\n';
			failed = true; return;
		}
		peer.server->AdvanceCustomRoom(GetTickCount64());
		if (peer.server->Step() != 0 || peer.client->Step() != 0) { failed = true; return; }
		if (match && !matchFailed && !match->Tick(false)) {
			std::cerr << "  " << label << ": match failed: " << match->Error() << '\n';
			matchFailed = true;
		}
	}
};

// Why a player is not where the script expects it.
void Dump(Player& player) {
	const auto& authority = player.Room().Coordination();
	const auto recovery = player.Room().RecoveryState();
	std::cerr << "  " << player.label << ": room=" << static_cast<int>(player.Room().GetState()) << " error=" << player.Room().Error()
		<< " term=" << authority.term << " revision=" << authority.revision << " writable=" << authority.writable
		<< " rebound=" << authority.rebound << " leader_local=" << authority.leaderLocal
		<< " applied=" << player.peer.recovery.AppliedRevision() << " recovery_error=" << player.peer.recovery.Error()
		<< " staged=" << recovery.stagedCheckpoints << " client_queue=" << recovery.clientQueueMessages
		<< " client_head=" << recovery.clientHeadType << '/' << recovery.clientHeadCommitted << '/' << recovery.clientHeadTerm
		<< '/' << recovery.clientHeadRevision << " failed=" << player.failed;
	if (player.peer.client) std::cerr << " members=" << player.View().members.size() << " local=" << player.View().localMember
		<< " view_revision=" << player.View().revision << " client_error=" << player.peer.client->RoomError()
		<< " rejected=" << (player.peer.client->JoinRejection() ? static_cast<int>(*player.peer.client->JoinRejection()) : -1);
	std::cerr << std::endl;
	// The commit waiting to be imported, and the endpoints this room can bind.
	session::IrohRoom::CommittedCheckpoint head;
	if (player.Room().TakeCommittedCheckpoint(head)) {
		std::cerr << "    head revision=" << head.identity.revision << " members:";
		for (const auto& member : head.proposal->checkpoint.at("members"))
			std::cerr << ' ' << member.at("data").value("authenticatedEndpoint", std::string()).substr(0, 8)
				<< '/' << member.value("incarnation", member.at("data").value("incarnation", std::uint64_t(0)));
		std::cerr << std::endl;
	}
	std::cerr << "    local=" << player.Room().LocalIdentity().substr(0, 8) << " known:";
	for (const auto& known : player.Room().ControlIdentities())
		std::cerr << ' ' << known.first << '=' << known.second.substr(0, 8) << '/' << player.Room().PeerIncarnation(known.first);
	std::cerr << std::endl;
}

std::wstring helperPath;
std::vector<Player*> players;
RoomHostChild* host = nullptr;
bool remoteHost = false; // --remote: the host's status lines are not visible here.
std::atomic<bool> finished{false};

void Pump() { for (auto* player : players) player->Pump(); }

bool WaitFor(const std::function<bool()>& done, std::uint64_t timeoutMs) {
	const auto deadline = GetTickCount64() + timeoutMs;
	do { Pump(); if (done()) return true; Sleep(5); } while (GetTickCount64() < deadline);
	return false;
}

int failures = 0;
void Step(const char* name, bool passed, const std::string& detail = {}) {
	std::cout << (passed ? "PASS " : "FAIL ") << name << (detail.empty() ? "" : " (" + detail + ")") << std::endl;
	if (!passed) ++failures;
}

std::unique_ptr<Player> StartPlayer(const std::string& label, const std::string& seed) {
	std::unique_ptr<Player> player(new Player(label, seed));
	CHECK(player->process.Start(helperPath, GetCurrentProcessId()));
	CHECK(player->helper.Start(player->process.Bootstrap()));
	player->peer.room = std::make_shared<session::IrohRoom>(player->helper);
	players.push_back(player.get());
	CHECK(WaitFor([&]() { return player->helper.State() == platform::HelperState::Connected; }, 15000));
	// The ticket names the endpoint, so ask for it before any room exists.
	CHECK(player->helper.Send("{\"type\":\"status\"}"));
	CHECK(WaitFor([&]() { return !player->Room().LocalIdentity().empty(); }, 15000));
	return player;
}

json Ticket(const std::string& ticketSeed, const std::string& kid, const std::string& emberId, const std::string& endpoint) {
	const auto lines = Tool("sign " + ticketSeed + " " + kid + " " + Bridge + " " + RoomIdHex + " " + emberId + " " + endpoint);
	return json::parse(lines.at(0));
}

std::uint64_t Members() { const auto status = host->Status(); return status.is_null() ? UINT64_MAX : status.value("members", UINT64_MAX); }

// Joins with a ticket for this player's own endpoint. True once it is a member.
bool JoinRoom(Player& player, const std::string& ticketSeed, const std::string& kid, std::uint8_t replicaCapacity = 4) {
	const auto invitation = host->Status().value("invitation", std::string());
	if (!player.Room().JoinPublic(invitation, Ticket(ticketSeed, kid, player.emberId, player.Room().LocalIdentity()), Build)) return false;
	using State = session::IrohRoom::State;
	if (!WaitFor([&]() { return player.Room().GetState() == State::Ready || player.Room().GetState() == State::Failed; }, 45000) ||
		player.Room().GetState() != State::Ready) { Dump(player); return false; }
	SessionClient::Callbacks callbacks = {};
	callbacks.OnError = [](SessionClient::ErrorType, SessionClient*, const SessionClient::Callbacks&) { ++clientErrors; };
	if (!test::ConfigureIrohIntegrationPeer(player.peer, callbacks, Build, 0, replicaCapacity)) return false;
	if (WaitFor([&]() { return player.Joined() || player.failed || player.peer.client->JoinRejection(); }, 30000) && player.Joined()) return true;
	Dump(player);
	return false;
}

// Which layer is expected to turn a join away. A refusal is only a pass when it
// is observed explicitly and is the expected one; a timeout, a stalled
// registration or a recovery failure is a failed step, not a refusal.
enum class Layer {
	// The helper's admission policy turns the proof away and says so: IrohRoom
	// fails with the helper's join_failed error and the reason `refused`.
	Helper,
	// The helper cannot read the proof and closes without an answer: join_failed
	// with no reason (not refused, not a timeout, not unreachable).
	HelperClosed,
	// The control is admitted and the room model rejects the registration: the
	// session client reports this join rejection.
	Model
};

// A refusal is reported within seconds; the bound also covers the helper's own
// give-up on a join whose controls keep closing (JOIN_GIVE_UP_AFTER, 45 s).
constexpr std::uint64_t helperRefusalMs = 90000;

// A join the room must turn away at `layer` (with `rejection`, for Layer::Model).
// Returns whether it was refused there, and says in `where` what was observed.
bool Refused(Player& player, const std::function<bool()>& begin, Layer layer, std::string& where,
	SessionClient::ErrorType rejection = SessionClient::ErrorType::SCE_UNKNOWN) {
	using State = session::IrohRoom::State;
	if (!begin()) { where = "command not sent"; return false; }
	const auto stateOf = [&]() { return static_cast<int>(player.Room().GetState()); };
	if (!WaitFor([&]() { return player.Room().GetState() == State::Ready || player.Room().GetState() == State::Failed ||
		player.Room().GetState() == State::Idle; }, helperRefusalMs)) {
		where = "no outcome within " + std::to_string(helperRefusalMs / 1000) + " s: room state " + std::to_string(stateOf());
		return false;
	}
	if (player.Room().GetState() != State::Ready) {
		if (player.Room().Error() != "join_failed") {
			where = "the room ended with state " + std::to_string(stateOf()) + " and error '" + player.Room().Error() + "', not join_failed";
			return false;
		}
		const std::string reason = player.Room().FailureReason();
		where = "helper ended the join: join_failed, reason '" + (reason.empty() ? std::string("(none)") : reason) + "'";
		if (layer == Layer::Model) { where += " (the room model was expected to refuse it)"; return false; }
		const std::string expected = layer == Layer::Helper ? "refused" : "";
		if (reason != expected) {
			where += ", expected " + (expected.empty() ? std::string("none (a silent close)") : "'" + expected + "'");
			return false;
		}
		return true;
	}
	// The control was admitted; the room model is the last line.
	SessionClient::Callbacks callbacks = {};
	callbacks.OnError = [](SessionClient::ErrorType, SessionClient*, const SessionClient::Callbacks&) { ++clientErrors; };
	if (!test::ConfigureIrohIntegrationPeer(player.peer, callbacks, Build, 0, 4)) { where = "client not attached"; return false; }
	if (!WaitFor([&]() { return player.Joined() || player.failed || player.peer.client->JoinRejection(); }, 30000)) {
		where = "the helper admitted the control, then the registration got no answer within 30 s";
		return false;
	}
	const auto reported = player.peer.client->JoinRejection();
	if (player.Joined()) { where = "ADMITTED"; return false; }
	if (!reported) {
		// A client that stops without a rejection failed in transport or recovery.
		where = "the helper admitted the control, then the client failed without a join rejection: " +
			(player.peer.client->RoomError().empty() ? player.peer.recovery.Error() : player.peer.client->RoomError());
		return false;
	}
	where = "room model rejected the registration: " + std::string(SessionClient::JoinRejectionKey(*reported)) +
		" (error type " + std::to_string(static_cast<int>(*reported)) + ")";
	if (layer != Layer::Model) { where += " (the helper was expected to refuse it)"; return false; }
	if (*reported != rejection) { where += ", expected error type " + std::to_string(static_cast<int>(rejection)); return false; }
	return true;
}

// Leaves the room, if it is in one, and ends the helper.
void StopPlayer(std::unique_ptr<Player>& player) {
	if (!player) return;
	using State = session::IrohRoom::State;
	player->match.reset();
	if (player->peer.client) player->peer.client->Disconnect();
	else if (player->peer.room) player->peer.room->Leave();
	player->peer.client.reset();
	player->peer.server.reset();
	player->peer.configured = false;
	WaitFor([&]() { return player->Room().GetState() == State::Idle || player->Room().GetState() == State::Failed; },
		session::IrohRoom::LeaveTimeoutMs + 2000);
	player->helper.Send("{\"type\":\"shutdown\"}");
	WaitFor([&]() { return !player->process.IsRunning(); }, 3000);
	players.erase(std::remove(players.begin(), players.end(), player.get()), players.end());
	player.reset();
}

bool SendAction(Player& player, room::Action action, std::uint64_t* actionId = nullptr) {
	const auto& view = player.View();
	action.roomEpoch = view.roomEpoch; action.revision = view.revision;
	action.tableRevision = view.tables[action.table].revision;
	return WaitFor([&]() { return player.peer.client->SendRoomAction(action, actionId) == session::SendResult::Queued; }, 10000);
}

// The connection check one seated player runs against the other before Ready.
bool Probe(Player& from, Player& to, std::uint64_t request, std::string& detail) {
	const auto peer = to.Room().LocalIdentity();
	if (!WaitFor([&]() { const auto control = from.Room().ConnectionForIdentity(peer);
		return control != 0 && from.Room().PeerIncarnation(control) != 0; }, 15000)) { detail = "peer endpoint unknown to the room"; return false; }
	if (!from.Room().RequestProbe(peer, request, from.View().tables[0].revision)) { detail = "request refused"; return false; }
	WaitFor([&]() { const auto& probe = from.Room().Probe(); return probe.status != "checking"; }, 40000);
	const auto& probe = from.Room().Probe();
	detail = "status=" + probe.status + " route=" + probe.route + " replies=" + std::to_string(probe.samples) + "/" +
		std::to_string(probe.samples + probe.lost) + " p95_us=" + std::to_string(probe.p95RttUs) +
		" delay=" + std::to_string(probe.recommended) + " failure=" + std::to_string(probe.failureReason);
	return (probe.status == "ready" || probe.status == "complete") && probe.samples >= 80 && probe.recommended >= 0;
}

// A and B, seated at table 0, ready up; the authority grants the match and the
// two helpers link directly. Then both report the result and acknowledge it.
void PlayMatch(Player& a, Player& b) {
	using Phase = session::IrohMatchSession::Phase;
	std::string detail;
	Step("A's connection check to B completes", Probe(a, b, 1, detail), detail);
	Step("B's connection check to A completes", Probe(b, a, 1, detail), detail);
	a.match.reset(new session::IrohMatchSession(*a.peer.client, a.peer.room));
	b.match.reset(new session::IrohMatchSession(*b.peer.client, b.peer.room));
	room::Action ready; ready.kind = room::ActionKind::Ready; ready.table = 0; ready.inputDelay = 2;
	const auto readied = [&](Player& player) {
		std::uint64_t id = 0;
		bool accepted = false;
		return SendAction(player, ready, &id) && WaitFor([&]() {
			SessionClient::ActionReply reply;
			while (player.peer.client->TakeActionReply(reply)) if (reply.actionId == id) accepted = reply.accepted;
			return accepted;
		}, 15000);
	};
	Step("A and B ready up", readied(a) && readied(b));
	const bool startedMatch = WaitFor([&]() { return (a.match->GetPhase() == Phase::Started && b.match->GetPhase() == Phase::Started) ||
		a.matchFailed || b.matchFailed; }, 60000) && !a.matchFailed && !b.matchFailed;
	const auto aLink = a.Room().Game(b.Room().LocalIdentity()), bLink = b.Room().Game(a.Room().LocalIdentity());
	Step("the match starts: both fighters' game links are ready", startedMatch &&
		aLink.state == session::IrohRoom::GameState::Ready && bLink.state == session::IrohRoom::GameState::Ready && aLink.virtualPort && bLink.virtualPort,
		"phases " + std::to_string(static_cast<int>(a.match->GetPhase())) + "/" + std::to_string(static_cast<int>(b.match->GetPhase())) +
		" generation " + std::to_string(a.match->Generation()) + " routes " + aLink.route + " , " + bLink.route +
		(a.match->Error().empty() ? "" : " A: " + a.match->Error()) + (b.match->Error().empty() ? "" : " B: " + b.match->Error()));
	if (!startedMatch) { Dump(a); Dump(b); return; }
	if (!remoteHost) Step("status shows 1 table playing", WaitFor([&]() { return host->Status().value("tables_playing", 0) == 1; }, 10000));

	const auto generation = a.match->Generation();
	a.match->End(); b.match->End();
	room::Action result; result.kind = room::ActionKind::RecordResult; result.table = 0;
	result.matchGeneration = generation; result.result = room::MatchResult::P1Win;
	const auto reported = [&](Player& player) {
		std::uint64_t id = 0;
		bool confirmed = false;
		return SendAction(player, result, &id) && WaitFor([&]() {
			SessionClient::ActionReply reply;
			while (player.peer.client->TakeActionReply(reply))
				if (reply.actionId == id) confirmed = reply.accepted || reply.reason == room::RejectReason::DuplicateResult;
			return confirmed;
		}, 20000);
	};
	Step("both fighters report the result", reported(a) && reported(b));
	bool aSeen = false, aQueued = false, bSeen = false, bQueued = false;
	const std::vector<std::pair<std::uint8_t, std::uint64_t>> terminal{{std::uint8_t(0), generation}};
	Step("the match ends and both acknowledge it", WaitFor([&]() {
		const bool idle = a.match->GetPhase() == Phase::Idle && b.match->GetPhase() == Phase::Idle;
		test::AcknowledgeIrohFixtureTerminal(*a.peer.client, 0, generation, idle, aSeen, aQueued);
		test::AcknowledgeIrohFixtureTerminal(*b.peer.client, 0, generation, idle, bSeen, bQueued);
		// A member's passive replica holds the committed room, receipts included.
		return aQueued && bQueued && test::IrohFixtureTerminalsCommitted(*a.peer.server, terminal);
	}, 45000), "score " + std::to_string(a.View().tables[0].score[0]) + "-" + std::to_string(a.View().tables[0].score[1]));
	if (!remoteHost) Step("status shows no table playing", WaitFor([&]() { return host->Status().value("tables_playing", 9) == 0; }, 10000));
	a.match.reset(); b.match.reset();
}

// Second scenario: what a member sees when the room host dies without closing.
int KillHostScenario(const std::wstring& roomHostPath, const json& config, const std::string& bridgeSeed, const std::string& kid) {
	RoomHostChild child;
	host = &child;
	CHECK(child.Start(roomHostPath, config.dump()));
	CHECK(WaitFor([&]() { return !child.Status().is_null(); }, 30000));
	auto a = StartPlayer("Alice", Seed('a'));
	Step("A joins", JoinRoom(*a, bridgeSeed, kid));
	using State = session::IrohRoom::State;
	const auto killed = GetTickCount64();
	child.Kill();
	const auto describe = [&]() {
		const auto& view = a->View();
		return "IrohRoom state=" + std::to_string(static_cast<int>(a->Room().GetState())) + " error='" + a->Room().Error() +
			"' writable=" + std::to_string(a->Room().Coordination().writable) + " | snapshot closed=" + std::to_string(view.closed) +
			" members=" + std::to_string(view.members.size()) + " localMember=" + std::to_string(view.localMember) +
			" host=" + std::to_string(view.host) + " | client connected=" + std::to_string(a->peer.client->IsConnected()) +
			" room_error='" + a->peer.client->RoomError() + "' step_failed=" + std::to_string(a->failed);
	};
	std::string last;
	const bool ended = WaitFor([&]() {
		const auto now = describe();
		if (now != last) { std::cout << "  +" << GetTickCount64() - killed << " ms: " << now << std::endl; last = now; }
		return a->Room().GetState() == State::Idle || a->Room().GetState() == State::Failed;
	}, 60000);
	Step("A's room ends after the host is killed", ended, std::to_string(GetTickCount64() - killed) + " ms; final: " + describe());
	StopPlayer(a);
	return failures ? 1 : 0;
}

// Third scenario: a room at capacity 16. Each member's helper holds control to
// the room host only, but the host's coordination membership is the 16 members
// and the host itself.
int FullRoomScenario(const std::wstring& roomHostPath, const json& config, const std::string& bridgeSeed, const std::string& kid) {
	using State = session::IrohRoom::State;
	constexpr std::size_t Capacity = room::MaximumMembers;
	RoomHostChild child;
	host = &child;
	CHECK(child.Start(roomHostPath, config.dump()));
	CHECK(WaitFor([&]() { return !child.Status().is_null(); }, 30000));
	// The first member is the creator the room host was configured with.
	const auto seedFor = [](std::size_t index) {
		static const char* const digits = "0123456789abcdef";
		return index == 0 ? Seed('a') : std::string(62, '7') + digits[index >> 4] + digits[index & 15];
	};
	std::vector<std::unique_ptr<Player>> members;
	for (std::size_t index = 0; index < Capacity; ++index) {
		members.push_back(StartPlayer("Member" + std::to_string(index + 1), seedFor(index)));
		const auto began = GetTickCount64();
		const bool joined = JoinRoom(*members.back(), bridgeSeed, kid, static_cast<std::uint8_t>(Capacity));
		Step(("member " + std::to_string(index + 1) + " joins").c_str(), joined, std::to_string(GetTickCount64() - began) + " ms");
		if (!joined) break;
	}
	const auto settled = [&](Player& player) {
		const auto& authority = player.Room().Coordination();
		return !player.failed && player.Joined() && player.Room().GetState() == State::Ready && authority.writable && authority.rebound &&
			player.View().members.size() == Capacity;
	};
	std::string unsettled;
	const bool allSettled = members.size() == Capacity && WaitFor([&]() {
		unsettled.clear();
		for (auto& member : members) if (!settled(*member)) unsettled += " " + member->label;
		return unsettled.empty();
	}, 60000);
	Step("every member is joined, rebound and following a writable host", allSettled, unsettled.empty() ? std::string() : "not settled:" + unsettled);
	if (!allSettled) for (auto& member : members) if (!settled(*member)) Dump(*member);
	Step("the last joiner sees 16 members", !members.empty() && members.back()->View().members.size() == Capacity);
	Step("status shows 16 members", WaitFor([&]() { return Members() == Capacity; }, 10000));
	{
		auto extra = StartPlayer("Member17", std::string(62, '7') + "ff");
		std::string where;
		const bool refused = Refused(*extra, [&]() {
			return extra->Room().JoinPublic(child.Status().value("invitation", std::string()),
				Ticket(bridgeSeed, kid, extra->emberId, extra->Room().LocalIdentity()), Build);
		}, Layer::Helper, where);
		Step("a 17th member is refused by the helper (room full)", refused, where);
		StopPlayer(extra);
	}
	Step("the refusal left 16 members, all still following the host", Members() == Capacity &&
		std::all_of(members.begin(), members.end(), [&](const std::unique_ptr<Player>& member) { return settled(*member); }));
	for (auto it = members.rbegin(); it != members.rend(); ++it) StopPlayer(*it);
	Step("everyone left: status shows 0 members", WaitFor([&]() { return Members() == 0; }, 60000));
	Step("room host wrote only protocol lines", !child.ProtocolError());
	return failures ? 1 : 0;
}

// Fourth scenario: a join that fails for a reason other than a refusal is not
// taken for one. The invitation is a room host's that has since been killed:
// its endpoint is real and the relay is the real one, but nothing answers.
int RefusalNegativesScenario(const std::wstring& roomHostPath, const json& config, const std::string& bridgeSeed, const std::string& kid) {
	std::string gone;
	{
		RoomHostChild child;
		json goneConfig = config;
		goneConfig["port"] = 45810;
		goneConfig["coordination_port"] = 45811;
		CHECK(child.Start(roomHostPath, goneConfig.dump()));
		const bool hosted = WaitFor([&]() { return !child.Hosted().is_null() || !child.Running(); }, 30000) && !child.Hosted().is_null();
		Step("a room host to take down reports hosted", hosted);
		if (!hosted) return 1;
		gone = child.Hosted().value("invitation", std::string());
		child.Kill();
		Step("the room host is gone", WaitFor([&]() { return !child.Running(); }, 5000));
	}
	auto lost = StartPlayer("Lost", Seed('d'));
	std::string where;
	const auto began = GetTickCount64();
	const bool accepted = Refused(*lost, [&]() {
		return lost->Room().JoinPublic(gone, Ticket(bridgeSeed, kid, lost->emberId, lost->Room().LocalIdentity()), Build);
	}, Layer::Helper, where);
	const std::string reason = lost->Room().FailureReason();
	Step("a join against a host that is gone is not accepted as a refusal", !accepted, where + " after " + std::to_string(GetTickCount64() - began) + " ms");
	Step("it ends join_failed with an unreachable reason, never refused", lost->Room().Error() == "join_failed" &&
		(reason == "host_unreachable" || reason == "relay_unreachable"), "reason '" + reason + "'");
	StopPlayer(lost);
	return failures ? 1 : 0;
}

// Fifth scenario: a room host that runs elsewhere, reached by its invitation.
// The joins, seats, connection checks and match are the default scenario's;
// what the host reports is read where it runs.
int RemoteScenario(const std::string& invitation, const std::string& bridgeSeed, const std::string& kid) {
	RoomHostChild stand;
	stand.UseRemote(invitation);
	host = &stand;
	remoteHost = true;
	const auto began = GetTickCount64();
	// Each join is timed from the command to membership.
	const auto join = [&](Player& player, const char* name) {
		const auto joining = GetTickCount64();
		const bool joined = JoinRoom(player, bridgeSeed, kid);
		Step(name, joined, std::to_string(GetTickCount64() - joining) + " ms");
	};
	auto a = StartPlayer("Alice", Seed('a'));
	join(*a, "A joins the remote room with a ticket");
	Step("A sees a server-owned room and is its host", a->Joined() && a->View().serverOwned && a->IsHost(),
		"members=" + std::to_string(a->Joined() ? a->View().members.size() : 0) + " relay=" + a->Room().Network().relay);
	auto b = StartPlayer("Bob", Seed('b'));
	join(*b, "B joins the remote room with a ticket");
	Step("both see 2 members; A is host", WaitFor([&]() { return a->View().members.size() == 2 && b->View().members.size() == 2; }, 15000) &&
		!b->IsHost() && a->IsHost() && b->View().serverOwned);
	room::Action queue; queue.kind = room::ActionKind::Queue; queue.table = 0;
	const auto seated = [&](Player& player) {
		const auto* member = room::FindMember(player.View(), player.View().localMember);
		return member && member->table == 0 && member->seat >= 0;
	};
	Step("A and B take the seats of table 0", SendAction(*a, queue) && WaitFor([&]() { return seated(*a); }, 15000) &&
		SendAction(*b, queue) && WaitFor([&]() { return seated(*b) && a->View().tables[0].p1 && a->View().tables[0].p2; }, 15000));
	PlayMatch(*a, *b);
	StopPlayer(b);
	Step("B left; A sees 1 member", WaitFor([&]() { return a->View().members.size() == 1; }, 15000));
	StopPlayer(a);
	std::cout << "  remote scenario took " << (GetTickCount64() - began) / 1000 << " s" << std::endl;
	return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
	std::cout << std::unitbuf;
	CHECK(argc == 4 || (argc == 5 && (std::wstring(argv[4]) == L"--kill-host" || std::wstring(argv[4]) == L"--full-room" ||
		std::wstring(argv[4]) == L"--refusal-negatives")) || (argc == 6 && std::wstring(argv[4]) == L"--remote"));
	if (argc == 5 && std::wstring(argv[4]) == L"--full-room") globalTimeoutMs = 900000;
	helperPath = argv[1];
	const std::wstring roomHostPath = argv[2];
	ticketTool = argv[3];
	const auto started = GetTickCount64();
	std::thread([]() {
		for (std::uint64_t waited = 0; waited < globalTimeoutMs && !finished; waited += 100) Sleep(100);
		if (finished) return;
		std::cout << "FAIL global timeout after " << globalTimeoutMs / 1000 << " s" << std::endl;
		// The job objects end the room host and every helper with this process.
		std::_Exit(1);
	}).detach();

	const std::string bridgeSeed = Seed('1');
	const auto key = Tool("key " + bridgeSeed);
	CHECK(key.size() == 2);
	const std::string kid = key[1];
	if (argc == 6) {
		const int code = RemoteScenario(Utf8(argv[5]), bridgeSeed, kid);
		finished = true;
		std::cout << (code ? "FAILED" : "PASSED") << " remote scenario" << std::endl;
		return code;
	}

	RoomHostChild child;
	host = &child;
	const json config = {{"room_id", RoomIdHex}, {"name", "Loopback public room"}, {"capacity", 4}, {"build_id", Build},
		{"creator", Tool("ember-id " + Seed('a')).at(0)}, {"bridge_id", Bridge}, {"ticket_key", key[0]}, {"ticket_kid", kid},
		{"helper", Utf8(helperPath)}, {"port", 45800}, {"coordination_port", 45801}};
	if (argc == 5) {
		const std::wstring mode = argv[4];
		json fullConfig = config;
		fullConfig["capacity"] = room::MaximumMembers;
		const int code = mode == L"--full-room" ? FullRoomScenario(roomHostPath, fullConfig, bridgeSeed, kid)
			: mode == L"--refusal-negatives" ? RefusalNegativesScenario(roomHostPath, config, bridgeSeed, kid)
			: KillHostScenario(roomHostPath, config, bridgeSeed, kid);
		finished = true;
		std::cout << (code ? "FAILED" : "PASSED") << (mode == L"--full-room" ? " full-room scenario" :
			mode == L"--refusal-negatives" ? " refusal-negatives scenario" : " kill-host scenario") << std::endl;
		return code;
	}
	CHECK(child.Start(roomHostPath, config.dump()));
	const bool hosted = WaitFor([&]() { return !child.Hosted().is_null() || !child.Running(); }, 30000) && !child.Hosted().is_null();
	Step("room host reports hosted", hosted && !child.Hosted().value("invitation", std::string()).empty(),
		hosted ? "region=" + child.Hosted().value("region", std::string()) + " after " + std::to_string(GetTickCount64() - started) + " ms" : "no hosted line");
	if (!hosted) { finished = true; return 1; }
	Step("first status shows an empty room", WaitFor([&]() { return Members() == 0; }, 5000));

	std::array<std::uint8_t, 16> roomId = {};
	for (std::size_t i = 0; i < 16; ++i) roomId[i] = static_cast<std::uint8_t>(std::stoul(std::string(RoomIdHex).substr(i * 2, 2), nullptr, 16));

	auto a = StartPlayer("Alice", Seed('a'));
	Step("A joins with a ticket", JoinRoom(*a, bridgeSeed, kid));
	Step("A's room is the id the test chose", a->Room().RoomId() == roomId);
	Step("A sees a server-owned room and is its host", a->Joined() && a->View().serverOwned && a->IsHost());
	Step("status shows 1 member", WaitFor([&]() { return Members() == 1; }, 10000));

	auto b = StartPlayer("Bob", Seed('b'));
	Step("B joins with a ticket", JoinRoom(*b, bridgeSeed, kid));
	Step("B is not host; A still is", WaitFor([&]() { return a->View().members.size() == 2 && b->View().members.size() == 2; }, 15000) &&
		!b->IsHost() && a->IsHost() && b->View().serverOwned);
	Step("status shows 2 members", WaitFor([&]() { return Members() == 2; }, 10000));

	{
		// A valid signature over somebody else's endpoint.
		auto stranger = StartPlayer("WrongEndpoint", Seed('e'));
		std::string where;
		const bool refused = Refused(*stranger, [&]() {
			return stranger->Room().JoinPublic(child.Status().value("invitation", std::string()),
				Ticket(bridgeSeed, kid, stranger->emberId, a->Room().LocalIdentity()), Build);
		}, Layer::Helper, where);
		Step("a ticket minted for another endpoint is refused by the helper", refused, where);
		StopPlayer(stranger);
	}
	{
		auto stranger = StartPlayer("NoTicket", Seed('f'));
		std::string where;
		const bool refused = Refused(*stranger, [&]() {
			return stranger->Room().Join(child.Status().value("invitation", std::string()), Build);
		}, Layer::HelperClosed, where);
		Step("a plain join with no ticket gets no answer: the proof does not parse", refused, where);
		StopPlayer(stranger);
	}
	Step("refused joins left 2 members", Members() == 2 && a->View().members.size() == 2);

	room::Action queue; queue.kind = room::ActionKind::Queue; queue.table = 0;
	const auto seated = [&](Player& player) {
		const auto* member = room::FindMember(player.View(), player.View().localMember);
		return member && member->table == 0 && member->seat >= 0;
	};
	Step("A and B take the seats of table 0", SendAction(*a, queue) && WaitFor([&]() { return seated(*a); }, 15000) &&
		SendAction(*b, queue) && WaitFor([&]() { return seated(*b) && a->View().tables[0].p1 && a->View().tables[0].p2; }, 15000));

	PlayMatch(*a, *b);

	room::Action kick; kick.kind = room::ActionKind::Kick; kick.target = b->View().localMember;
	const auto kickStarted = GetTickCount64();
	Step("A kicks B", SendAction(*a, kick) && WaitFor([&]() { return a->View().members.size() == 1; }, 15000));
	const bool bSaw = WaitFor([&]() { return !b->Joined() || b->failed; }, 15000);
	Step("B is removed", bSaw, "B's client: joined=" + std::to_string(b->Joined()) + " error=" + b->peer.client->RoomError() +
		" room state=" + std::to_string(static_cast<int>(b->Room().GetState())) + " after " + std::to_string(GetTickCount64() - kickStarted) + " ms");
	Step("status shows 1 member and B's account banned", WaitFor([&]() {
		const auto status = child.Status();
		const auto banned = status.value("banned", json::array());
		return status.value("members", 99) == 1 && banned.size() == 1 && banned[0] == b->emberId;
	}, 10000));
	const std::string bobEmber = b->emberId;
	StopPlayer(b);

	{
		// The same account from a new helper process, hence a new endpoint,
		// with a ticket that is valid in every other respect.
		Sleep(1500); // The host hands the ban to its helper a second after the kick.
		auto returned = StartPlayer("BobAgain", Seed('b'));
		CHECK(returned->emberId == bobEmber);
		std::string where;
		const bool refused = Refused(*returned, [&]() {
			return returned->Room().JoinPublic(child.Status().value("invitation", std::string()),
				Ticket(bridgeSeed, kid, returned->emberId, returned->Room().LocalIdentity()), Build);
		}, Layer::Helper, where);
		Step("B's banned account is refused from a new endpoint by the helper", refused, where);
		StopPlayer(returned);
	}

	auto c = StartPlayer("Carol", Seed('c'));
	Step("C, another account, is admitted", JoinRoom(*c, bridgeSeed, kid) &&
		WaitFor([&]() { return c->View().members.size() == 2 && a->View().members.size() == 2; }, 15000) && !c->IsHost() && a->IsHost());
	auto e = StartPlayer("Erin", Seed('9'));
	Step("E is admitted", JoinRoom(*e, bridgeSeed, kid) &&
		WaitFor([&]() { return e->View().members.size() == 3 && a->View().members.size() == 3; }, 15000) && !e->IsHost() && a->IsHost());
	Step("status shows 3 members", WaitFor([&]() { return Members() == 3; }, 10000));

	StopPlayer(a);
	Step("A leaves; C, the oldest remaining member, becomes host", WaitFor([&]() {
		return c->View().members.size() == 2 && e->View().members.size() == 2 && c->IsHost() && !e->IsHost();
	}, 30000) && e->View().host == c->View().localMember);
	Step("status shows 2 members", WaitFor([&]() { return Members() == 2; }, 10000));

	StopPlayer(c);
	StopPlayer(e);
	Step("everyone left: status shows 0 members", WaitFor([&]() { return Members() == 0; }, 30000));
	Sleep(2000);
	Step("the empty room's host keeps running", child.Running() && child.Closed().is_null());

	auto d = StartPlayer("Dana", Seed('d'));
	Step("D joins the empty room and becomes host", JoinRoom(*d, bridgeSeed, kid) && d->IsHost());
	Step("status shows 1 member", WaitFor([&]() { return Members() == 1; }, 10000));
	Step("the ban list survived", child.Status().value("banned", json::array()).size() == 1);

	const auto closing = GetTickCount64();
	child.CloseStdin();
	std::optional<DWORD> exitCode;
	WaitFor([&]() { exitCode = child.WaitExit(0); return exitCode.has_value(); }, 10000);
	Step("room host exits 0 within 10 s of stdin closing", exitCode && *exitCode == 0,
		exitCode ? "exit " + std::to_string(*exitCode) + " after " + std::to_string(GetTickCount64() - closing) + " ms" : "still running");
	Step("room host wrote only protocol lines", !child.ProtocolError());

	// The host closes the room in the model before it leaves, so D's snapshot
	// says closed at once rather than after the helper's host-loss grace.
	const bool sawClosed = WaitFor([&]() { return d->View().closed; }, 5000);
	Step("D's snapshot shows the room closed within 5 s", sawClosed, std::to_string(GetTickCount64() - closing) +
		" ms after stdin closed; room state " + std::to_string(static_cast<int>(d->Room().GetState())));

	StopPlayer(d);
	finished = true;
	std::cout << (failures ? "FAILED: " : "PASSED: ") << failures << " failed step(s), " << clientErrors
		<< " client error callback(s), " << (GetTickCount64() - started) / 1000 << " s" << std::endl;
	return failures ? 1 : 0;
}
