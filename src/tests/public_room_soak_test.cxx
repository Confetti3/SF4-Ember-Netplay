// Soak harness for public rooms on a room host elsewhere (a Linux
// sf4e-room-host started by hand, src/roomhost/soak/README.md). Not a test:
// it needs hosts to talk to, so it is built but not registered with ctest.
//
//   PublicRoomSoakTest <sf4-net.exe> <room_ticket.exe> [--members 16] [--minutes 240]
//       [--log soak.csv] [--seed N] [--first-room N] [--match-every SECONDS]
//       [--rejoin-every SECONDS] [--activity X] [--max-<limit> N ...] <invitation> [<invitation> ...]
//   PublicRoomSoakTest --print-config <room_ticket.exe>
//
// Each invitation is one room. Room N (1 for the first invitation, or from
// --first-room; an invitation can also be written N=<invitation>) has the room
// id src/roomhost/soak/soak-hosts.sh gives host N, so the tickets minted here
// are the ones that host admits. Every room is filled with --members simulated
// members, each with its own helper process and Ember ID (the first member is
// the creator the hosts are configured with), and each keeps doing what a
// player does, rate-limited and spread over time: chat, seat changes, fighter
// changes, Ready and Unready, connection checks, a match now and then between
// the two fighters at table 0, and a random member leaving and rejoining every
// few minutes. No GGPO session is started (as in PublicRoomHostTest).
//
// Failures are counted, not fatal. One CSV row per room per minute (counters
// are running totals, latencies are for that minute), an events file beside it
// (<log>.events.log) and a summary at the end. Each room runs on its own thread,
// because every member of a room imports every commit and a real room spreads
// that over 16 PCs. The exit code is non-zero when a room breaks one of the
// limits in Evaluate: it was lost, most members could not stay connected, too
// many actions timed out, chat lagged or lines went missing, matches failed,
// members were dropped, or the client itself was saturated.
#include "../session/IrohMatchSession.hxx"
#include "iroh_integration_fixture.hxx"
#include "../platform/HelperProcess.hxx"
#include "public_room_support.hxx"
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>

using namespace sf4e;
using namespace sf4e::test::publicroom;

namespace {
using Clock = ULONGLONG;
using MatchPhase = session::IrohMatchSession::Phase;
using State = session::IrohRoom::State;

Clock Now() { return GetTickCount64(); }
std::atomic<bool> stopRequested{false};
thread_local std::mt19937_64 rng; // one per thread: every room has its own thread
std::mutex outputMutex;
Clock runStart = 0;
std::ofstream eventsFile;

std::uint64_t Uniform(std::uint64_t low, std::uint64_t high) { return std::uniform_int_distribution<std::uint64_t>(low, high)(rng); }
bool Chance(double probability) { return std::uniform_real_distribution<double>(0, 1)(rng) < probability; }
// seconds -> milliseconds in [low, high]
Clock Seconds(double low, double high) { return static_cast<Clock>(1000 * (low + (high - low) * std::uniform_real_distribution<double>(0, 1)(rng))); }

std::string Hms(Clock ms) {
	char text[32];
	std::snprintf(text, sizeof(text), "%02llu:%02llu:%02llu", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60);
	return text;
}
std::string UtcStamp() {
	const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm parts;
	gmtime_s(&parts, &now);
	char text[40];
	std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
	return text;
}

// One line in the events file; failures also go to the console.
void Event(int room, int member, const std::string& what, bool console = true) {
	char tag[32];
	if (member >= 0) std::snprintf(tag, sizeof(tag), "r%02d m%02d", room, member);
	else std::snprintf(tag, sizeof(tag), "r%02d    ", room);
	const std::string line = "[+" + Hms(Now() - runStart) + "] " + tag + " " + what;
	std::lock_guard<std::mutex> lock(outputMutex);
	if (eventsFile.is_open()) eventsFile << UtcStamp() << ' ' << line << std::endl;
	if (console) std::cout << line << std::endl;
}

// Logs a call that held up the loop, since every member shares it and a stall
// there delays the chat round trips measured on it.
struct SlowCall {
	const char* what; int room, member; Clock began = Now();
	~SlowCall() { const auto took = Now() - began; if (took > 400) Event(room, member, std::string("slow call: ") + what + " took " + std::to_string(took) + " ms", false); }
};

const char* const RejectNames[] = {"None", "Closed", "RoomFull", "AdmissionLocked", "NameTaken", "UnknownMember", "UnknownTable",
	"NotHost", "NotSeated", "AlreadySeated", "AlreadyQueued", "NotQueued", "NotWatching", "InvalidSeat", "InvalidRules",
	"InvalidCapacity", "NotReady", "StaleRoom", "StaleTable", "WrongPhase", "WrongGeneration", "Unauthorized", "DuplicateResult",
	"InvalidChat", "MemberKicked", "TerminalLedgerFull"};
std::string RejectName(room::RejectReason reason) {
	const auto index = static_cast<std::size_t>(reason);
	return index < sizeof(RejectNames) / sizeof(RejectNames[0]) ? RejectNames[index] : "Reason" + std::to_string(index);
}

using Counts = std::map<std::string, std::uint64_t>;
std::string Flat(const Counts& counts) {
	std::string out;
	for (const auto& entry : counts) out += (out.empty() ? "" : ";") + entry.first + "=" + std::to_string(entry.second);
	return out;
}

double Percentile(std::vector<std::uint64_t> values, double fraction) {
	if (values.empty()) return 0;
	std::sort(values.begin(), values.end());
	return static_cast<double>(values[(std::min)(values.size() - 1, static_cast<std::size_t>(fraction * values.size()))]);
}
double Average(const std::vector<std::uint64_t>& values) {
	if (values.empty()) return 0;
	double sum = 0;
	for (const auto value : values) sum += static_cast<double>(value);
	return sum / values.size();
}
std::uint64_t Maximum(const std::vector<std::uint64_t>& values) { return values.empty() ? 0 : *std::max_element(values.begin(), values.end()); }

struct Options {
	std::wstring helper;
	std::size_t members = 16;
	double minutes = 240;
	std::string log = "soak.csv";
	int firstRoom = 1;
	double matchEvery = 240, rejoinEvery = 180;
	// 1 is a quiet lobby (a chat line every 1.5 to 5 minutes and a move every 45 to 150 seconds per
	// member); larger values multiply the rate for stress.
	double activity = 1;
	// The run fails when a room goes past any of these (see Evaluate).
	double maxTimeoutPct = 2, maxChatP95Ms = 5000, maxChatLostPct = 5, maxChatMissingPct = 1, maxMatchFailPct = 25, maxDropsPerMemberHour = 0.25, maxJoinFailPct = 20;
	std::vector<std::pair<int, std::string>> rooms; // room number, invitation
} options;

struct Stats {
	std::uint64_t joins = 0, rejoins = 0, joinFailures = 0, refused = 0, degraded = 0, controlLosses = 0, helperCrashes = 0;
	std::uint64_t actionsSent = 0, actionsAccepted = 0, actionsRejected = 0, actionsSuperseded = 0, actionTimeouts = 0, sendFailures = 0;
	std::uint64_t chatSent = 0, chatSampled = 0, chatSeen = 0, chatLost = 0, clientErrors = 0;
	std::uint64_t chatChecked = 0, chatMissing = 0;
	std::uint64_t fighterSent = 0, fighterRefused = 0, fighterUnseen = 0, roomClosed = 0;
	std::uint64_t probesOk = 0, probesFailed = 0, matchesStarted = 0, matchesOk = 0, matchesFailed = 0;
	Counts rejects, joinFailureReasons, lossReasons, matchFailureSteps;
	// This minute, cleared with each row; the all-run copies feed the summary.
	std::vector<std::uint64_t> joinMs, chatRtt, joinMsAll, chatRttAll, staleSamples;
	std::uint64_t staleMax = 0, staleCount = 0, staleSum = 0;
	// The deepest backlog any member of the room showed this minute.
	std::uint64_t queueMax = 0, queueBytesMax = 0, stagedMax = 0, helperLagMaxMs = 0, helperSilentMaxMs = 0;
};

struct Member;
struct Room;

// One helper process and what hangs off it: the fixture's Player.
struct Session {
	platform::HelperProcess process;
	platform::HelperClient helper;
	test::IrohIntegrationPeer peer;
	std::unique_ptr<session::IrohMatchSession> match;
	bool failed = false, matchFailed = false;
	std::string failure, matchError;
	session::IrohRoom& Room() { return *peer.room; }
	// One tick in the application's order.
	void Pump() {
		if (!peer.room) return;
		if (!peer.configured) { peer.room->Poll(); return; }
		if (failed) return;
		if (!peer.recovery.Tick(*peer.server, *peer.room)) { failure = "recovery_import: " + peer.recovery.Error(); failed = true; return; }
		peer.server->AdvanceCustomRoom(GetTickCount64());
		if (peer.server->Step() != 0 || peer.client->Step() != 0) { failure = "step_failed: " + peer.client->RoomError(); failed = true; return; }
		if (match && !matchFailed && !match->Tick(false)) { matchFailed = true; matchError = match->Error(); }
	}
};

struct Pending {
	room::ActionKind kind;
	Clock sentAt;
	int outcome = 0; // 0 waiting, 1 accepted, 2 rejected, 3 superseded
	room::RejectReason reason = room::RejectReason::None;
	std::string text; // a chat line, kept to learn where it ended up
};

struct AcceptedChat {
	std::string text;
	Clock at;
	Member* sender;
	Clock senderSince;
};

struct ChatSample {
	std::string text;
	Clock sentAt;
	Member* observer;
	Clock observerSince; // the observer's session at the time, so a rejoin voids it
	Member* sender = nullptr;
	Clock senderSince = 0;
};

// What two or three members do together at one table, one at a time per table.
struct Flow {
	enum class Kind { Probe, Ready, Match } kind;
	int step = 0;
	Member* a = nullptr; // the acting fighter, P1 for a match
	Member* b = nullptr;
	std::uint8_t table = 0;
	Clock since = 0, holdUntil = 0;
	std::uint64_t idA = 0, idB = 0, generation = 0;
	bool probeStarted = false, aQueued = false, bQueued = false;
	std::string detail;
};

struct Member {
	enum class Phase { Down, Starting, Identity, WaitTurn, Joining, Registering, Active, Leaving } phase = Phase::Down;
	Room* room = nullptr;
	int index = 0;
	std::string label, seed, emberId;
	std::unique_ptr<Session> s;
	Clock phaseSince = 0, nextAt = 0, joinStarted = 0, activeSince = 0, degradedSince = 0, behindSince = 0;
	Clock nextChatAt = 0, nextActAt = 0;
	bool everJoined = false, planned = false, forceRejoin = false, turnGranted = false, statusAsked = false, closedSeen = false;
	int attempts = 0, leaveStage = 0;
	std::uint64_t chatSeq = 0, probeRequest = 0;
	std::map<std::uint64_t, Pending> pending;
	std::array<std::uint64_t, room::TableCount> matchEnded = {};
	int fighterWanted = -1;
	Clock fighterSentAt = 0;
	bool busy = false;

