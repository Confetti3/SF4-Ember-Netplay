#pragma once

// The simulated members and their rooms. A member's life (helper, identity,
// ticket, join, activity, leave) is in soak_member.cxx; a room's admissions,
// table flows, chat checks and thread are in soak_room.cxx.
#include "../../session/IrohMatchSession.hxx"
#include "../iroh_integration_fixture.hxx"
#include "../../platform/HelperProcess.hxx"
#include "soak_common.hxx"
#include <array>
#include <deque>
#include <map>
#include <memory>

namespace sf4e { namespace test { namespace soak {

using MatchPhase = session::IrohMatchSession::Phase;
using State = session::IrohRoom::State;

std::string RejectName(room::RejectReason reason);

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
	bool StartFlow(Flow::Kind kind, std::uint8_t table, Member* a, Member* b, Clock now);
	void EndFlow(std::uint8_t table);
	void FailFlow(Flow& flow, const std::string& step, const std::string& detail);
	int AdvanceFlow(Flow& flow, Clock now); // 0 running, 1 done, 2 failed
	int ProbeLeg(Flow& flow, Member& from, Member& to, Clock now);
	void Sample(Clock now);
	void CheckChat(Clock now);
};

// One room, on its own thread, until endAt or a stop; then ends its members.
void RoomMain(Room* room, Clock endAt, std::ofstream* csv, std::uint64_t seed);

} } }
