// IrohRoom: peer records. One record per session of a remote endpoint: its
// transport handle, message numbering, admission and control connection.
// Every transition lives here, so a departure, a control reconnect and a new
// session from the same endpoint agree on which handle owns queued work.
#include "IrohRoom.hxx"
#include "RoomMessageQueue.hxx"
#include "RoomMessageTag.hxx"
#include "../common/SkillBadgeJson.hxx"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace sf4e { namespace session {

namespace {
// What a public host's helper says the ticket admitted, on "connected".
Admission ReadAdmission(const nlohmann::json& event) {
	return {event.value("account", std::string()), ReadSkillBadge(event)};
}
}
using nlohmann::json;

std::string IrohRoom::PeerTag(const std::string& peer) { return peer.substr(0, 8); }

bool IrohRoom::IsEndpointIdentity(const std::string& identity) { return ::sf4e::IsEndpointIdentity(identity); }

// A present but malformed latency field reads as no route, and a relay that is
// not a region code as no hint. An event without the field leaves the route:
// a host hears one connected event per joiner, none of them its own.
void IrohRoom::ReadLocalLatency(const json& event) {
	const auto endpoint = event.find("latency");
	if (endpoint == event.end()) return;
	const auto relay = event.find("latency_relay");
	room_.localLatency = !endpoint->is_string() ? LatencyRoute{} : LatencyRoute::Parsed(endpoint->get<std::string>(),
		relay != event.end() && relay->is_string() ? relay->get<std::string>() : std::string());
}

std::string IrohRoom::PeerIdentity(Connection connection) const {
	if (connection == 1) return localIdentity_;
	const auto peer = room_.peers.find(connection);
	return peer == room_.peers.end() ? std::string() : peer->second.identity;
}

std::uint64_t IrohRoom::PeerIncarnation(Connection connection) const {
	const auto member = room_.memberIncarnations.find(PeerIdentity(connection));
	return member == room_.memberIncarnations.end() ? 0 : member->second;
}

// A retired connection has no record left; its closure is already queued for
// the server, which may still address it until it processes that closure.
bool IrohRoom::PeerControlClosed(Connection connection) const {
	const auto peer = room_.peers.find(connection);
	return peer == room_.peers.end() || !peer->second.control;
}

std::map<Connection, IrohRoom::Peer>::iterator IrohRoom::FindPeer(const std::string& identity) {
	return std::find_if(room_.peers.begin(), room_.peers.end(), [&](const std::pair<const Connection, Peer>& item) {
		return item.second.identity == identity;
	});
}

Connection IrohRoom::ConnectionForIdentity(const std::string& identity) const {
	if (identity == localIdentity_ && !identity.empty()) return 1;
	for (const auto& peer : room_.peers) if (peer.second.identity == identity) return peer.first;
	return 0;
}

std::map<Connection, std::string> IrohRoom::ControlIdentities() const {
	std::map<Connection, std::string> identities;
	if (!localIdentity_.empty()) identities.emplace(1, localIdentity_);
	for (const auto& peer : room_.peers) identities.emplace(peer.first, peer.second.identity);
	return identities;
}