	bool IsActive() const { return phase == Phase::Active; }
	const room::Snapshot& View() const { return s->peer.client->GetRoomSnapshot(); }
	session::IrohRoom& Room_() { return s->Room(); }
	int HomeTable() const { return index < 2 ? 0 : 1 + index % 3; }

	void Tick(Clock now);
	void StartSession();
	void FailJoin(const std::string& reason, const std::string& detail);
	void Activated(Clock now);
	void BeginLeave(const std::string& why, Clock retryInMs);
	void ControlLoss(const std::string& why);
	void Health(Clock now);
	void Drain(Clock now);
	void Act(Clock now);
	void Chat(Clock now);
	bool Send(room::Action action, std::uint64_t* id = nullptr);
	int Outcome(std::uint64_t id) const { const auto found = pending.find(id); return found == pending.end() ? 3 : found->second.outcome; }
	room::RejectReason Reason(std::uint64_t id) const { const auto found = pending.find(id); return found == pending.end() ? room::RejectReason::None : found->second.reason; }
};

struct Room {
	int number = 0;
	std::string invitation, roomIdHex, kid;
	std::vector<std::unique_ptr<Member>> members;
	Stats st;
	Member* joiner = nullptr;
	bool creatorJoined = false, closing = false, reachedHalf = false, lost = false;
	std::array<std::unique_ptr<Flow>, room::TableCount> flows;
	std::vector<ChatSample> samples;
	std::deque<AcceptedChat> chatLog; // every line the host accepted, newest last
	Clock nextMatchAt = 0, nextRejoinAt = 0, nextSampleAt = 0, started = 0;
	unsigned emptyRows = 0, rows = 0, saturatedRows = 0;
	// Read by the other room threads and the main thread.
	std::atomic<std::size_t> activeNow{0};
	std::atomic<std::uint64_t> helpersNow{0}, helpersWs{0};
	std::atomic<double> helpersCpuPct{0};
	struct LoopTiming { std::uint64_t sum = 0, count = 0, max = 0; } loop;
	Clock lastRowAt = 0, cpuAt = 0;
	std::map<DWORD, std::uint64_t> cpuLast;
	double fractionSum = 0;
	std::uint64_t requestCounter = 0;

