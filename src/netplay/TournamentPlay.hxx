#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../common/MatchResult.hxx"
#include "../session/RoomModel.hxx"

// Playing one bridge-run tournament match (spec 14 to 16), as a pure state
// machine the runtime ticks. It decides which helper request to send, when to
// host or join the match's room, when the room's leader applies the bridge's
// binding, when to fetch each game's permit and tell the room, and what to
// report after each game. It never touches the game, the helper or the room
// itself: the runtime turns each Output into the real call and feeds every
// answer back.
namespace sf4e { namespace netplay { namespace tournament {

// How often a fighter claims the match again: it renews a provisioning lease
// and picks up a newer binding.
constexpr std::uint64_t ClaimIntervalMs = 10000;
// How soon a failed request is tried again.
constexpr std::uint64_t RetryMs = 5000;
// How often a fighter waiting for a permit asks the bridge, at most.
constexpr std::uint64_t PrepareIntervalMs = 2000;
// How long a host or join may take before it is tried again.
constexpr std::uint64_t OpenTimeoutMs = 30000;
// How many times a game's report is handed to the helper before the match
// stops as unsaved; the tries are RetryMs apart.
constexpr int MaxReportAttempts = 6;

// The bridge's binding, as the fighter's own helper checked it.
struct Binding {
	std::string roomId;
	room::TournamentBinding room;
	int localSlot = -1;
};

// The answer to a claim or a room publish.
struct ClaimReply {
	enum class Role { Host, Wait, Room } role = Role::Wait;
	std::string leaseId, fence, roomId, invitation;
	std::uint64_t retryAfterMs = 0;
	std::optional<Binding> binding;
};

// The answer to a prepare: the permit for `generation`, or not yet.
struct PrepareReply {
	bool permitted = false;
	std::uint64_t generation = 0;
	std::string permitId;
	// How long the game may start after the bridge issued the permit (its
	// start_by less its issued_at, both on the bridge's clock).
	std::uint64_t startWindowMs = 0;
	std::uint64_t retryAfterMs = 0;
};

// The room as the runtime sees it this tick.
struct RoomView {
	// In an open room. `opening` while a host or join is under way.
	bool joined = false;
	bool opening = false;
	// 32 lowercase hex digits, and the invitation another player joins with.
	std::string roomId;
	std::string invitation;
	// This game applies the room's changes (the coordination leader).
	bool authorityWritable = false;
	const room::Snapshot* snapshot = nullptr;
};

struct Output {
	enum class Kind {
		Claim, Publish, Prepare, Report,  // helper requests
		Host, Join, Leave,                // room commands
		Bind, PermitReady,                // room changes
		Forget,                           // the helper drops what it verified
	} kind = Kind::Claim;
	std::string roomId, invitation;               // Publish, Join
	std::string leaseId, fence, replaces;         // Publish
	std::uint64_t generation = 0;                 // Prepare, Report, PermitReady
	std::string permitId;                         // PermitReady
	std::uint64_t startWindowMs = 0;              // PermitReady
	std::string result;                           // Report: p1_win, p2_win, draw, abort or cancel
	std::uint64_t captureFrame = 0, confirmedFrame = 0;  // Report; zero when absent
	room::TournamentBinding binding;              // Bind
};

enum class Phase { Idle, Claiming, Opening, InRoom, Finished, Failed };

class TournamentPlay {
public:
	void Start(std::string bridgeId, std::string matchId, std::uint64_t nowMs);
	// Leaves the match's room and forgets it. Safe to call in any phase.
	void Stop();
	// Gives the match up for a reason outside it (the helper is gone): Failed
	// with `reason`. Nothing when no match is under way.
	void Abandon(const std::string& reason);
	std::vector<Output> Tick(std::uint64_t nowMs, const RoomView& room);

