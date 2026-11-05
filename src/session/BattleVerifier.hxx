#pragma once
// The room client's desync check, which the game owns: its checkpoints and
// their hashes, how a mismatch is reported, and how a desynced fight ends.
// The room client carries the messages: it buffers the remote hashes as they
// arrive, hands over SF4's legacy snapshots, and steps the check once per
// client step while a match's snapshots are enabled.
//
// Each game defines MakeBattleVerifier() once, as it defines Game(); SF4's is
// sf4e__Sf4Verification.cxx. A binary without one fails to link.
#include <map>
#include <memory>

#include "sf4e__SessionProtocol.hxx"

namespace sf4e { class SessionClient; }

namespace sf4e { namespace session {

class BattleVerifier {
public:
	virtual ~BattleVerifier() = default;
	// A remote player's whole-state snapshot (SF4's legacy check). A game
	// that sends none ignores it.
	virtual void OnRemoteSnapshot(SessionClient& client, const SessionProtocol::StateSnapshot& snapshot) = 0;
	// Publishes this player's confirmed checkpoints, and compares and erases
	// the remote hashes whose local checkpoint is confirmed here.
	virtual void Step(SessionClient& client, std::map<int, SessionProtocol::BattleHashV2>& remoteHashes) = 0;
	// The battle is over or the client disconnected: drops what it buffered.
	virtual void Reset() = 0;
};

std::unique_ptr<BattleVerifier> MakeBattleVerifier();

} } // namespace sf4e::session