	Member* ByMemberId(room::MemberId id) {
		if (!id) return nullptr;
		for (auto& member : members) if (member->IsActive() && member->View().localMember == id) return member.get();
		return nullptr;
	}
	std::size_t ActiveCount() const { std::size_t n = 0; for (const auto& m : members) n += m->IsActive(); return n; }
	void Tick(Clock now);
	void TickFlows(Clock now);
	bool StartFlow(Flow::Kind kind, std::uint8_t table, Member* a, Member* b);
	void EndFlow(std::uint8_t table);
	void FailFlow(Flow& flow, const std::string& step, const std::string& detail);
	int AdvanceFlow(Flow& flow, Clock now); // 0 running, 1 done, 2 failed
	int ProbeLeg(Flow& flow, Member& from, Member& to, Clock now);
	void Sample(Clock now);
	void CheckChat(Clock now);
};

// ---- Member -------------------------------------------------------------

void Member::StartSession() {
	SlowCall slow{"helper start", room->number, index};
	s.reset(new Session());
	s->peer.name = label;
	phase = Phase::Starting;
	phaseSince = Now();
	statusAsked = false;
	turnGranted = false;
	if (!s->process.Start(options.helper, GetCurrentProcessId()) || !s->helper.Start(s->process.Bootstrap())) {
		FailJoin("helper_start", "error " + std::to_string(s->process.LastError()));
		return;
	}
	s->peer.room = std::make_shared<session::IrohRoom>(s->helper);
}

void Member::FailJoin(const std::string& reason, const std::string& detail) {
	auto& st = room->st;
	if (reason == "refused") ++st.refused; else ++st.joinFailures;
	++st.joinFailureReasons[reason];
	Event(room->number, index, "join failed: " + reason + (detail.empty() ? "" : " (" + detail + ")") + " after " +
		std::to_string(Now() - joinStarted) + " ms, attempt " + std::to_string(attempts + 1));
	++attempts;
	const Clock backoff = (std::min<Clock>)(60000, 5000ull << (std::min)(attempts - 1, 4));
	BeginLeave("join failed", backoff + Seconds(0, 3));
}

void Member::Activated(Clock now) {
	phase = Phase::Active;
	phaseSince = activeSince = now;
	attempts = 0;
	auto& st = room->st;
	const auto took = now - joinStarted;
	++st.joins;
	if (everJoined) ++st.rejoins;
	st.joinMs.push_back(took);
	st.joinMsAll.push_back(took);
	Event(room->number, index, std::string(everJoined ? "rejoined" : "joined") + " in " + std::to_string(took) + " ms, " +
		std::to_string(View().members.size()) + " members in view", false);
	everJoined = true;
	planned = false;
	if (index == 0) room->creatorJoined = true;
	if (room->joiner == this) room->joiner = nullptr;
	nextChatAt = now + Seconds(30 / options.activity, 200 / options.activity);
	nextActAt = now + Seconds(1, 6);
	degradedSince = 0;
	matchEnded = {};
	closedSeen = false;
}

// Ends the session (the fixture's StopPlayer, without blocking) and schedules
// the next start retryInMs after the helper is gone.
void Member::BeginLeave(const std::string& why, Clock retryInMs) {
	SlowCall slow{"leave", room->number, index};
	if (phase == Phase::Leaving || phase == Phase::Down) return;
	if (room->joiner == this) room->joiner = nullptr;
	pending.clear();
	busy = false;
	turnGranted = false;
	nextAt = retryInMs;
	if (s) {
		auto& peer = s->peer;
		s->match.reset();
		if (peer.client) peer.client->Disconnect();
		else if (peer.room) peer.room->Leave();
		peer.client.reset();
		peer.server.reset();
		peer.configured = false;
	}
	Event(room->number, index, "leaving: " + why, false);
	phase = Phase::Leaving;
	leaveStage = 0;
	phaseSince = Now();
}

void Member::ControlLoss(const std::string& why) {
	auto& st = room->st;
	++st.controlLosses;
	++st.lossReasons[why.substr(0, why.find(':'))];
	std::string detail = why;
	if (s && s->peer.room) detail += " | room state " + std::to_string(static_cast<int>(s->Room().GetState())) + " error '" + s->Room().Error() + "'";
	Event(room->number, index, "control lost: " + detail + " after " + std::to_string((Now() - activeSince) / 1000) + " s");
	BeginLeave("control lost", Seconds(3, 8));
}

bool Member::Send(room::Action action, std::uint64_t* idOut) {
	if (!IsActive() || !s->peer.client) return false;
	const auto& view = View();
	action.roomEpoch = view.roomEpoch;
	action.revision = view.revision;
	action.tableRevision = view.tables[action.table % room::TableCount].revision;
	std::uint64_t id = 0;
	if (s->peer.client->SendRoomAction(action, &id) != session::SendResult::Queued) { ++room->st.sendFailures; return false; }
	pending[id] = Pending{action.kind, Now(), 0, room::RejectReason::None, action.kind == room::ActionKind::Chat ? action.text : std::string()};
	++room->st.actionsSent;
	if (idOut) *idOut = id;
	return true;
}

void Member::Drain(Clock now) {
	auto& st = room->st;
	SessionClient::ActionReply reply;
	while (s->peer.client->TakeActionReply(reply)) {
		const auto found = pending.find(reply.actionId);
		if (found == pending.end()) continue; // a terminal acknowledgement's own retries
		auto& entry = found->second;
		if (reply.superseded) { entry.outcome = 3; ++st.actionsSuperseded; }
		else if (reply.accepted) {
			entry.outcome = 1; ++st.actionsAccepted;
			if (entry.kind == room::ActionKind::Chat) { room->chatLog.push_back({entry.text, now, this, activeSince}); if (room->chatLog.size() > 400) room->chatLog.pop_front(); }
		}
		else if (reply.reason == room::RejectReason::DuplicateResult) { entry.outcome = 1; entry.reason = reply.reason; ++st.actionsAccepted; }
		else { entry.outcome = 2; entry.reason = reply.reason; ++st.actionsRejected; ++st.rejects[RejectName(reply.reason)]; }
	}
	room::Event event;
	while (s->peer.client->TakeRoomEvent(event)) {
		if (event.kind == room::Event::Kind::MatchEnded && event.table < matchEnded.size()) matchEnded[event.table] = event.matchGeneration;
		else if (event.kind == room::Event::Kind::RoomClosed && !closedSeen) { closedSeen = true; ++st.roomClosed; Event(room->number, index, "room closed event"); }
	}
	for (auto it = pending.begin(); it != pending.end();) {
		const auto age = now - it->second.sentAt;
		if (it->second.outcome == 0 && age > 15000) {
			++st.actionTimeouts;
			Event(room->number, index, "action timeout: kind " + std::to_string(static_cast<int>(it->second.kind)));
			it = pending.erase(it);
		} else if (it->second.outcome != 0 && age > 20000) it = pending.erase(it);
		else ++it;
	}
	// A fighter change is seen when the member's own row shows it.
	if (fighterWanted >= 0) {
		const auto* me = room::FindMember(View(), View().localMember);
		if (me && me->fighter == fighterWanted) fighterWanted = -1;
		else if (now - fighterSentAt > 10000) { ++st.fighterUnseen; fighterWanted = -1; }
	}
}

void Member::Health(Clock now) {
	if (!s->process.IsRunning()) { ++room->st.helperCrashes; ControlLoss("helper_crash"); return; }
	if (s->failed) { ControlLoss(s->failure.empty() ? "pump_failed" : s->failure); return; }
	const auto state = s->Room().GetState();
	if (state == State::Degraded) {
		if (!degradedSince) { degradedSince = now; ++room->st.degraded; Event(room->number, index, "room degraded (control down)"); }
		else if (now - degradedSince > 20000) { ControlLoss("degraded_20s"); return; }
	} else degradedSince = 0;
	if (state == State::Failed || state == State::Idle) { ControlLoss("room_" + std::string(state == State::Failed ? "failed" : "idle") + ": " + s->Room().Error()); return; }
	if (!s->peer.client->IsConnected()) { ControlLoss("client_disconnected: " + s->peer.client->RoomError()); return; }
	const auto& view = View();
	if (!room::FindMember(view, view.localMember)) { ControlLoss("not_a_member"); return; }
	if (view.closed && !closedSeen) { closedSeen = true; ++room->st.roomClosed; Event(room->number, index, "snapshot says the room is closed"); }
}

void Member::Chat(Clock now) {
	std::ostringstream text;
	text << "soak r" << room->number << " m" << index << " #" << ++chatSeq << ' ';
	static const char letters[] = "abcdefghijklmnopqrstuvwxyz     ";
	for (auto n = Uniform(8, 70); n; --n) text << letters[Uniform(0, sizeof(letters) - 2)];
	text << '.'; // never ends in a space
	room::Action action;
	action.kind = room::ActionKind::Chat;
	action.text = text.str();
	if (!Send(action)) return;
	++room->st.chatSent;
	// One message in four is timed to another member's snapshot.
	if (Chance(0.25)) {
		std::vector<Member*> others;
		for (auto& other : room->members) if (other.get() != this && other->IsActive()) others.push_back(other.get());
		if (!others.empty()) {
			auto* observer = others[Uniform(0, others.size() - 1)];
			room->samples.push_back(ChatSample{action.text, Now(), observer, observer->activeSince, this, activeSince});
			++room->st.chatSampled;
		}
	}
}

// One thing a player does, chosen by where the member stands now.
void Member::Act(Clock now) {
	const auto& view = View();
	const auto* me = room::FindMember(view, view.localMember);
	if (!me || view.localTerminalPending) return;
	const auto place = room::PlaceOf(view, view.localMember);
	const auto otherTable = [&]() { return index < 2 ? 0 : 1 + static_cast<int>(Uniform(0, 2)); };
	room::Action action;
	const auto simple = [&](room::ActionKind kind, int table) {
		action.kind = kind;
		action.table = static_cast<std::uint8_t>(table);
		Send(action);
	};
	if (place.kind == room::Place::Kind::Seat) {
		const auto& table = view.tables[place.table];
		auto* opponent = room->ByMemberId(place.seat == 0 ? table.p2 : table.p1);
		const bool idle = (table.phase == room::TablePhase::Waiting || table.phase == room::TablePhase::Idle) && !table.ready[0] && !table.ready[1];
		const auto roll = std::uniform_real_distribution<double>(0, 1)(rng);
		if (roll < 0.30) {
			if (!room::SeatEditable(table, place.seat)) return;
			++room->st.fighterSent;
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions chara{};
			fighterWanted = static_cast<int>(Uniform(0, 34));
			if (me->fighter == fighterWanted) fighterWanted = (fighterWanted + 1) % 35;
			chara.charaID = static_cast<std::uint8_t>(fighterWanted);
			chara.unc_edition = 14;
			fighterSentAt = now;
			if (s->peer.client->PreBattle_SetChara(chara) != session::SendResult::Queued) { ++room->st.fighterRefused; fighterWanted = -1; }
		} else if (roll < 0.55) {
			if (opponent && idle) room->StartFlow(Flow::Kind::Ready, static_cast<std::uint8_t>(place.table), this, opponent);
		} else if (roll < 0.70) {
			if (opponent && idle) room->StartFlow(Flow::Kind::Probe, static_cast<std::uint8_t>(place.table), this, opponent);
		} else if (roll < 0.85) {
			if (idle && !room->flows[place.table]) simple(room::ActionKind::Unqueue, place.table);
		}
	} else if (place.kind == room::Place::Kind::Queue) {
		if (Chance(0.5)) simple(room::ActionKind::Unqueue, place.table);
	} else if (me->status == room::MemberStatus::Watching || me->status == room::MemberStatus::WatchingNext) {
		const auto roll = std::uniform_real_distribution<double>(0, 1)(rng);
		if (roll < 0.40) {
			for (const auto& table : view.tables)
				if (std::find(table.spectators.begin(), table.spectators.end(), view.localMember) != table.spectators.end() ||
					std::find(table.watchingNext.begin(), table.watchingNext.end(), view.localMember) != table.watchingNext.end()) {
					simple(room::ActionKind::Unwatch, table.id);
					break;
				}
		} else if (roll < 0.70) simple(room::ActionKind::Watch, otherTable());
	} else if (index < 2) {
		simple(room::ActionKind::Queue, 0); // table 0 is the match table: only its two fighters come near it
	} else if (index < 8) {
		simple(Chance(0.85) ? room::ActionKind::Queue : room::ActionKind::Watch, HomeTable());
	} else {
		const auto roll = std::uniform_real_distribution<double>(0, 1)(rng);
		if (roll < 0.60) simple(room::ActionKind::Watch, HomeTable());
		else if (roll < 0.85) simple(room::ActionKind::Queue, HomeTable());
	}
}

void Member::Tick(Clock now) {
	switch (phase) {
	case Phase::Down:
		if (!room->closing && now >= nextAt) { joinStarted = now; StartSession(); }
		return;
	case Phase::Starting:
		s->Pump();
		if (s->helper.State() == platform::HelperState::Connected) {
			if (!statusAsked) { statusAsked = s->helper.Send("{\"type\":\"status\"}"); }
			if (statusAsked) { phase = Phase::Identity; phaseSince = now; }
		} else if (now - phaseSince > 15000) FailJoin("helper_connect_timeout", "");
		return;
	case Phase::Identity:
		s->Pump();
		if (!s->Room().LocalIdentity().empty()) { phase = Phase::WaitTurn; phaseSince = now; }
		else if (now - phaseSince > 15000) FailJoin("identity_timeout", "");
		return;
	case Phase::WaitTurn:
		s->Pump();
		if (!turnGranted) return;
		{
			std::vector<std::string> lines;
			SlowCall slow{"ticket and join command", room->number, index};
			if (!RunTool("sign " + Seed('1') + " " + room->kid + " " + Bridge + " " + room->roomIdHex + " " + emberId + " " + s->Room().LocalIdentity(), lines) ||
				lines.empty()) { FailJoin("ticket_tool", ""); return; }
			const auto ticket = nlohmann::json::parse(lines[0], nullptr, false);
			joinStarted = now;
			if (ticket.is_discarded() || !s->Room().JoinPublic(room->invitation, ticket, Build)) { FailJoin("join_command_refused", ""); return; }
			phase = Phase::Joining;
			phaseSince = now;
		}
		return;
	case Phase::Joining: {
		s->Pump();
		const auto state = s->Room().GetState();
		if (state == State::Ready) {
			SessionClient::Callbacks callbacks = {};
			callbacks.data = this;
			callbacks.OnError = [](SessionClient::ErrorType, SessionClient* const, const SessionClient::Callbacks& self) {
				++static_cast<Member*>(self.data)->room->st.clientErrors;
			};
			if (!test::ConfigureIrohIntegrationPeer(s->peer, callbacks, Build, 0, static_cast<std::uint8_t>(room::MaximumMembers))) { FailJoin("client_not_attached", ""); return; }
			phase = Phase::Registering;
			phaseSince = now;
		} else if (state == State::Failed || state == State::Idle) {
			const auto reason = s->Room().FailureReason();
			FailJoin(s->Room().Error() == "join_failed" ? (reason.empty() ? "join_failed_no_reason" : reason) : "room_" + s->Room().Error(),
				"state " + std::to_string(static_cast<int>(state)));
		} else if (now - phaseSince > 45000) FailJoin("join_timeout", "state " + std::to_string(static_cast<int>(state)));
		return;
	}
	case Phase::Registering: {
		s->Pump();
		const auto& client = s->peer.client;
		const auto& view = View();
		const bool joined = view.localMember && room::FindMember(view, view.localMember) != nullptr;
		if (joined) Activated(now);
		else if (s->failed) FailJoin("registration_failed", s->failure);
		else if (client->JoinRejection()) FailJoin(std::string("rejected_") + SessionClient::PublicJoinRejectionKey(*client->JoinRejection()), "");
		else if (now - phaseSince > 30000) FailJoin("registration_timeout", s->peer.recovery.Error());
		return;
	}
	case Phase::Active:
		s->Pump();
		Drain(now);
		Health(now);
		if (phase != Phase::Active) return;
		if (forceRejoin) { forceRejoin = false; BeginLeave("rejoin after a failed table flow", Seconds(2, 6)); return; }
		if (now >= nextChatAt) { nextChatAt = now + Seconds(90 / options.activity, 300 / options.activity); Chat(now); }
		if (now >= nextActAt) { nextActAt = now + Seconds(45 / options.activity, 150 / options.activity); if (!busy) Act(now); }
		return;
	case Phase::Leaving:
		s->Pump();
		if (leaveStage == 0) {
			const auto state = s->peer.room ? s->Room().GetState() : State::Idle;
			if (state == State::Idle || state == State::Failed || !s->process.IsRunning() ||
				now - phaseSince > session::IrohRoom::LeaveTimeoutMs + 2000) {
				s->helper.Send("{\"type\":\"shutdown\"}");
				leaveStage = 1;
				phaseSince = now;
			}
		} else if (!s->process.IsRunning() || now - phaseSince > 3000) {
			SlowCall slow{"helper stop", room->number, index};
			s->process.Stop(0);
			s.reset();
			phase = Phase::Down;
			nextAt = now + nextAt; // nextAt held the delay
		}
		return;
	}
}

// ---- Flows ---------------------------------------------------------------

bool Room::StartFlow(Flow::Kind kind, std::uint8_t table, Member* a, Member* b) {
	if (flows[table] || !a || !b || a->busy || b->busy) return false;
	flows[table].reset(new Flow());
	auto& flow = *flows[table];
	flow.kind = kind; flow.table = table; flow.a = a; flow.b = b; flow.since = Now();
	a->busy = b->busy = true;
	if (kind == Flow::Kind::Match) ++st.matchesStarted;
	return true;
}

void Room::EndFlow(std::uint8_t table) {
	auto& flow = *flows[table];
	flow.a->busy = flow.b->busy = false;
	flows[table].reset();
}

void Room::FailFlow(Flow& flow, const std::string& step, const std::string& detail) {
	const char* kinds[] = {"probe", "ready", "match"};
	const std::string name = std::string(kinds[static_cast<int>(flow.kind)]) + ":" + step;
	if (flow.kind == Flow::Kind::Match) { ++st.matchesFailed; ++st.matchFailureSteps[step]; }
	Event(number, -1, "table " + std::to_string(flow.table) + " " + name + " failed: " + detail);
	for (auto* member : {flow.a, flow.b}) {
		if (!member->IsActive()) continue;
		if (member->s) member->s->match.reset();
		// A table left half-way is cleared by the members leaving it.
		if (flow.kind == Flow::Kind::Match || flow.kind == Flow::Kind::Ready) member->forceRejoin = true;
	}
}

// One leg of the connection check, like the fixture's Probe.
int Room::ProbeLeg(Flow& flow, Member& from, Member& to, Clock now) {
	if (!flow.probeStarted) {
		const auto peer = to.Room_().LocalIdentity();
		const auto control = from.Room_().ConnectionForIdentity(peer);
		if (control == 0 || from.Room_().PeerIncarnation(control) == 0) {
			if (now - flow.since > 15000) { flow.detail = "peer endpoint unknown to the room"; return 2; }
			return 0;
		}
		if (!from.Room_().RequestProbe(peer, ++requestCounter, from.View().tables[flow.table].revision)) { flow.detail = "request refused"; return 2; }
		flow.probeStarted = true;
		flow.since = now;
		return 0;
	}
	const auto& probe = from.Room_().Probe();
	if (probe.status == "checking") {
		if (now - flow.since > 40000) { flow.detail = "no result in 40 s"; return 2; }
		return 0;
	}
	flow.detail = "status=" + probe.status + " route=" + probe.route + " replies=" + std::to_string(probe.samples) + "/" +
		std::to_string(probe.samples + probe.lost) + " failure=" + std::to_string(probe.failureReason);
	return (probe.status == "ready" || probe.status == "complete") && probe.samples >= 80 && probe.recommended >= 0 ? 1 : 2;
}

int Room::AdvanceFlow(Flow& flow, Clock now) {
	Member& a = *flow.a;
	Member& b = *flow.b;
	if (!a.IsActive() || !b.IsActive()) { flow.detail = "a member left"; return 2; }
	const auto tableId = flow.table;
	const auto fail = [&](const std::string& what) { flow.detail = what; return 2; };
	const auto settled = [&](Member& member, std::uint64_t id, Clock limit, std::string& why) {
		const int outcome = member.Outcome(id);
		if (outcome == 1 || outcome == 3) return 1;
		if (outcome == 2) { why = "rejected " + RejectName(member.Reason(id)); return 2; }
		if (now - flow.since > limit) { why = "no reply in " + std::to_string(limit / 1000) + " s"; return 2; }
		return 0;
	};
	room::Action action;
	action.table = tableId;
	switch (flow.kind) {
	case Flow::Kind::Probe: {
		const int r = ProbeLeg(flow, a, b, now);
		if (r == 0) return 0;
		++(r == 1 ? st.probesOk : st.probesFailed);
		if (r == 2) Event(number, -1, "table " + std::to_string(tableId) + " connection check failed: " + flow.detail);
		return 1;
	}
	case Flow::Kind::Ready: {
		std::string why;
		if (flow.step == 0) {
			action.kind = room::ActionKind::Ready; action.inputDelay = 2;
			if (!a.Send(action, &flow.idA)) return fail("send failed");
			flow.step = 1; flow.since = now;
		} else if (flow.step == 1) {
			const int r = settled(a, flow.idA, 15000, why);
			if (r == 2) { flow.step = 5; flow.detail = why; return 1; } // nothing to take back
			if (r == 1) { flow.step = 2; flow.holdUntil = now + Seconds(1, 3); }
		} else if (flow.step == 2) {
			if (now >= flow.holdUntil) {
				action.kind = room::ActionKind::Unready;
				if (!a.Send(action, &flow.idA)) return fail("unready send failed");
				flow.step = 3; flow.since = now;
			}
		} else if (flow.step == 3) {
			const int r = settled(a, flow.idA, 15000, why);
			if (r == 2) return fail("unready " + why);
			if (r == 1) return 1;
		} else return 1;
		return 0;
	}
	case Flow::Kind::Match: {
		std::string why;
		switch (flow.step) {
		case 0: case 1: {
			const int r = flow.step == 0 ? ProbeLeg(flow, a, b, now) : ProbeLeg(flow, b, a, now);
			if (r == 0) return 0;
			++(r == 1 ? st.probesOk : st.probesFailed);
			// A failed check is recorded and the match goes on, as the fixture does.
			if (r == 2) Event(number, -1, std::string("table ") + std::to_string(tableId) + " match check " + (flow.step == 0 ? "A to B" : "B to A") + " failed: " + flow.detail);
			flow.probeStarted = false; flow.since = now; ++flow.step;
			return 0;
		}
		case 2:
			a.s->match.reset(new session::IrohMatchSession(*a.s->peer.client, a.s->peer.room));
			b.s->match.reset(new session::IrohMatchSession(*b.s->peer.client, b.s->peer.room));
			action.kind = room::ActionKind::Ready; action.inputDelay = 2;
			if (!a.Send(action, &flow.idA) || !b.Send(action, &flow.idB)) return fail("ready send failed");
			flow.step = 3; flow.since = now;
			return 0;
		case 3: {
			const int ra = settled(a, flow.idA, 15000, why), rb = settled(b, flow.idB, 15000, why);
			if (ra == 2 || rb == 2) return fail("ready " + why);
			if (ra == 1 && rb == 1) { flow.step = 4; flow.since = now; }
			return 0;
		}
		case 4:
			if (a.s->matchFailed || b.s->matchFailed) return fail("match setup: " + a.s->matchError + " " + b.s->matchError);
			if (a.s->match->GetPhase() == MatchPhase::Started && b.s->match->GetPhase() == MatchPhase::Started) {
				flow.generation = a.s->match->Generation();
				flow.step = 5; flow.holdUntil = now + Seconds(2, 6);
			} else if (now - flow.since > 60000) return fail("not started in 60 s: phases " + std::to_string(static_cast<int>(a.s->match->GetPhase())) +
				"/" + std::to_string(static_cast<int>(b.s->match->GetPhase())));
			return 0;
		case 5:
			if (a.s->matchFailed || b.s->matchFailed) return fail("match failed while running: " + a.s->matchError + " " + b.s->matchError);
			if (now < flow.holdUntil) return 0;
			a.s->match->End(); b.s->match->End();
			action.kind = room::ActionKind::RecordResult; action.matchGeneration = flow.generation; action.result = room::MatchResult::P1Win;
			if (!a.Send(action, &flow.idA) || !b.Send(action, &flow.idB)) return fail("result send failed");
			flow.step = 6; flow.since = now;
			return 0;
		case 6: {
			const int ra = settled(a, flow.idA, 20000, why), rb = settled(b, flow.idB, 20000, why);
			if (ra == 2 || rb == 2) return fail("result " + why);
			if (ra == 1 && rb == 1) { flow.step = 7; flow.since = now; }
			return 0;
		}
		default: {
			const bool idle = a.s->match->GetPhase() == MatchPhase::Idle && b.s->match->GetPhase() == MatchPhase::Idle;
			if (!flow.aQueued && a.matchEnded[tableId] == flow.generation && idle)
				flow.aQueued = a.s->peer.client->AcknowledgeTerminal(tableId, flow.generation) == session::SendResult::Queued;
			if (!flow.bQueued && b.matchEnded[tableId] == flow.generation && idle)
				flow.bQueued = b.s->peer.client->AcknowledgeTerminal(tableId, flow.generation) == session::SendResult::Queued;
			if (flow.aQueued && flow.bQueued && !a.View().terminalPending[tableId] && !b.View().terminalPending[tableId]) {
				a.s->match.reset(); b.s->match.reset();
				++st.matchesOk;
				Event(number, -1, "table " + std::to_string(tableId) + " match complete, score " +
					std::to_string(a.View().tables[tableId].score[0]) + "-" + std::to_string(a.View().tables[tableId].score[1]), false);
				return 1;
			}
			if (now - flow.since > 45000) return fail(std::string("not acknowledged in 45 s: ended ") + (a.matchEnded[tableId] == flow.generation ? "A" : "-") +
				(b.matchEnded[tableId] == flow.generation ? "B" : "-") + " queued " + (flow.aQueued ? "A" : "-") + (flow.bQueued ? "B" : "-") +
				(idle ? "" : " sessions not idle"));
			return 0;
		}
		}
	}
	}
	return 1;
}

void Room::TickFlows(Clock now) {
	for (std::uint8_t table = 0; table < flows.size(); ++table) {
		if (!flows[table]) continue;
		auto& flow = *flows[table];
		const int result = AdvanceFlow(flow, now);
		if (result == 0) continue;
		if (result == 2) {
			static const char* const names[] = {"probe_a_to_b", "probe_b_to_a", "ready_send", "ready_reply", "game_start", "game_running", "result", "acknowledge"};
			FailFlow(flow, flow.kind == Flow::Kind::Match ? names[(std::min)(flow.step, 7)] : "step" + std::to_string(flow.step), flow.detail);
		}
		EndFlow(table);
	}
}

// ---- Room ----------------------------------------------------------------

// Every accepted line should reach every member that was in the room when it
// was said: not just the few that are timed. Each line is checked once per
// member, when it is 20 to 80 s old (a row is written every 60 s). The room
// drops a member's lines when they leave, so a line whose sender has since
// left (a planned rejoin counts) is not expected in any snapshot.
void Room::CheckChat(Clock now) {
	for (auto& member : members) {
		if (!member->IsActive()) continue;
		const auto& chat = member->View().chat;
		for (const auto& line : chatLog) {
			if (line.at + 80000 <= now || line.at + 20000 > now || line.at < member->activeSince + 10000) continue;
			if (!line.sender->IsActive() || line.sender->activeSince != line.senderSince) continue;
			++st.chatChecked;
			if (std::any_of(chat.begin(), chat.end(), [&](const room::ChatMessage& value) { return value.text == line.text; })) continue;
			++st.chatMissing;
			const bool senderHas = line.sender->IsActive() && line.sender->activeSince == line.senderSince &&
				std::any_of(line.sender->View().chat.begin(), line.sender->View().chat.end(), [&](const room::ChatMessage& value) { return value.text == line.text; });
			if (st.chatMissing <= 20) Event(number, member->index, "chat line missing from a member's snapshot " + std::to_string((now - line.at) / 1000) +
				" s after it was accepted: \"" + line.text.substr(0, 40) + "\" (length " + std::to_string(line.text.size()) + "), sender " +
				std::to_string(line.sender->index) + (senderHas ? " has it" : " does not have it") + ", this member sees " + std::to_string(chat.size()) +
				" lines, member joined " + std::to_string((now - member->activeSince) / 1000) + " s ago");
		}
	}
}

void Room::Sample(Clock now) {
	std::uint64_t newest = 0;
	for (auto& member : members) if (member->IsActive()) newest = (std::max)(newest, member->View().revision);
	for (auto& member : members) {
		if (!member->IsActive()) { member->behindSince = 0; continue; }
		const auto received = member->s->peer.client->RoomSnapshotReceivedMs();
		if (received) {
			const std::uint64_t age = now >= received ? now - received : 0;
			st.staleSum += age; ++st.staleCount; st.staleMax = (std::max)(st.staleMax, age);
		}
		if (member->View().revision < newest) { if (!member->behindSince) member->behindSince = now; }
		else member->behindSince = 0;
		const auto recovery = member->s->Room().RecoveryState();
		st.queueMax = (std::max<std::uint64_t>)(st.queueMax, recovery.clientQueueMessages + recovery.serverQueueMessages);
		st.queueBytesMax = (std::max<std::uint64_t>)(st.queueBytesMax, recovery.queuedBytes);
		st.stagedMax = (std::max<std::uint64_t>)(st.stagedMax, recovery.stagedCheckpoints);
		const auto& load = member->s->Room().HelperLoad();
		st.helperLagMaxMs = (std::max<std::uint64_t>)(st.helperLagMaxMs, load.actorTickLagMaxUs / 1000);
		if (load.lastEventMs) st.helperSilentMaxMs = (std::max<std::uint64_t>)(st.helperSilentMaxMs, now >= load.lastEventMs ? now - load.lastEventMs : 0);
	}
}

void Room::Tick(Clock now) {
	// Admissions are one at a time per room, the creator first.
	if (!joiner && !closing) {
		Member* next = nullptr;
		for (auto& member : members)
			if (member->phase == Member::Phase::WaitTurn && (member->index == 0 || creatorJoined) && (!next || member->index < next->index)) next = member.get();
		if (next) { joiner = next; next->turnGranted = true; }
	}
	if (joiner && joiner->phase != Member::Phase::WaitTurn && joiner->phase != Member::Phase::Joining &&
		joiner->phase != Member::Phase::Registering) joiner = nullptr;
	TickFlows(now);
	if (closing) return;

	// Chat round trips: a sampled message is seen when it is in another
	// member's snapshot.
	for (auto it = samples.begin(); it != samples.end();) {
		bool done = false;
		if (!it->observer->IsActive() || it->observer->activeSince != it->observerSince) done = true; // the observer's session ended
		else if (it->sender && (!it->sender->IsActive() || it->sender->activeSince != it->senderSince)) done = true; // the room dropped the leaver's lines
		else {
			for (const auto& chat : it->observer->View().chat)
				if (chat.text == it->text) {
					const auto rtt = now - it->sentAt;
					st.chatRtt.push_back(rtt); st.chatRttAll.push_back(rtt); ++st.chatSeen;
					done = true;
					break;
				}
			if (!done && now - it->sentAt > 30000) {
				++st.chatLost;
				// Where the line is, to tell a lost line from a late snapshot.
				const auto has = [&](Member* member) {
					if (!member->IsActive() || member->activeSince != (member == it->sender ? it->senderSince : it->observerSince)) return std::string("gone");
					for (const auto& chat : member->View().chat) if (chat.text == it->text) return std::string("yes");
					return std::string("no");
				};
				const auto& view = it->observer->View();
				Event(number, it->observer->index, "chat not seen within 30 s: " + it->text.substr(0, 24) + " | sender has it: " + has(it->sender) +
					", observer has it: " + has(it->observer) + ", observer chat lines " + std::to_string(view.chat.size()) + " revision " +
					std::to_string(view.revision) + " snapshot age " + std::to_string(it->observer->s->peer.client->RoomSnapshotReceivedMs() <= now ? now - it->observer->s->peer.client->RoomSnapshotReceivedMs() : 0) + " ms");
				done = true;
			}
		}
		it = done ? samples.erase(it) : it + 1;
	}

	if (now >= nextSampleAt) { nextSampleAt = now + 1000; Sample(now); }

	// A match at table 0 between its two fighters, when both are free.
	if (now >= nextMatchAt && ActiveCount() >= 2) {
		nextMatchAt = now + Seconds(options.matchEvery * 0.5, options.matchEvery * 1.5);
		Member* anyone = nullptr;
		for (auto& member : members) if (member->IsActive()) { anyone = member.get(); break; }
		const auto& table = anyone->View().tables[0];
		Member* a = ByMemberId(table.p1);
		Member* b = ByMemberId(table.p2);
		if (a && b && !flows[0] && !a->busy && !b->busy && table.phase == room::TablePhase::Waiting && !table.ready[0] && !table.ready[1] &&
			table.spectators.empty() && table.queue.empty() && !a->View().localTerminalPending && !b->View().localTerminalPending)
			StartFlow(Flow::Kind::Match, 0, a, b);
		else nextMatchAt = now + 20000; // the fighters are not both seated and free yet
	}

	// Every few minutes one member leaves and rejoins.
	if (now >= nextRejoinAt && ActiveCount() >= 3) {
		nextRejoinAt = now + Seconds(options.rejoinEvery * 0.5, options.rejoinEvery * 1.5);
		std::vector<Member*> candidates;
		for (auto& member : members)
			if (member->IsActive() && !member->busy && !member->forceRejoin && now - member->activeSince > 30000) candidates.push_back(member.get());
		if (!candidates.empty()) {
			auto* chosen = candidates[Uniform(0, candidates.size() - 1)];
			chosen->planned = true;
			chosen->BeginLeave("planned rejoin", Seconds(2, 10));
		}
	}
}

// ---- Process accounting ----------------------------------------------------

struct Memory { std::size_t helpers = 0; std::uint64_t workingSet = 0; };
Memory HelperMemory(const Room& room) {
	Memory total;
	for (const auto& member : room.members) {
		if (!member->s || !member->s->process.IsRunning()) continue;
		++total.helpers;
		if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, member->s->process.Bootstrap().helperPid)) {
			PROCESS_MEMORY_COUNTERS counters = {sizeof(counters)};
			if (GetProcessMemoryInfo(process, &counters, sizeof(counters))) total.workingSet += counters.WorkingSetSize;
			CloseHandle(process);
		}
	}
	return total;
}
double Mb(std::uint64_t bytes) { return bytes / 1048576.0; }
std::uint64_t SoakWorkingSet() {
	PROCESS_MEMORY_COUNTERS counters = {sizeof(counters)};
	return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.WorkingSetSize : 0;
}
// Processor time of a process in milliseconds.
std::uint64_t ProcessTime(HANDLE process) {
	FILETIME created, exited, kernel, user;
	if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
	return ((static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
		(static_cast<std::uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime)) / 10000;
}
// The room's helpers' processor time since the last call, as percent of one core.
double RoomHelperCpu(Room& room, Clock now) {
	std::uint64_t used = 0;
	for (const auto& member : room.members) {
		if (!member->s || !member->s->process.IsRunning()) continue;
		const DWORD pid = member->s->process.Bootstrap().helperPid;
		if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
			const auto total = ProcessTime(process);
			used += total - (std::min)(total, room.cpuLast[pid]);
			room.cpuLast[pid] = total;
			CloseHandle(process);
		}
	}
	const double wall = static_cast<double>(now - room.cpuAt);
	room.cpuAt = now;
	return wall > 0 ? 100.0 * used / wall : 0;
}

