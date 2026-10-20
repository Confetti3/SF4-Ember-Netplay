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

// One line of the child protocol, without its newline.
std::string HostedLine(const std::string& invitation, const std::string& region);
std::string ClosedLine(const std::string& reason);

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
	// Hosted, empty and with nothing in flight: no proposal, decode, ban or
	// timer waits on the next tick, so the caller may tick slowly until a
	// helper event arrives.
	bool Quiet() const;
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
	void ForwardBans(const std::vector<std::string>& banned, std::uint64_t nowMs);
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
	std::set<std::string> forwardedBans_;
	// When each ban not yet given to the helper was first seen.
	std::map<std::string, std::uint64_t> pendingBans_;
	// The snapshot and applied revisions the last Report looked at; see Report.
	std::uint64_t reportedSnapshotRevision_ = 0, reportedAppliedRevision_ = 0;
	Status reported_;
	bool haveReported_ = false;
};

} }