	// The answer to a Claim or a Publish: both name the match's room.
	void OnRoom(Output::Kind request, const ClaimReply& reply, std::uint64_t nowMs);
	void OnPrepare(const PrepareReply& reply, std::uint64_t nowMs);
	// A Claim, Publish or Prepare failed. Reports have their own answers.
	void OnFailure(Output::Kind request, const std::string& code, std::uint64_t nowMs);
	// The helper saved (and perhaps sent) generation's report.
	void OnReported(std::uint64_t generation);
	// The helper did not save generation's report.
	void OnReportFailed(std::uint64_t generation, const std::string& code, std::uint64_t nowMs);
	// A rollback-confirmed native result of the bound table's game.
	void OnTerminal(std::uint64_t generation, room::MatchResult result, std::uint64_t captureFrame, std::uint64_t confirmedFrame);

	Phase GetPhase() const { return phase_; }
	const std::string& BridgeId() const { return bridgeId_; }
	const std::string& MatchId() const { return matchId_; }
	// Why the match ended or failed: a stable code, empty otherwise.
	const std::string& Reason() const { return reason_; }
	// Waiting for the other fighter's room, or for the bridge's permit.
	bool WaitingForOpponent() const { return waitingForOpponent_; }
	bool WaitingForPermit() const { return waitingForPermit_; }
	// A game's report is not saved yet. Reports go out under the current
	// match, so another match waits until they are.
	bool SavingReports() const { return !unsaved_.empty(); }

private:
	void Fail(Phase phase, const std::string& reason);
	void FollowClaim(std::uint64_t nowMs, const RoomView& room, std::vector<Output>& out);
	void TrackPermits(std::uint64_t nowMs, const RoomView& room, std::vector<Output>& out);
	// Hands `report` to the helper and keeps it until the helper saved it.
	void SendReport(Output report, std::vector<Output>& out);
	void ResendReports(std::uint64_t nowMs, std::vector<Output>& out);
	void ReportsSettled();

	Phase phase_ = Phase::Idle;
	std::string bridgeId_, matchId_, reason_;
	std::vector<Output> pending_;
	bool claimInFlight_ = false, prepareInFlight_ = false, publishInFlight_ = false;
	std::uint64_t nextClaimMs_ = 0, nextPrepareMs_ = 0, nextPublishMs_ = 0, openedAtMs_ = 0;
	// The provisioning lease from a Host answer, until the room is published.
	std::string leaseId_, fence_;
	bool hostRequested_ = false, joinRequested_ = false, leaving_ = false, published_ = false;
	// The room the bridge names for the match.
	std::string targetRoom_, targetInvitation_, publishedInvitation_, publishingInvitation_;
	// When the binding was last handed to the room. The room can refuse it for
	// a moment (a quorum round in progress), so it is offered again, at most
	// once a second, until the room shows it.
	std::uint64_t boundAtMs_ = 0, offeredGeneration_ = 0, offeredRevision_ = 0;
	std::optional<Binding> binding_;
	bool waitingForOpponent_ = false, waitingForPermit_ = false;
	// Permits this fighter holds, by generation; games that started; games reported.
	struct HeldPermit { std::string id; std::uint64_t startWindowMs = 0; };
	std::map<std::uint64_t, HeldPermit> permits_;
	std::map<std::uint64_t, std::uint64_t> permitToldMs_;
	std::set<std::uint64_t> started_, reported_;
	// Reports the helper has not saved yet, by generation: sent and waiting for
	// the answer, or waiting to be sent again. The helper keeps the match's
	// permits until they are saved, so Forget waits for them, and no further
	// game is prepared while one is unsaved.
	struct UnsavedReport { Output report; int attempts = 0; bool inFlight = false; std::uint64_t retryAtMs = 0; };
	std::map<std::uint64_t, UnsavedReport> unsaved_;
	bool forgetAfterReports_ = false;
};

// The wire name of a native result in a report.
const char* ResultName(room::MatchResult result);

} } }