// Every room thread writes its own rows; the process-wide figures come from the
// main thread and from what the other rooms last published.
std::mutex csvMutex;
std::atomic<double> processCpuPct{0};
std::vector<Room*> allRooms;

const char* const CsvHeader =
	"time_utc,elapsed_s,room,members_active,members_target,view_members_min,view_members_max,behind_members,"
	"joins_total,rejoins_total,join_failures_total,refused_total,degraded_total,control_losses_total,helper_crashes_total,"
	"join_ms_avg,join_ms_max,actions_sent_total,actions_accepted_total,actions_rejected_total,action_timeouts_total,send_failures_total,"
	"client_errors_total,rejects_by_reason,join_failures_by_reason,control_losses_by_reason,"
	"chat_sent_total,chat_sampled_total,chat_seen_total,chat_lost_total,chat_lines_checked_total,chat_lines_missing_total,chat_rtt_avg_ms,chat_rtt_max_ms,"
	"stale_avg_ms,stale_max_ms,probes_ok_total,probes_failed_total,matches_started_total,matches_ok_total,matches_failed_total,"
	"match_failures_by_step,fighter_changes_total,fighter_unseen_total,"
	"room_helpers,room_helpers_ws_mb,room_helpers_cpu_pct,all_helpers,all_helpers_ws_mb,all_helpers_cpu_pct,soak_ws_mb,client_cpu_pct,"
	"loop_avg_ms,loop_max_ms,loop_busy_pct,"
	"backlog_msgs_max,backlog_bytes_max,staged_checkpoints_max,helper_lag_ms_max,helper_silent_ms_max";

