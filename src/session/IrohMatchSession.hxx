#pragma once
#include <winsock2.h>
#include <functional>
#include <deque>
#include "IrohRoom.hxx"
#include "sf4e__SessionClient.hxx"
#include "MatchAuthority.hxx"
#include "MatchTeardownTiming.hxx"

namespace sf4e { namespace session {

// Game-thread owner of a single authorized match, including pre-GGPO port
// reservation. The helper moves packets independently of Tick and rendering.
class IrohMatchSession {
public:
	enum class Phase { Idle, Preparing, Prepared, Connecting, Started, Ending, Failed };
	using Clock = std::function<ULONGLONG()>;
	IrohMatchSession(SessionClient& client, std::shared_ptr<IrohRoom> room, Clock clock = Clock());
	~IrohMatchSession();
	bool Tick(bool ggpoOwnsSocket = false);
	void End();
	// Convert a failed setup into ordinary generation-scoped teardown. The
	// caller invokes this after native GGPO has released its socket; unlike End,
	// abort does not wait for a server game_end acknowledgment.
	bool Abort();
	bool BeginReplacement();
	void ReleasePortToGgpo();
	std::uint16_t RemotePort(const SessionProtocol::ConnectionID& member) const;
	Phase GetPhase() const { return phase_; }
	std::uint64_t Generation() const { return generation_; }
	std::size_t LocalSlot() const { return slot_; }
	const std::vector<SessionProtocol::ConnectionID>& Roster() const { return roster_; }
	const std::string& Error() const { return error_; }
	// True only after native socket ownership and every locally prepared helper
	// mapping for this accepted generation have retired. Room authority state is
	// deliberately excluded because replacement is offered when it is unavailable.
	bool CanReplace(bool ggpoOwnsSocket) const;
	// Explicit replacement may begin teardown for a preparation that never
	// reached native GGPO. A started native session and failed close remain fenced.
	bool CanBeginReplacement(bool ggpoOwnsSocket) const;
private:
	struct Link {
		std::size_t slot;
		std::string peer;
		std::array<std::uint8_t, 32> capability;
		bool dial;
	};
	bool AcceptGrant(const nlohmann::json& message);
	bool ControlReadyForSetup() const;
	bool StartConnecting();
	bool StartQueuedSetup();
	bool Acknowledge(const char* type);
	bool FlushPendingAcks();
	bool Fail(const char* error);
	void ClearLinks();
	ULONGLONG Now() const;
	SessionClient& client_;
	std::shared_ptr<IrohRoom> room_;
	Clock clock_;
	MatchTeardownTiming teardown_;
	Phase phase_ = Phase::Idle;
	std::uint64_t generation_ = 0;
	std::size_t slot_ = 0;
	std::vector<SessionProtocol::ConnectionID> roster_;
	std::vector<Link> links_;
	std::string error_;
	nlohmann::json pendingGrant_;
	std::uint64_t pendingGrantTerm_ = 0;
	bool waitingForProjection_ = false;
	bool pendingConnect_ = false;
	bool pendingStart_ = false;
	ULONGLONG deadline_ = 0;
	ULONGLONG deadlineRemaining_ = 0;
	bool deadlineSuspended_ = false;
	struct PendingAck { std::string type; std::uint64_t generation = 0; };
	std::deque<PendingAck> pendingAcks_;
	SOCKET reservedPort_ = INVALID_SOCKET;
	bool winsock_ = false;
	bool readySent_ = false, mappingsPrepared_ = false, endSent_ = false;
	bool roomEndReceived_ = false;
	bool replacementRetiring_ = false;
};

} }