IrohRoom::Peer* IrohRoom::AllocatePeer(const std::string& identity) {
	if (room_.peers.size() >= MaximumRemotePeers || nextConnection_ == (std::numeric_limits<Connection>::max)()) return nullptr;
	Peer peer;
	peer.identity = identity;
	peer.admitted = room_.memberIncarnations.count(identity) || replica_.committedMembers.count(identity);
	return &room_.peers.emplace(nextConnection_++, std::move(peer)).first->second;
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
// intents. Callers check room_.closed has room first.
std::map<Connection, IrohRoom::Peer>::iterator IrohRoom::RetirePeer(std::map<Connection, Peer>::iterator peer) {
	room_.closed.push_back(peer->first);
	return ErasePeer(peer);
}

std::map<Connection, IrohRoom::Peer>::iterator IrohRoom::ErasePeer(std::map<Connection, Peer>::iterator peer) {
	for (auto message = room_.serverMessages.begin(); message != room_.serverMessages.end();) {
		if (message->connection == peer->first) { room_.queuedBytes -= message->payload.size(); message = room_.serverMessages.erase(message); }
		else ++message;
	}
	return room_.peers.erase(peer);
}

void IrohRoom::PruneRetiredPeers() {
	// Wait until native application of the committed candidate has queued its
	// final effects. Socket loss alone never retires a stable member mapping,
	// and a record whose control is open is this endpoint's current session
	// even while the roster sets have yet to list it again.
	if (!replica_.haveCommittedMembers || !replica_.committedCheckpoints.empty() || !replica_.decoding.empty()) return;
	for (auto peer = room_.peers.begin(); peer != room_.peers.end();) {
		const auto& identity = peer->second.identity;
		if (!peer->second.admitted || peer->second.control) { ++peer; continue; }
		const auto game = room_.games.find(identity);
		if (room_.memberIncarnations.count(identity) || replica_.committedMembers.count(identity) ||
			(game != room_.games.end() && game->second.state != GameState::Closed)) { ++peer; continue; }
		peer = ErasePeer(peer);
	}
}

void IrohRoom::ExpireDepartedPeers() {
	if (room_.state != State::Ready || !room_.coordination.leaderLocal || !room_.coordination.writable) return;
	const auto now = GetTickCount64();
	for (auto peer = room_.peers.begin(); peer != room_.peers.end();) {
		if (!peer->second.departureDeadline || now < peer->second.departureDeadline) { ++peer; continue; }
		if (room_.closed.size() >= MaximumQueuedMessages) return; // Retried next poll.
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
	for (auto& peer : room_.peers)
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
	if (room_.coordination.leaderLocal)
		for (auto& peer : room_.peers)
			if (incarnations.count(peer.second.identity) && !connected.count(peer.second.identity))
				ControlClosed(peer.second, DepartureGraceMs);
	return true;
}

bool IrohRoom::HandleConnected(const json& event) {
	const auto identity = event.at("peer").get<std::string>();
	const auto room = event.at("room").get<std::array<std::uint8_t, 16>>();
	const auto control = event.at("control").get<std::uint64_t>();
	if (room_.hosting && room != room_.id) { Fail("wrong_room"); return false; }
	if (!room_.hosting && room_.state != State::Joining && !room_.coordination.active) { Fail("unexpected_peer"); return false; }
	// The route belongs to one room: a joiner's own join completion carries it.
	if (room != room_.id) room_.localLatency = {};
	ReadLocalLatency(event);
	const auto known = FindPeer(identity);
	if (room_.coordination.active && (known != room_.peers.end() || (!localIdentity_.empty() && identity == localIdentity_))) {
		if (known != room_.peers.end()) {
			ControlOpened(known->second, control); // Its control is back.
			auto admission = ReadAdmission(event);
			if (!admission.account.empty()) known->second.admission = std::move(admission);
		}
		return true;
	}
	if (known != room_.peers.end()) { Fail("duplicate_peer"); return false; }
	PruneRetiredPeers();
	auto* peer = AllocatePeer(identity);
	if (!peer) { Fail("peer_limit"); return false; }
	ControlOpened(*peer, control);
	peer->admission = ReadAdmission(event);
	room_.id = room;
	ReadyIfRebound();
	return true;
}

bool IrohRoom::HandleControlTraffic(const json& event, const std::string& type, std::optional<std::string_view> body) {
	const auto identity = event.at("peer").get<std::string>();
	const auto peer = FindPeer(identity);
	if (peer == room_.peers.end()) return true; // Late event for a departed peer.
	if (type == "peer_session") return BeginPeerSession(peer, event.at("incarnation").get<std::uint64_t>());
	if (type == "peer_departed") return PeerDeparted(peer);
	if (type == "message") return ReceiveControlMessage(peer, event, body);
	// control_closed. A close for a superseded connection says nothing about
	// the one this record holds now.
	const auto control = event.at("control").get<std::uint64_t>();
	if (peer->second.control && *peer->second.control != control) return true;
	if (room_.coordination.active) {
		// A control socket is not a membership decision. Retain its
		// stable mapping while committed coordination reconnects it.
		// A member whose game was killed never sends Leave. Its control
		// gets a grace period to reconnect; after that the committed
		// leader treats it as departed (ExpireDepartedPeers). That holds
		// for the leader too: once another member takes over, the former
		// leader's record is pruned or expired like any other.
		ControlClosed(peer->second, DepartureGraceMs);
		if (identity == room_.coordination.leader) {
			room_.coordination.writable = false; room_.coordination.rebound = false;
			room_.state = State::Degraded;
			room_.probe = {};
			// Make the freeze observable for one owner tick even if a
			// queued authority watch and rebound follow immediately. This
			// prevents the UI or SessionServer from issuing a mutation in
			// the same poll that detected loss of its leader control.
			return false;
		}
		return true;
	}
	if (room_.closed.size() >= MaximumQueuedMessages) { Fail("room_close_queue"); return false; }
	RetirePeer(peer);
	if (!room_.hosting) {
		// Keep the loss visible to the room controller while retaining
		// independently authorized gameplay mappings. New match
		// preparation is blocked by the degraded state.
		room_.state = State::Degraded;
		room_.error = "room_control_closed";
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
	if (room_.closed.size() >= MaximumQueuedMessages) { Fail("room_close_queue"); return false; }
	spdlog::info("Room: peer {} started a new session; connection {} retired", PeerTag(record.identity), peer->first);
	const auto identity = record.identity;
	const auto nextId = record.nextId;
	const auto control = record.control;
	const auto admission = record.admission;
	RetirePeer(peer);
	auto* fresh = AllocatePeer(identity);
	if (!fresh) { Fail("peer_limit"); return false; }
	fresh->nextId = nextId;
	fresh->incarnation = incarnation;
	fresh->admission = admission;
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
	if (!room_.coordination.leaderLocal || peer->second.identity == room_.coordination.leader) return true;
	if (room_.closed.size() >= MaximumQueuedMessages) { ControlClosed(peer->second, 0); return true; } // Retired once the queue drains.
	spdlog::info("Room: peer {} announced its departure", PeerTag(peer->second.identity));
	RetirePeer(peer);
	return true;
}

bool IrohRoom::ReceiveControlMessage(std::map<Connection, Peer>::iterator peer, const json& event,
	std::optional<std::string_view> body) {
	const auto id = event.at("message_id").get<std::int64_t>();
	// A raw-body frame carries the text as it is; a JSON one as a string field.
	auto payload = body ? std::string(*body) : event.at("payload").get<std::string>();
	// A message that repeats a number or does not parse is skipped on its own;
	// the room and the peer's later messages carry on.
	json decoded;
	try { decoded = json::parse(payload); } catch (const json::exception&) {}
	const auto typeField = decoded.is_object() ? decoded.find("type") : decoded.end();
	if (id <= peer->second.receivedId || payload.empty() || payload.size() > MaximumPayload || !decoded.is_object() ||
		(typeField != decoded.end() && !typeField->is_string())) {
		spdlog::warn("Room: control message dropped peer={} id={} last={} bytes={}", PeerTag(peer->second.identity), id,
			peer->second.receivedId, payload.size());
		return true;
	}
	peer->second.receivedId = id;
	// Authority watches and control streams can arrive in either
	// order during takeover. Committed effects always go through
	// recipient validation; client intents wait behind the server's
	// local committed-leader gate regardless of the current UI role.
	const bool effect = decoded.contains("_commit");
	// Only a bare admission response from the committed
	// leader enters the local client queue.  Other untagged
	// peer payloads remain server intents.
	const auto messageType = decoded.value("type", std::string());
	const bool fromLeader = !effect && room_.coordination.active && room_.coordination.writable &&
		peer->second.identity == room_.coordination.leader;
	const bool leaderHandshake = fromLeader && room_.pendingAdmission &&
		(messageType == "hello_resp" || messageType == "join_rej");
	// Verification is forwarded by the leader without a commit
	// token; it carries no room mutation, so it is delivered
	// directly rather than through a checkpoint.
	const bool leaderVerification = fromLeader && IsVerificationType(messageType);
	// A public host has no local client to drain the client queue, so a member
	// sending commit-tagged payloads would fill it and fail the room.
	if (room_.publicHost && (effect || leaderHandshake || leaderVerification)) return true;
	// Classified from this one parse; neither queue nor the session layer
	// parses it again to learn its kind.
	return Queue((effect || leaderHandshake || leaderVerification) ? room_.clientMessages : room_.serverMessages,
		{peer->first, id, std::move(payload), "", ClassifyRoomMessage(decoded)});
}

SendResult IrohRoom::SendRemote(Connection connection, const std::string& payload, std::int64_t* id) {
	if (room_.state != State::Ready) return SendResult::NotConnected;
	if (payload.empty() || payload.size() > MaximumPayload) return SendResult::InvalidPayload;
	auto peer = room_.peers.find(connection);
	if (peer == room_.peers.end() || !peer->second.control) return SendResult::NotConnected;
	if (peer->second.nextId == (std::numeric_limits<std::int64_t>::max)()) return SendResult::Failed;
	const auto messageId = peer->second.nextId;
	// Named for the control this record holds: the helper refuses the send if
	// that endpoint has since connected again for a session this room has
	// not been told about, instead of delivering old numbering into it.
	// The payload goes as the frame's raw body, not escaped into the header.
	const auto header = json{{"type", "send"}, {"epoch", epoch_}, {"peer", peer->second.identity},
		{"control", *peer->second.control}, {"message_id", messageId}}.dump();
	if (!Command(platform::helper_frame::EncodeRawBody(header, payload.data(), payload.size()))) return SendResult::QueueFull;
	++peer->second.nextId;
	if (id) *id = messageId;
	return SendResult::Queued;
}

} }