void WriteRow(std::ofstream& csv, Room& room, Clock now) {
	auto& st = room.st;
	room.CheckChat(now);
	std::size_t viewMin = SIZE_MAX, viewMax = 0, behind = 0;
	for (auto& member : room.members) {
		if (!member->IsActive()) continue;
		viewMin = (std::min)(viewMin, member->View().members.size());
		viewMax = (std::max)(viewMax, member->View().members.size());
		if (member->behindSince && now - member->behindSince > 5000) ++behind;
	}
	if (viewMin == SIZE_MAX) viewMin = 0;
	const auto mine = HelperMemory(room);
	const double helperCpu = RoomHelperCpu(room, now);
	room.helpersNow = mine.helpers; room.helpersWs = mine.workingSet; room.helpersCpuPct = helperCpu;
	std::uint64_t allHelpers = 0, allWs = 0;
	double allCpu = 0;
	for (const auto* other : allRooms) { allHelpers += other->helpersNow; allWs += other->helpersWs; allCpu += other->helpersCpuPct; }
	const auto active = room.ActiveCount();
	const double fraction = static_cast<double>(active) / room.members.size();
	const double busy = 100.0 * room.loop.sum / (std::max<Clock>)(1, now - room.lastRowAt);
	++room.rows;
	room.fractionSum += fraction;
	if (busy > 90) ++room.saturatedRows;
	if (fraction >= 0.5) room.reachedHalf = true;
	room.emptyRows = active == 0 ? room.emptyRows + 1 : 0;
	if (room.reachedHalf && room.emptyRows >= 3 && !room.lost) { room.lost = true; Event(room.number, -1, "ROOM LOST: no member connected for 3 minutes"); }
	if (!room.reachedHalf && now - room.started > 15 * 60000ull && !room.lost) { room.lost = true; Event(room.number, -1, "ROOM LOST: never filled to half in 15 minutes"); }
	std::lock_guard<std::mutex> lock(csvMutex);
	csv << std::fixed << std::setprecision(1) << UtcStamp() << ',' << (now - runStart) / 1000 << ',' << room.number << ',' << active << ',' << room.members.size()
		<< ',' << viewMin << ',' << viewMax << ',' << behind
		<< ',' << st.joins << ',' << st.rejoins << ',' << st.joinFailures << ',' << st.refused << ',' << st.degraded << ',' << st.controlLosses << ',' << st.helperCrashes
		<< ',' << Average(st.joinMs) << ',' << Maximum(st.joinMs)
		<< ',' << st.actionsSent << ',' << st.actionsAccepted << ',' << st.actionsRejected << ',' << st.actionTimeouts << ',' << st.sendFailures
		<< ',' << st.clientErrors << ",\"" << Flat(st.rejects) << "\",\"" << Flat(st.joinFailureReasons) << "\",\"" << Flat(st.lossReasons) << '"'
		<< ',' << st.chatSent << ',' << st.chatSampled << ',' << st.chatSeen << ',' << st.chatLost << ',' << st.chatChecked << ',' << st.chatMissing << ',' << Average(st.chatRtt) << ',' << Maximum(st.chatRtt)
		<< ',' << (st.staleCount ? static_cast<double>(st.staleSum) / st.staleCount : 0.0) << ',' << st.staleMax
		<< ',' << st.probesOk << ',' << st.probesFailed << ',' << st.matchesStarted << ',' << st.matchesOk << ',' << st.matchesFailed
		<< ",\"" << Flat(st.matchFailureSteps) << '"' << ',' << st.fighterSent << ',' << st.fighterUnseen
		<< ',' << mine.helpers << ',' << Mb(mine.workingSet) << ',' << helperCpu << ',' << allHelpers << ',' << Mb(allWs) << ',' << allCpu
		<< ',' << Mb(SoakWorkingSet()) << ',' << processCpuPct.load()
		<< ',' << (room.loop.count ? static_cast<double>(room.loop.sum) / room.loop.count : 0.0) << ',' << room.loop.max << ',' << busy
		<< ',' << st.queueMax << ',' << st.queueBytesMax << ',' << st.stagedMax << ',' << st.helperLagMaxMs << ',' << st.helperSilentMaxMs << std::endl;
	st.joinMs.clear(); st.chatRtt.clear();
	st.staleSum = st.staleCount = st.staleMax = 0;
	st.queueMax = st.queueBytesMax = st.stagedMax = st.helperLagMaxMs = st.helperSilentMaxMs = 0;
	room.loop = {};
	room.lastRowAt = now;
}

