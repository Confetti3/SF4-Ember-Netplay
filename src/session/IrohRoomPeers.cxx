// IrohRoom: peer records. One record per session of a remote endpoint: its
// transport handle, message numbering, admission and control connection.
// Every transition lives here, so a departure, a control reconnect and a new
// session from the same endpoint agree on which handle owns queued work.
#include "IrohRoom.hxx"
#include "RoomMessageQueue.hxx"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace sf4e { namespace session {
using nlohmann::json;

std::string IrohRoom::PeerTag(const std::string& peer) { return peer.substr(0, 8); }

bool IrohRoom::IsEndpointIdentity(const std::string& identity) {
	return identity.size() == 64 && std::all_of(identity.begin(), identity.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}

std::string IrohRoom::PeerIdentity(Connection connection) const {
	if (connection == 1) return localIdentity_;
	const auto peer = peers_.find(connection);
	return peer == peers_.end() ? std::string() : peer->second.identity;
}

std::uint64_t IrohRoom::PeerIncarnation(Connection connection) const {
	const auto member = memberIncarnations_.find(PeerIdentity(connection));
	return member == memberIncarnations_.end() ? 0 : member->second;
}

// A retired connection has no record left; its closure is already queued for
// the server, which may still address it until it processes that closure.
bool IrohRoom::PeerControlClosed(Connection connection) const {
	const auto peer = peers_.find(connection);
	return peer == peers_.end() || !peer->second.control;
}

std::map<Connection, IrohRoom::Peer>::iterator IrohRoom::FindPeer(const std::string& identity) {
	return std::find_if(peers_.begin(), peers_.end(), [&](const std::pair<const Connection, Peer>& item) {
		return item.second.identity == identity;
	});
}

Connection IrohRoom::ConnectionForIdentity(const std::string& identity) const {
	if (identity == localIdentity_ && !identity.empty()) return 1;
	for (const auto& peer : peers_) if (peer.second.identity == identity) return peer.first;
	return 0;
}

std::map<Connection, std::string> IrohRoom::ControlIdentities() const {
	std::map<Connection, std::string> identities;
	if (!localIdentity_.empty()) identities.emplace(1, localIdentity_);
	for (const auto& peer : peers_) identities.emplace(peer.first, peer.second.identity);
	return identities;
}

IrohRoom::Peer* IrohRoom::AllocatePeer(const std::string& identity) {
	if (peers_.size() >= MaximumRemotePeers || nextConnection_ == (std::numeric_limits<Connection>::max)()) return nullptr;
	Peer peer;
	peer.identity = identity;
	peer.admitted = memberIncarnations_.count(identity) || committedMembers_.count(identity);
	return &peers_.emplace(nextConnection_++, std::move(peer)).first->second;
}

void IrohRoom::ControlOpened(Peer& peer, std::uint64_t control) {
	peer.control = control;
	peer.departureDeadline = 0;
}

void IrohRoom::ControlClosed(Peer& peer, std::uint64_t graceMs) {
	peer.control.reset();
	// The earliest deadline wins: a departure notice after a silent close
	// retires the seat now rather than when the close's grace runs out.
	const auto deadline = GetTickCount64() + graceMs;
	if (!peer.departureDeadline || deadline < peer.departureDeadline) peer.departureDeadline = deadline;
}

// Closes the handle for the server and drops the record with its queued
// intents. Callers check closed_ has room first.
std::map<Connection, IrohRoom::Peer>::iterator IrohRoom::RetirePeer(std::map<Connection, Peer>::iterator peer) {
	closed_.push_back(peer->first);
	return ErasePeer(peer);
}

std::map<Connection, IrohRoom::Peer>::iterator IrohRoom::ErasePeer(std::map<Connection, Peer>::iterator peer) {
	for (auto message = serverMessages_.begin(); message != serverMessages_.end();) {
		if (message->connection == peer->first) { queuedBytes_ -= message->payload.size(); message = serverMessages_.erase(message); }
		else ++message;
	}
	return peers_.erase(peer);
}

void IrohRoom::PruneRetiredPeers() {
	// Wait until native application of the committed candidate has queued its
	// final effects. Socket loss alone never retires a stable member mapping,
	// and a record whose control is open is this endpoint's current session
	// even while the roster sets have yet to list it again.
	if (!haveCommittedMembers_ || !committedCheckpoints_.empty() || !decoding_.empty()) return;
	for (auto peer = peers_.begin(); peer != peers_.end();) {
		const auto& identity = peer->second.identity;
		if (!peer->second.admitted || peer->second.control) { ++peer; continue; }
		const auto game = games_.find(identity);
		if (memberIncarnations_.count(identity) || committedMembers_.count(identity) ||
			(game != games_.end() && game->second.state != GameState::Closed)) { ++peer; continue; }
		peer = ErasePeer(peer);
	}
}

void IrohRoom::ExpireDepartedPeers() {
	if (state_ != State::Ready || !coordination_.leaderLocal || !coordination_.writable) return;
	const auto now = GetTickCount64();
	for (auto peer = peers_.begin(); peer != peers_.end();) {
		if (!peer->second.departureDeadline || now < peer->second.departureDeadline) { ++peer; continue; }
		if (closed_.size() >= MaximumQueuedMessages) return; // Retried next poll.
		// SessionServer::Step turns this into a committed Leave, which also
		// retires the member's coordination vote.
		peer = RetirePeer(peer);
	}
}

// control_rebound.members describes currently connected control edges, while
// member_incarnations is the authenticated room roster. Returns false when the
// rebound cannot be applied and the event must be dropped whole.
bool IrohRoom::ApplyControlRebound(const std::map<std::string, std::uint64_t>& incarnations,
	const std::set<std::string>& connected) {
	for (auto& peer : peers_)
		if (incarnations.count(peer.second.identity)) peer.second.admitted = true;
	PruneRetiredPeers();
	// Recovery rebind needs a stable logical connection for every admitted
	// endpoint, including a follower that has only its leader edge after
	// takeover. Allocate those handles locally; this does not claim a
	// transport path or synthesize a peer connection.
	for (const auto& entry : incarnations) {
		const auto& identity = entry.first;
		if (identity == localIdentity_ || ConnectionForIdentity(identity)) continue;
		if (!AllocatePeer(identity)) return false;
	}
	// A new leader only ever saw its own edge to the old leader, so a member
	// whose game died before the handoff never sends it a control_closed.
	// Start the same departure grace for every roster member without a
	// control edge; a reconnect clears it (ledger H-008).
	if (coordination_.leaderLocal)
		for (auto& peer : peers_)
			if (incarnations.count(peer.second.identity) && !connected.count(peer.second.identity))
				ControlClosed(peer.second, DepartureGraceMs);
	return true;
}

bool IrohRoom::HandleConnected(const json& event) {
	const auto identity = event.at("peer").get<std::string>();
	const auto room = event.at("room").get<std::array<std::uint8_t, 16>>();
	const auto control = event.at("control").get<std::uint64_t>();
	if (hosting_ && room != room_) { Fail("wrong_room"); return false; }
	if (!hosting_ && state_ != State::Joining && !coordination_.active) { Fail("unexpected_peer"); return false; }
	const auto known = FindPeer(identity);
	if (coordination_.active && (known != peers_.end() || (!localIdentity_.empty() && identity == localIdentity_))) {
		if (known != peers_.end()) ControlOpened(known->second, control); // Its control is back.
		return true;
	}
	if (known != peers_.end()) { Fail("duplicate_peer"); return false; }
	PruneRetiredPeers();
	auto* peer = AllocatePeer(identity);
	if (!peer) { Fail("peer_limit"); return false; }
	ControlOpened(*peer, control);
	room_ = room;
	if (coordination_.writable && coordination_.rebound) state_ = State::Ready;
	return true;
}

bool IrohRoom::HandleControlTraffic(const json& event, const std::string& type) {
	const auto identity = event.at("peer").get<std::string>();
	const auto peer = FindPeer(identity);
	if (peer == peers_.end()) return true; // Late event for a departed peer.
	if (type == "peer_session") return BeginPeerSession(peer, event.at("incarnation").get<std::uint64_t>());
	if (type == "peer_departed") return PeerDeparted(peer);
	if (type == "message") return ReceiveControlMessage(peer, event);
	// control_closed. A close for a superseded connection says nothing about
	// the one this record holds now.
	const auto control = event.at("control").get<std::uint64_t>();
	if (peer->second.control && *peer->second.control != control) return true;
	if (coordination_.active) {
		// A control socket is not a membership decision. Retain its
		// stable mapping while committed coordination reconnects it.
		// A member whose game was killed never sends Leave. Its control
		// gets a grace period to reconnect; after that the committed
		// leader treats it as departed (ExpireDepartedPeers). That holds
		// for the leader too: once another member takes over, the former
		// leader's record is pruned or expired like any other.
		ControlClosed(peer->second, DepartureGraceMs);
		if (identity == coordination_.leader) {
			coordination_.writable = false; coordination_.rebound = false;
			state_ = State::Degraded; invitation_.clear(); discordInvitation_.clear();
			probe_ = {};
			// Make the freeze observable for one owner tick even if a
			// queued authority watch and rebound follow immediately. This
			// prevents the UI or SessionServer from issuing a mutation in
			// the same poll that detected loss of its leader control.
			return false;
		}
		return true;
	}
	if (closed_.size() >= MaximumQueuedMessages) { Fail("room_close_queue"); return false; }
	RetirePeer(peer);
	if (!hosting_) {
		// Keep the loss visible to the room controller while retaining
		// independently authorized gameplay mappings. New match
		// preparation is blocked by the degraded state.
		state_ = State::Degraded;
		invitation_.clear(); discordInvitation_.clear();
		error_ = "room_control_closed";
	}
	return true;
}

// The helper accepted this endpoint's Admission for a process incarnation it
// had not admitted before: a new room session, not a control reconnect.
bool IrohRoom::BeginPeerSession(std::map<Connection, Peer>::iterator peer, std::uint64_t incarnation) {
	auto& record = peer->second;
	if (record.incarnation == incarnation) return true; // Repeated for the same session.
	if (record.incarnation == 0) { record.incarnation = incarnation; return true; } // Allocated for this session by its connected event.
	// This endpoint's previous session left its record behind: the retired-
	// peer prune waits for checkpoint application. Retire that handle with
	// its queued work, since a client the server still holds on it belongs
	// to the old session, and continue outgoing numbering on a fresh handle.
	// The returning client accepts any higher number and starts its own from
	// the beginning, which the fresh record expects.
	if (closed_.size() >= MaximumQueuedMessages) { Fail("room_close_queue"); return false; }
	spdlog::info("Room: peer {} started a new session; connection {} retired", PeerTag(record.identity), peer->first);
	const auto identity = record.identity;
	const auto nextId = record.nextId;
	const auto control = record.control;
	RetirePeer(peer);
	auto* fresh = AllocatePeer(identity);
	if (!fresh) { Fail("peer_limit"); return false; }
	fresh->nextId = nextId;
	fresh->incarnation = incarnation;
	if (control) ControlOpened(*fresh, *control);
	return true;
}

// The member announced its departure on its still-open control. Only the
// committed leader's room removes members, and a leader never announces one:
// it hands off. Retire the seat now, as an expired departure grace does; the
// member's control and vote stay live until the committed removal has
// excluded it, so the grace path must not be used: a rebound in between
// still lists that edge as connected.
bool IrohRoom::PeerDeparted(std::map<Connection, Peer>::iterator peer) {
	if (!coordination_.leaderLocal || peer->second.identity == coordination_.leader) return true;
	if (closed_.size() >= MaximumQueuedMessages) { ControlClosed(peer->second, 0); return true; } // Retired once the queue drains.
	spdlog::info("Room: peer {} announced its departure", PeerTag(peer->second.identity));
	RetirePeer(peer);
	return true;
}

bool IrohRoom::ReceiveControlMessage(std::map<Connection, Peer>::iterator peer, const json& event) {
	const auto id = event.at("message_id").get<std::int64_t>();
	const auto payload = event.at("payload").get<std::string>();
	if (id <= peer->second.receivedId || payload.empty() || payload.size() > MaximumPayload) {
		spdlog::warn("Room: control message rejected peer={} id={} last={} bytes={}", PeerTag(peer->second.identity), id,
			peer->second.receivedId, payload.size());
		Fail("invalid_control_message"); return false;
	}
	peer->second.receivedId = id;
	// Authority watches and control streams can arrive in either
	// order during takeover. Committed effects always go through
	// recipient validation; client intents wait behind the server's
	// local committed-leader gate regardless of the current UI role.
	const auto decoded = json::parse(payload);
	const bool effect = decoded.is_object() && decoded.contains("_commit");
	// Only a bare admission response from the committed
	// leader enters the local client queue.  Other untagged
	// peer payloads remain server intents.
	const auto messageType = decoded.value("type", std::string());
	const bool fromLeader = !effect && coordination_.active && coordination_.writable &&
		peer->second.identity == coordination_.leader;
	const bool leaderHandshake = fromLeader && pendingAdmission_ &&
		(messageType == "hello_resp" || messageType == "join_rej");
	// Verification is forwarded by the leader without a commit
	// token; it carries no room mutation, so it is delivered
	// directly rather than through a checkpoint.
	const bool leaderVerification = fromLeader && IsVerificationType(messageType);
	return Queue((effect || leaderHandshake || leaderVerification) ? clientMessages_ : serverMessages_, {peer->first, id, payload, ""});
}

SendResult IrohRoom::SendRemote(Connection connection, const std::string& payload, std::int64_t* id) {
	if (state_ != State::Ready) return SendResult::NotConnected;
	if (payload.empty() || payload.size() > MaximumPayload) return SendResult::InvalidPayload;
	auto peer = peers_.find(connection);
	if (peer == peers_.end() || !peer->second.control) return SendResult::NotConnected;
	if (peer->second.nextId == (std::numeric_limits<std::int64_t>::max)()) return SendResult::Failed;
	const auto messageId = peer->second.nextId;
	// Named for the control this record holds: the helper refuses the send if
	// that endpoint has since connected again for a session this room has
	// not been told about, instead of delivering old numbering into it.
	if (!Command(json{{"type", "send"}, {"epoch", epoch_}, {"peer", peer->second.identity}, {"control", *peer->second.control},
		{"message_id", messageId}, {"payload", payload}}.dump())) return SendResult::QueueFull;
	++peer->second.nextId;
	if (id) *id = messageId;
	return SendResult::Queued;
}

} }
