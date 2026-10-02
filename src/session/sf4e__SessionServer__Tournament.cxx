// Binding the room this server leads to a tournament match. The binding comes
// from the leader's own helper, which checked the bridge's signature; another
// member can never supply one.
#include "sf4e__SessionServer.hxx"

#include <algorithm>

using sf4e::SessionServer;
using nlohmann::json;

void SessionServer::ForgetRoomMember(room::MemberId member) {
	roomIncarnations.erase(member);
	for (auto entry = roomMembers.begin(); entry != roomMembers.end(); ++entry) {
		if (entry->second != member) continue;
		const auto connection = entry->first;
		roomSelectedTables.erase(connection);
		roomPeerIdentities.erase(member);
		roomMembers.erase(entry);
		cidMap.erase(connection);
		clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const SessionMember& client) {
			return client.conn == connection;
		}), clients.end());
		for (auto& loaded : _roomBattleLoaded) loaded.erase(connection);
		for (auto& punch : _roomPunchReady) punch.erase(connection);
		return;
	}
}

bool SessionServer::BindTournament(const room::TournamentBinding& binding) {
	if (!_roomAuthority) return false;
	// Only the writable owner changes the room, one quorum round at a time;
	// the caller tries again on a later tick.
	if (_recovery.Enabled() && (!_recovery.Writable() || _roomAuthority->RecoveryPaused() ||
		_recoveryCandidateReady || _recovery.PendingProposal())) return false;
	if (_recovery.Enabled()) {
		BeginRecoveryCandidate();
		if (!_recoveryCandidateReady) return false;
	}
	const auto prior = _roomAuthority->SnapshotCopy();
	const auto bound = _roomAuthority->BindTournament(binding);
	if (!bound.accepted) {
		if (_recovery.Enabled()) DropRecoveryCandidate();
		return false;
	}
	// Members the binding does not name were sent away; their connections go
	// the way a kicked member's do.
	for (const auto& event : bound.events) {
		if (event.kind != room::Event::Kind::MemberRemoved) continue;
		CaptureFrozenMember(event.member, prior);
		for (const auto& entry : roomMembers) {
			if (entry.second != event.member) continue;
			SessionProtocol::RoomResultMessage removed;
			removed.result.accepted = false;
			removed.result.reason = room::RejectReason::MemberKicked;
			removed.result.snapshot = _roomAuthority->SnapshotCopy();
			removed.result.snapshot.localMember = 0;
			Respond(entry.first, json(removed));
			break;
		}
		ForgetRoomMember(event.member);
	}
	BroadcastRoomState(bound.events);
	PruneFrozenMembers();
	if (_recovery.Enabled()) FinishRecoveryCandidate();
	return true;
}