// One room, on its own thread: every member of a real room runs on a PC of its
// own, and a passive replica imports every commit, so a single thread for many
// rooms would be slower than the people it stands in for. Also ends the room
// cleanly (every member out, every helper ended) when the time is up.
void RoomMain(Room* roomPtr, Clock endAt, std::ofstream* csv, std::uint64_t seed) {
	Room& room = *roomPtr;
	rng.seed(seed + 7919ull * room.number);
	Clock nextRow = runStart + 60000;
	room.lastRowAt = room.cpuAt = runStart;
	while (!stopRequested && Now() < endAt) {
		const auto began = Now();
		for (auto& member : room.members) member->Tick(began);
		room.Tick(began);
		room.activeNow = room.ActiveCount();
		if (began >= nextRow) {
			nextRow += 60000;
			WriteRow(*csv, room, began);
		}
		const auto spent = Now() - began;
		room.loop.sum += spent; ++room.loop.count; room.loop.max = (std::max<std::uint64_t>)(room.loop.max, spent);
		Sleep(spent < 5 ? static_cast<DWORD>(5 - spent) : 1);
	}
	room.closing = true;
	for (auto& flow : room.flows) if (flow) {
		// A match cut off by the end of the run is neither a success nor a failure.
		if (flow->kind == Flow::Kind::Match) --room.st.matchesStarted;
		flow->a->busy = flow->b->busy = false;
		flow.reset();
	}
	for (auto& member : room.members) member->BeginLeave("shutdown", 0);
	const Clock stopBy = Now() + 40000;
	for (;;) {
		bool pendingLeave = false;
		const auto now = Now();
		for (auto& member : room.members) if (member->phase != Member::Phase::Down) { member->Tick(now); pendingLeave = true; }
		if (!pendingLeave || Now() > stopBy) break;
		Sleep(5);
	}
	room.activeNow = 0;
	WriteRow(*csv, room, Now());
}

