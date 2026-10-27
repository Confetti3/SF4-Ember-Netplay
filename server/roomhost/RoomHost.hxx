#pragma once

// The authority of one public (server-owned) room, with no game and no local
// player: a SessionServer over an IrohRoom this process hosts. See
// docs/design/PUBLIC_ROOMS.md. Portable; the helper process and its pipe are
// the platform's (RoomHostHelper.hxx).
#include "RoomHostStatus.hxx"
#include "../../src/session/IrohRoom.hxx"
#include "../../src/session/RoomRecoveryRuntime.hxx"
#include "../../src/session/sf4e__SessionServer.hxx"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sf4e { namespace roomhost {

// The supervisor's configuration line (server/ember-rooms/src/protocol.rs).
struct Config {
	std::array<std::uint8_t, 16> roomId = {};
	std::string name;
	std::uint8_t capacity = 0;
	std::string buildId, creator, bridgeId, ticketKey, ticketKid;
	std::string helper; // UTF-8 path of sf4-net
	std::uint16_t port = 0, coordinationPort = 0;
	// Each fighter plays at their own input delay (room::DelayPolicy::Own),
	// fixed for the room's life. The supervisor writes "own_delay": true only for
	// such a room; a line without the key is the shared delay.
	bool ownDelay = false;
};
// False with the offending field's name in `error`.
bool ParseConfig(const std::string& line, Config& config, std::string& error);
// Makes the room the configuration describes the server's server-owned room:
// its name, capacity, public default rules and delay policy. False when the
// server will not take it.
bool EnableConfiguredRoom(SessionServer& server, const Config& config, std::uint64_t roomEpoch);

// How long the host sleeps between ticks: the game's rate while anything
// moves, and at most QuietWaitMs while only a room deadline is ahead. A helper
// event ends either wait at once (HelperClient::WaitForIncoming).
constexpr std::uint64_t TickWaitMs = 16, QuietWaitMs = 250;
// The wait for a room whose next timer is due in `nextDueMs` (the largest value
// when none runs): until then, but not less than a tick and not more than
// QuietWaitMs, so stdin closing is noticed within that.
constexpr std::uint64_t IdleWaitMs(std::uint64_t nextDueMs) {
	return nextDueMs < TickWaitMs ? TickWaitMs : nextDueMs > QuietWaitMs ? QuietWaitMs : nextDueMs;
}

// One line of the child protocol, without its newline.
std::string HostedLine(const std::string& invitation, const std::string& region);
std::string ClosedLine(const std::string& reason);

// The status lines a hosted room gives the supervisor and the bans it gives the
// helper, each its own step. Portable and with no helper, so a test can drive it
// over a SessionServer of its own.
class RoomReporter {
public:
	using Emit = std::function<void(const std::string& line)>;
	// Asks the helper to ban an account; false when its queue is full.
	using Ban = std::function<bool(const std::string& account)>;
	// Emits the status line when anything it says has changed. Everything it says
	// comes from the snapshot (which moves its revision on every change), the
	// committed bans and won games (which change with a commit, and so with the
	// applied revision) and the invitation: while none of those has moved,
	// nothing is copied or compared. A committed ban seen here for the first time
	// waits for ForwardBans. False when the line is over MaximumStatusLineBytes;
	// nothing is emitted then.
	bool Report(const SessionServer& server, const room::Snapshot& snapshot, std::uint64_t appliedRevision,
		const std::string& invitation, const Emit& emit, std::uint64_t nowMs);
	// Gives the helper each waiting ban once it has been committed for NoticeMs
	// and no commit is in flight; one the helper's queue refuses is tried again on
	// the next call. The room model has already removed and banned the account;
	// the helper's ban is the outer door. It closes the member's control at once,
	// so it waits a moment for the kick's commit to carry the removal notice to
	// that member.
	void ForwardBans(const SessionServer& server, const Ban& ban, std::uint64_t nowMs);
	// A committed ban has not reached the helper yet.
	bool BansPending() const { return !pendingBans_.empty(); }

private:
	std::set<std::string> forwardedBans_;
	// When each ban not yet given to the helper was first seen.
	std::map<std::string, std::uint64_t> pendingBans_;
	// The snapshot and applied revisions the last Report looked at.
	std::uint64_t reportedSnapshotRevision_ = 0, reportedAppliedRevision_ = 0;
	Status reported_;
	bool haveReported_ = false;
};

class RoomHost {
public:
	using Emit = std::function<void(const std::string& line)>;
	RoomHost(std::shared_ptr<session::IrohRoom> room, Config config, Emit emit);
	~RoomHost();
	// Asks the helper to open the room. Call once the helper is connected.
	bool Open();
	// One tick: room poll, recovery, authority timers, server step, then the
	// status lines that changed. False once the room cannot continue; Error()
	// is the reason for the closed line.
	bool Tick(std::uint64_t nowMs);
	bool Hosted() const { return hosted_; }
	// How long the caller may sleep after a tick at nowMs before the next one,
	// a helper event cutting it short. TickWaitMs while a game is on or starting
	// (a table Ready, Playing or Paused), anything is in flight (a proposal,
	// checkpoint, recovery candidate, ban, queued message or send that waits for
	// the helper), the room is closing or not hosted yet, or recovery failed;
	// otherwise IdleWaitMs of the room's next timer, so occupied rooms that only
	// sit in the lobby tick a few times a second and every timer still fires on
	// time.
	std::uint64_t WaitMs(std::uint64_t nowMs) const;
	// True once the room ends on its own: the room model closed it (a kick at the
	// ban cap) or its last member left with a Leave. The host then says closed
	// with CloseReason(), runs the close sequence and exits.
	bool Closing() const { return closing_; }
	const std::string& CloseReason() const { return closeReason_; }
	const std::string& Error() const { return error_; }
	// Closing, in order: BeginClose, then keep ticking until CloseDelivered
	// (the room model's Close has committed and had a moment to reach the
	// members, or CloseWaitMs passed), then Leave, then poll Leaving() until it
	// is false (the helper confirmed) or the caller's patience runs out.
	static constexpr std::uint64_t CloseWaitMs = 2000;
	void BeginClose(std::uint64_t nowMs);
	bool CloseDelivered(std::uint64_t nowMs);
	void Leave();
	bool Leaving();

private:
	class AccountTransport;
	void Attach();
	// The steps of a tick's report, in this order (Report).
	void NoteInvitation();
	bool EmitHosted(std::uint64_t nowMs);
	void DetectModelClose(const room::Snapshot& snapshot, std::uint64_t nowMs);
	void Report(std::uint64_t nowMs);
	bool Fail(const std::string& reason);

	std::shared_ptr<session::IrohRoom> room_;
	Config config_;
	Emit emit_;
	std::unique_ptr<SessionServer> server_;
	session::RoomRecoveryRuntime recovery_;
	bool opened_ = false, hosted_ = false;
	bool closing_ = false, closeApplied_ = false;
	std::uint64_t closeStartedMs_ = 0, closeCommittedMs_ = 0;
	std::uint64_t readySinceMs_ = 0, recoveryFailedSinceMs_ = 0;
	std::string error_, recoveryError_, closeReason_;
	// The supervisor treats an empty invitation as a protocol error, and the
	// room hides its invitation while control reconnects: keep the last one.
	std::string invitation_;
	RoomReporter reporter_;
};

} }