BOOL WINAPI OnConsole(DWORD) { stopRequested = true; return TRUE; }

// Prints what the hosts must be configured with (src/roomhost/soak/soak-hosts.sh
// carries the same values) so the two sides can be compared.
int PrintConfig(const std::wstring& tool) {
	ticketTool = tool;
	const auto key = Tool("key " + Seed('1'));
	CHECK(key.size() == 2);
	std::cout << "bridge_id=" << Bridge << "\nbuild_id=" << Build << "\nroom_id_prefix=" << std::string(RoomIdHex).substr(0, 30)
		<< " (room N appends its number as two hex digits)\nticket_key=" << key[0] << "\nticket_kid=" << key[1]
		<< "\ncreator=" << Tool("ember-id " + Seed('a')).at(0) << std::endl;
	return 0;
}

std::string RoomIdFor(int number) {
	char suffix[4];
	std::snprintf(suffix, sizeof(suffix), "%02x", number & 0xff);
	return std::string(RoomIdHex).substr(0, 30) + suffix;
}

struct Check { std::string name, measured, limit; bool ok; };
std::string Num(double value, int digits = 1) {
	std::ostringstream text;
	text << std::fixed << std::setprecision(digits) << value;
	return text.str();
}

// The criteria a run has to meet. A room that stayed up but dropped its actions,
// lagged its chat by seconds or lost its members is not a pass.
std::vector<Check> Evaluate(const Room& room, double hours) {
	const auto& st = room.st;
	std::vector<Check> checks;
	const auto add = [&](const std::string& name, double measured, double limit, bool ok, const char* unit = "") {
		checks.push_back({name, Num(measured) + unit, "<= " + Num(limit) + unit, ok});
	};
	const double average = room.rows ? room.fractionSum / room.rows : 0;
	checks.push_back({"room stayed up", room.lost ? "lost" : "up", "up", !room.lost});
	add("members connected on average", 100 * average, 50, average >= 0.5, "%");
	checks.back().limit = ">= 50.0%";
	const double timeoutPct = st.actionsSent ? 100.0 * st.actionTimeouts / st.actionsSent : 0;
	add("actions timed out", timeoutPct, options.maxTimeoutPct, timeoutPct <= options.maxTimeoutPct, "%");
	if (st.chatRttAll.size() >= 5) {
		const double p95 = Percentile(st.chatRttAll, 0.95);
		add("chat round trip p95", p95, options.maxChatP95Ms, p95 <= options.maxChatP95Ms, " ms");
	}
	if (st.chatSampled >= 5) {
		const double lostPct = 100.0 * st.chatLost / st.chatSampled;
		add("timed chat lines never seen", lostPct, options.maxChatLostPct, lostPct <= options.maxChatLostPct, "%");
	}
	if (st.chatChecked >= 20) {
		const double missingPct = 100.0 * st.chatMissing / st.chatChecked;
		add("chat lines missing from members' snapshots", missingPct, options.maxChatMissingPct, missingPct <= options.maxChatMissingPct, "%");
	}
	if (st.matchesStarted >= 1) {
		const double failedPct = 100.0 * st.matchesFailed / st.matchesStarted;
		add("matches that failed or never started", failedPct, options.maxMatchFailPct, failedPct <= options.maxMatchFailPct, "%");
	}
	const double drops = static_cast<double>(st.controlLosses + st.helperCrashes) / (room.members.size() * (std::max)(hours, 0.25));
	add("members dropped per member-hour", drops, options.maxDropsPerMemberHour, drops <= options.maxDropsPerMemberHour);
	const double joinFailPct = st.joins + st.joinFailures ? 100.0 * st.joinFailures / (st.joins + st.joinFailures) : 0;
	add("join attempts that failed", joinFailPct, options.maxJoinFailPct, joinFailPct <= options.maxJoinFailPct, "%");
	const double saturated = room.rows ? 100.0 * room.saturatedRows / room.rows : 0;
	add("minutes the client loop was saturated", saturated, 20, saturated <= 20, "%");
	return checks;
}

void PrintSummary(const std::vector<std::unique_ptr<Room>>& rooms, Clock ranMs, bool& failed) {
	std::cout << "\n==== Soak summary after " << Hms(ranMs) << " ====\n";
	Stats total;
	const double hours = ranMs / 3600000.0;
	for (const auto& roomPtr : rooms) {
		const auto& room = *roomPtr;
		const auto& st = room.st;
		const double average = room.rows ? room.fractionSum / room.rows : 0;
		const auto checks = Evaluate(room, hours);
		const bool bad = std::any_of(checks.begin(), checks.end(), [](const Check& check) { return !check.ok; });
		failed = failed || bad;
		std::cout << "room " << room.number << (bad ? "  FAILED" : "  ok") << ": avg members connected " << std::fixed << std::setprecision(1) << average * room.members.size()
			<< "/" << room.members.size() << ", joins " << st.joins << " (rejoins " << st.rejoins << "), join failures " << st.joinFailures << ", refused " << st.refused
			<< ", control losses " << st.controlLosses << ", helper crashes " << st.helperCrashes << ", degraded " << st.degraded << "\n"
			<< "    actions " << st.actionsSent << " sent, " << st.actionsAccepted << " accepted, " << st.actionsRejected << " rejected, " << st.actionTimeouts << " timed out"
			<< "; rejects: " << (st.rejects.empty() ? "none" : Flat(st.rejects)) << "\n"
			<< "    chat " << st.chatSent << " sent, " << st.chatSampled << " timed, " << st.chatSeen << " seen, " << st.chatLost << " lost; rtt p50 " << Percentile(st.chatRttAll, 0.5)
			<< " ms p95 " << Percentile(st.chatRttAll, 0.95) << " ms max " << Maximum(st.chatRttAll) << " ms\n"
			<< "    join ms p50 " << Percentile(st.joinMsAll, 0.5) << " p95 " << Percentile(st.joinMsAll, 0.95) << " max " << Maximum(st.joinMsAll)
			<< "; checks " << st.probesOk << " ok " << st.probesFailed << " failed; matches " << st.matchesOk << "/" << st.matchesStarted << " ok"
			<< (st.matchFailureSteps.empty() ? "" : " (failed at " + Flat(st.matchFailureSteps) + ")") << "\n";
		if (!st.joinFailureReasons.empty()) std::cout << "    join failures by reason: " << Flat(st.joinFailureReasons) << "\n";
		if (!st.lossReasons.empty()) std::cout << "    control losses by reason: " << Flat(st.lossReasons) << "\n";
		for (const auto& check : checks) if (!check.ok) std::cout << "    FAILED criterion: " << check.name << " " << check.measured << " (limit " << check.limit << ")\n";
		total.joins += st.joins; total.joinFailures += st.joinFailures; total.controlLosses += st.controlLosses; total.helperCrashes += st.helperCrashes;
	}
	std::cout << "total: " << total.joins << " joins, " << total.joinFailures << " join failures, " << total.controlLosses << " control losses, "
		<< total.helperCrashes << " helper crashes\n" << (failed ? "RESULT: FAILED (see the failed criteria above)"
		: "RESULT: PASSED (every room met every criterion)") << std::endl;
}
}

int wmain(int argc, wchar_t** argv) {
	std::cout << std::unitbuf;
	if (argc == 3 && std::wstring(argv[1]) == L"--print-config") return PrintConfig(argv[2]);
	std::uint64_t seed = std::random_device()();
	std::vector<std::wstring> positional;
	for (int i = 1; i < argc; ++i) {
		const std::wstring arg = argv[i];
		const auto value = [&]() { CHECK(i + 1 < argc); return std::wstring(argv[++i]); };
		if (arg == L"--members") options.members = std::stoul(value());
		else if (arg == L"--minutes") options.minutes = std::stod(value());
		else if (arg == L"--log") options.log = Utf8(value());
		else if (arg == L"--seed") seed = std::stoull(value());
		else if (arg == L"--first-room") options.firstRoom = std::stoi(value());
		else if (arg == L"--match-every") options.matchEvery = std::stod(value());
		else if (arg == L"--rejoin-every") options.rejoinEvery = std::stod(value());
		else if (arg == L"--activity") options.activity = std::stod(value());
		else if (arg == L"--max-timeout-pct") options.maxTimeoutPct = std::stod(value());
		else if (arg == L"--max-chat-p95-ms") options.maxChatP95Ms = std::stod(value());
		else if (arg == L"--max-chat-lost-pct") options.maxChatLostPct = std::stod(value());
		else if (arg == L"--max-chat-missing-pct") options.maxChatMissingPct = std::stod(value());
		else if (arg == L"--max-match-fail-pct") options.maxMatchFailPct = std::stod(value());
		else if (arg == L"--max-drops-per-member-hour") options.maxDropsPerMemberHour = std::stod(value());
		else if (arg == L"--max-join-fail-pct") options.maxJoinFailPct = std::stod(value());
		else positional.push_back(arg);
	}
	if (positional.size() < 3 || options.members < 2 || options.members > room::MaximumMembers) {
		std::cerr << "usage: PublicRoomSoakTest <sf4-net.exe> <room_ticket.exe> [--members 2..16] [--minutes M] [--log csv] [--seed N] "
			"[--first-room N] [--match-every S] [--rejoin-every S] <invitation> [<invitation> ...]\n"
			"       PublicRoomSoakTest --print-config <room_ticket.exe>" << std::endl;
		return 2;
	}
	rng.seed(seed);
	options.helper = positional[0];
	ticketTool = positional[1];
	int nextNumber = options.firstRoom;
	for (std::size_t i = 2; i < positional.size(); ++i) {
		std::string text = Utf8(positional[i]);
		int number = nextNumber;
		const auto equals = text.find('=');
		if (equals != std::string::npos && equals < 4) { number = std::stoi(text.substr(0, equals)); text = text.substr(equals + 1); }
		CHECK(number >= 1 && number <= 255);
		options.rooms.emplace_back(number, text);
		nextNumber = number + 1;
	}
	std::vector<std::string> key;
	CHECK(RunTool("key " + Seed('1'), key) && key.size() == 2);
	const std::string kid = key[1];

	SetConsoleCtrlHandler(OnConsole, TRUE);
	runStart = Now();
	eventsFile.open(options.log + ".events.log", std::ios::app);
	std::ofstream csv(options.log, std::ios::app);
	CHECK(csv.is_open());
	if (csv.tellp() == 0) csv << CsvHeader << std::endl;
	std::cout << "seed " << seed << ": " << options.rooms.size() << " room(s) of " << options.members << " members for " << options.minutes
		<< " minutes; csv " << options.log << std::endl;
	Event(0, -1, "soak start, seed " + std::to_string(seed), false);

	// The people. Member 0 of every room is the creator the hosts are configured with.
	std::vector<std::unique_ptr<Room>> rooms;
	for (const auto& entry : options.rooms) {
		auto room = std::make_unique<Room>();
		room->number = entry.first;
		room->invitation = entry.second;
		room->roomIdHex = RoomIdFor(entry.first);
		room->kid = kid;
		room->started = Now();
		for (std::size_t index = 0; index < options.members; ++index) {
			auto member = std::make_unique<Member>();
			char hex[8];
			std::snprintf(hex, sizeof(hex), "%02x%02x", entry.first & 0xff, static_cast<int>(index));
			member->room = room.get();
			member->index = static_cast<int>(index);
			char label[16];
			std::snprintf(label, sizeof(label), "r%02dm%02d", entry.first, static_cast<int>(index));
			member->label = label;
			member->seed = index == 0 ? Seed('a') : std::string(60, '7') + hex;
			std::vector<std::string> ember;
			CHECK(RunTool("ember-id " + member->seed, ember) && !ember.empty());
			member->emberId = ember[0];
			// The creator first, the rest a second and a half apart.
			member->nextAt = Now() + (index == 0 ? 0 : Seconds(3, 4) + index * 1500) + (rooms.size() * 1000);
			room->members.push_back(std::move(member));
		}
		room->nextMatchAt = Now() + Seconds(90, 120);
		room->nextRejoinAt = Now() + Seconds(120, 150);
		rooms.push_back(std::move(room));
	}
	const Clock endAt = runStart + static_cast<Clock>(options.minutes * 60000);
	for (auto& room : rooms) allRooms.push_back(room.get());
	std::vector<std::thread> threads;
	for (auto& room : rooms) threads.emplace_back(RoomMain, room.get(), endAt, &csv, seed);

	// The main thread only reports: the process's own processor use for the rows,
	// and how many members are connected.
	Clock nextStatus = runStart + 60000, nextCpu = runStart + 58000, lastCpuAt = runStart;
	std::uint64_t lastSelf = 0;
	while (!stopRequested && Now() < endAt) {
		Sleep(200);
		const auto now = Now();
		if (now >= nextCpu) {
			nextCpu += 60000;
			const auto self = ProcessTime(GetCurrentProcess());
			processCpuPct = 100.0 * (self - lastSelf) / (std::max<Clock>)(1, now - lastCpuAt);
			lastSelf = self; lastCpuAt = now;
		}
		if (now >= nextStatus) {
			nextStatus += 60000;
			std::size_t active = 0, total = 0;
			for (auto* room : allRooms) { active += room->activeNow; total += room->members.size(); }
			std::lock_guard<std::mutex> lock(outputMutex);
			std::cout << "[+" << Hms(now - runStart) << "] " << active << "/" << total << " members connected" << std::endl;
		}
	}
	// Leave cleanly: every room thread ends its members and helpers.
	std::cout << (stopRequested ? "interrupted; " : "time is up; ") << "stopping helpers" << std::endl;
	stopRequested = true;
	for (auto& thread : threads) thread.join();
	bool failed = false;
	PrintSummary(rooms, Now() - runStart, failed);
	Event(0, -1, std::string("soak end: ") + (failed ? "FAILED" : "PASSED"), false);
	return failed ? 1 : 0;
}
