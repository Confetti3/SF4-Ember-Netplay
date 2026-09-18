#include "IrohMatchSession.hxx"
#include "../common/RoomLimits.hxx"
#include <algorithm>
#include <set>

namespace sf4e { namespace session {
using nlohmann::json;

IrohMatchSession::IrohMatchSession(SessionClient& client, std::shared_ptr<IrohRoom> room, Clock clock)
	: client_(client), room_(std::move(room)), clock_(std::move(clock)) {
	client_.RequireMatchAuthorization();
	WSADATA data; winsock_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
IrohMatchSession::~IrohMatchSession() {
	ReleasePortToGgpo(); ClearLinks();
	if (winsock_) WSACleanup();
}
void IrohMatchSession::ClearLinks() {
	for (auto& link : links_) SecureZeroMemory(link.capability.data(), link.capability.size());
	links_.clear();
}
ULONGLONG IrohMatchSession::Now() const {
	return clock_ ? clock_() : GetTickCount64();
}
void IrohMatchSession::ReleasePortToGgpo() {
	if (reservedPort_ != INVALID_SOCKET) { closesocket(reservedPort_); reservedPort_ = INVALID_SOCKET; }
}
bool IrohMatchSession::Fail(const char* error) { error_ = error; phase_ = Phase::Failed; return false; }
bool IrohMatchSession::Acknowledge(const char* type, json extra) {
	if (!type || generation_ == 0) return Fail("match_control_send_failed");
	const std::string name(type);
	for (const auto& pending : pendingAcks_)
		if (pending.generation == generation_ && pending.type == name) return true;
	json message = std::move(extra);
	message["type"] = name; message["generation"] = generation_;
	const auto result = client_.Send(message, nullptr);
	if (result == SendResult::Queued) return true;
	// A temporary control outage is recoverable. Keep the logical
	// acknowledgement generation-bound and let Tick flush it once the room
	// stream is writable again; phase advancement remains valid because the
	// authority may already have committed the corresponding preparation.
	if (result == SendResult::NotConnected || result == SendResult::QueueFull) {
		if (pendingAcks_.size() >= 8) return Fail("match_control_queue_full");
		pendingAcks_.push_back({name, generation_, std::move(message)});
		return true;
	}
	return Fail("match_control_send_failed");
}

bool IrohMatchSession::FlushPendingAcks() {
	for (auto it = pendingAcks_.begin(); it != pendingAcks_.end();) {
		if (it->generation != generation_) { it = pendingAcks_.erase(it); continue; }
		const auto result = client_.Send(it->message, nullptr);
		if (result == SendResult::Queued) { it = pendingAcks_.erase(it); continue; }
		if (result == SendResult::NotConnected || result == SendResult::QueueFull) return true;
		return Fail("match_control_send_failed");
	}
	return true;
}

bool IrohMatchSession::ControlReadyForSetup() const {
    return room_ && room_->ReadyForMatch();
}

bool IrohMatchSession::StartConnecting() {
    if (phase_ != Phase::Prepared || !ControlReadyForSetup()) return false;
    for (const auto& link : links_) if (link.dial) {
        if (!room_->PrepareGame(link.peer, generation_, link.capability, client_._ggpoPort,
            GgpoMaximumPacket, true)) return Fail("match_connection_failed");
        mappingsPrepared_ = true;
    }
    phase_ = Phase::Connecting;
    // The committed connect instruction starts a new bounded phase. Waiting
    // for every participant to prepare listeners must not consume the time
    // allowed to establish those connections and commit their ready replies.
    // Duplicate instructions cannot renew this window: only Prepared enters.
    deadlineRemaining_ = 30000;
    deadline_ = Now() + deadlineRemaining_;
    deadlineSuspended_ = false;
    return true;
}

bool IrohMatchSession::StartQueuedSetup() {
	if (replacementRetiring_) {
		pendingGrant_ = nullptr;
		pendingGrantTerm_ = 0;
		waitingForProjection_ = false;
		pendingConnect_ = pendingStart_ = false;
		return true;
	}
    if (room_ && room_->Coordination().active && pendingGrantTerm_ &&
        room_->Coordination().term != pendingGrantTerm_) {
        // A private grant which has not been accepted belongs to the old
        // authority term and must not be replayed by a newly rebound room.
        // Once AcceptGrant has established helper mappings, however, the
        // native socket and those mappings remain owned by this generation
        // until the committed game_end path retires them.  Clearing them here
        // would lose the close bookkeeping (and can strand a live GGPO port).
        if (phase_ == Phase::Idle || phase_ == Phase::Ending) {
            pendingGrant_ = nullptr;
            pendingGrantTerm_ = 0;
			waitingForProjection_ = false;
            pendingConnect_ = pendingStart_ = false;
        } else {
            // This is an admitted generation.  Keep all setup state and let a
            // committed game_end drive normal Ending/native socket retirement;
            // avoid reapplying the term fence on every Tick meanwhile.
            pendingGrantTerm_ = 0;
        }
    }
    if (!ControlReadyForSetup()) return true;
    if (phase_ == Phase::Idle && !pendingGrant_.is_null()) {
		try { if (!AcceptGrant(pendingGrant_)) return false; }
        catch (const json::exception&) { return Fail("invalid_match_message"); }
		// A private grant and its separately committed public projection may
		// occupy different transport polls. Keep the exact grant staged until the
		// generation projection arrives or a term/cancel/timeout fence retires it.
		if (phase_ == Phase::Idle) return true;
		pendingGrant_=nullptr;
		pendingGrantTerm_=0;
		waitingForProjection_=false;
    }
    if (pendingConnect_ && phase_ == Phase::Prepared) {
        pendingConnect_=false;
        if (!StartConnecting()) return false;
    }
    // game_start can outrun this spectator's own ready report; it starts once
    // its link to P1 is up and that report has gone out.
    if (pendingStart_ && phase_ == Phase::Connecting && readySent_) {
        pendingStart_=false;
        phase_=Phase::Started;
    }
    return true;
}

bool IrohMatchSession::AcceptGrant(const json& message) {
	const auto generation = message.at("generation").get<std::uint64_t>();
	if (generation <= generation_) return true; // A stale room message cannot reopen a match.
	if (phase_ != Phase::Idle || !winsock_ || message.at("version") != 1 ||
		message.at("room").get<std::array<std::uint8_t, 16>>() != room_->RoomId() ||
		message.at("local_identity").get<std::string>() != room_->LocalIdentity() ||
		message.at("max_packet").get<std::size_t>() != GgpoMaximumPacket) return Fail("invalid_match_permission");
	auto roster = message.at("roster").get<std::vector<SessionProtocol::ConnectionID>>();
	const auto slot = message.at("slot").get<std::size_t>();
	if (roster.size() < 2 || roster.size() > room::MaxMatchParticipants || slot >= roster.size() ||
		!(roster[slot] == client_._cid)) return Fail("invalid_match_roster");
	// Freeze first, then apply the separately committed projection for this
	// generation. Recovery delivery may place game_prepare before data_update;
	// comparing against the preceding Waiting roster would reject queued members
	// which BeginMatch has just promoted to native spectators.
	client_.FreezeRoomProjection();
	if (client_.IsCustomRoom() && !client_.ApplyPendingRoomProjection(generation)) {
		if (!waitingForProjection_) {
			waitingForProjection_ = true;
			deadlineRemaining_ = 30000;
			deadline_ = Now() + deadlineRemaining_;
			deadlineSuspended_ = false;
		}
		return true;
	}
	waitingForProjection_ = false;
	// _lobbyData is the local table projection. Room membership can be sixteen
	// members, while this native session contains two fighters and admitted
	// spectators for its selected table.
	if (roster.size() != client_._lobbyData.members.size()) return Fail("invalid_match_roster");
	for (std::size_t i = 0; i < roster.size(); ++i) if (!(roster[i] == client_._lobbyData.members[i].connId)) return Fail("stale_match_roster");
	const auto& edges = message.at("links");
	if (!edges.is_array() || edges.size() != (slot == 0 ? roster.size() - 1 : 1)) return Fail("invalid_match_links");
	ClearLinks();
	std::set<std::size_t> slots;
	std::set<std::string> peers;
	for (const auto& edge : edges) {
		Link link{edge.at("slot").get<std::size_t>(), edge.at("peer").get<std::string>(),
			edge.at("capability").get<std::array<std::uint8_t, 32>>(), edge.at("dial").get<bool>()};
		if (link.slot >= roster.size() || link.slot == slot || (slot != 0 && link.slot != 0) ||
			link.dial != (slot != 0) || !slots.insert(link.slot).second || !peers.insert(link.peer).second ||
			link.peer == room_->LocalIdentity() || link.peer.size() != 64 ||
			std::all_of(link.capability.begin(), link.capability.end(), [](std::uint8_t b) { return b == 0; })) return Fail("invalid_match_link");
		links_.push_back(std::move(link));
	}
	reservedPort_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (reservedPort_ == INVALID_SOCKET) return Fail("local_port_unavailable");
	sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(reservedPort_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return Fail("local_port_unavailable");
	int size = sizeof(address);
	if (getsockname(reservedPort_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return Fail("local_port_unavailable");
	client_._ggpoPort = ntohs(address.sin_port);
	generation_ = generation; slot_ = slot; roster_ = std::move(roster);
	client_.SetGameplayGeneration(generation_);
	pendingGrantTerm_ = room_ && room_->Coordination().active ? room_->Coordination().term : 0;
	readySent_ = mappingsPrepared_ = endSent_ = false;
	roomEndReceived_ = false;
	spectatorsOptional_ = message.value("spectators_optional", false);
	fighterReadyAt_ = 0;
	fighterClosedAt_ = 0;
	pendingAcks_.clear();
	pendingConnect_ = pendingStart_ = false;
	deadlineSuspended_ = false;
	deadlineRemaining_ = 30000;
	phase_ = Phase::Preparing; deadline_ = Now() + deadlineRemaining_;
	for (const auto& link : links_) if (!link.dial) {
		if (!room_->PrepareGame(link.peer, generation_, link.capability, client_._ggpoPort, GgpoMaximumPacket, false)) return Fail("match_listener_failed");
		mappingsPrepared_ = true;
	}
	return true;
}

void IrohMatchSession::End() {
	if (phase_ != Phase::Idle && phase_ != Phase::Failed) {
		pendingAcks_.clear();
		phase_ = Phase::Ending;
		teardown_.RequestEnd();
	}
}

bool IrohMatchSession::Abort() {
	if (phase_ == Phase::Idle) {
		pendingGrant_ = nullptr;
		pendingGrantTerm_ = 0;
		waitingForProjection_ = false;
		pendingConnect_ = pendingStart_ = false;
		pendingAcks_.clear();
		return true;
	}
	// A helper close timeout is terminal for this room epoch. Keep the failed
	// fence in place until the application closes the room; re-entering Ending
	// would permit a later match to reuse a port while the old bridge is still
	// alive.
	if (phase_ == Phase::Failed && error_ == "match_teardown_timeout") {
		if (room_) room_->Leave();
		return false;
	}
	// The native caller owns the socket retirement boundary. Once it calls
	// Abort, use the normal helper teardown path but do not wait for a room
	// authority game_end that may never arrive after a failed preparation.
	phase_ = Phase::Ending;
	teardown_.RequestEnd();
	roomEndReceived_ = true;
	pendingGrant_ = nullptr;
	waitingForProjection_ = false;
	pendingAcks_.clear();
	error_.clear();
	return true;
}

bool IrohMatchSession::BeginReplacement() {
	replacementRetiring_ = true;
	return Abort();
}

bool IrohMatchSession::CanReplace(bool ggpoOwnsSocket) const {
	if (ggpoOwnsSocket || reservedPort_ != INVALID_SOCKET) return false;
	if (!generation_) return true;
	if (!endSent_) return false;
	if (!mappingsPrepared_) return true;
	return room_ && std::all_of(links_.begin(), links_.end(), [&](const Link& link) {
		const auto game = room_->Game(link.peer);
		return game.generation == generation_ && game.state == IrohRoom::GameState::Closed;
	});
}

bool IrohMatchSession::CanBeginReplacement(bool ggpoOwnsSocket) const {
	return !ggpoOwnsSocket && phase_ != Phase::Started && phase_ != Phase::Failed;
}

bool IrohMatchSession::Tick(bool ggpoOwnsSocket) {
	if (phase_ == Phase::Failed) return false;
	json message;
		while (client_.TakeGameplayMessage(message)) {
		try {
			const auto type = message.at("type").get<std::string>();
			if (replacementRetiring_ &&
				(type == "game_prepare" || type == "game_connect" || type == "game_start")) continue;
			if (type == "game_prepare") {
				const auto grantGeneration=message.at("generation").get<std::uint64_t>();
				if (grantGeneration <= generation_) continue;
				if (phase_ == Phase::Ending || phase_ == Phase::Idle) {
					if (!pendingGrant_.is_null() && pendingGrant_.at("generation") != message.at("generation")) return Fail("overlapping_match_permission");
					if (pendingGrant_.is_null()) {
						pendingGrant_ = std::move(message);
						pendingGrantTerm_ = room_ && room_->Coordination().active ? room_->Coordination().term : 0;
						waitingForProjection_ = false;
					}
				} else {
					return Fail("overlapping_match_permission");
				}
				continue;
			}
			// A committed cancellation may arrive for a future grant while the
			// previous generation is still closing. Consume it before the generic
			// generation fence so the stale private permission cannot reopen.
			if (type == "game_end" && !pendingGrant_.is_null() &&
				message.at("generation").get<std::uint64_t>() == pendingGrant_.at("generation").get<std::uint64_t>()) {
				pendingGrant_=nullptr; pendingGrantTerm_=0; waitingForProjection_=false; pendingConnect_=pendingStart_=false;
				continue;
			}
			if (message.at("generation").get<std::uint64_t>() != generation_) continue;
			if (type == "game_peer_end") {
				const auto peerSlot = message.at("slot").get<std::size_t>();
				if (phase_ == Phase::Ending) continue;
				// Only local P1 owns spectator edges. A peer-end for a fighter or
				// a non-P1 recipient is an authority/protocol mismatch. Before the
				// native session is Started, the fixed GGPO roster cannot shrink;
				// fail setup so the authority can issue a fresh reduced grant.
				if (slot_ != 0 || peerSlot < 2 || peerSlot >= roster_.size()) return Fail("invalid_match_peer_end");
				// Without the authority's optional-spectator offer a pre-start
				// peer end cannot be absorbed: the older authority still waits
				// for that spectator.
				if (phase_ != Phase::Started && !spectatorsOptional_) return Fail("spectator_peer_ended_before_start");
				auto link = std::find_if(links_.begin(), links_.end(), [&](const Link& candidate) {
					return candidate.slot == peerSlot;
				});
				if (link == links_.end()) continue; // Tolerate a late duplicate event.
				if (!DropSpectatorLink(link)) return false;
				continue;
			}
			if (type == "game_end") { roomEndReceived_ = true; End(); continue; }
			if (type == "game_connect" && phase_ == Phase::Prepared) {
				if (!ControlReadyForSetup()) pendingConnect_=true;
				else if (!StartConnecting()) return false;
			} else if (type == "game_connect" && phase_ == Phase::Preparing) {
				// The fighters may be told to connect before this spectator has
				// prepared; connect as soon as it has.
				pendingConnect_=true;
			} else if (type == "game_start" && phase_ == Phase::Connecting) {
				if (!ControlReadyForSetup() || !readySent_) pendingStart_=true;
				else phase_ = Phase::Started;
			}
		} catch (const json::exception&) { return Fail("invalid_match_message"); }
	}
	// A coordinated control outage is not a setup failure. Preserve the
	// remaining relative deadline while the room is non-writable, then rebase
	// it on the first healthy clock value. Committed game_end messages above
	// are still processed, so native ownership is retained until teardown.
	const auto now = Now();
	const bool coordinatedPause = room_ && !ControlReadyForSetup();
	if (coordinatedPause && phase_ != Phase::Started && phase_ != Phase::Ending &&
		(phase_ != Phase::Idle || waitingForProjection_) && !deadlineSuspended_) {
		deadlineRemaining_ = now < deadline_ ? deadline_ - now : 0;
		deadlineSuspended_ = true;
	} else if (!coordinatedPause && deadlineSuspended_) {
		deadline_ = now + deadlineRemaining_;
		deadlineSuspended_ = false;
	}
	if (!FlushPendingAcks()) return false;
	if (phase_ == Phase::Ending) {
		if (ggpoOwnsSocket) return true;
		ReleasePortToGgpo();
		if (!endSent_) {
			if (mappingsPrepared_ && !room_->EndMatch(generation_)) {
				// A failed first dispatch cannot establish a safe helper-close
				// boundary. Fail closed while native ownership is already gone;
				// no later match may reuse a stale helper mapping.
				return Fail("match_teardown_failed");
			}
			endSent_ = true;
			// The helper timer starts only after native GGPO has released the
			// socket and this game generation has dispatched its close command.
			if (mappingsPrepared_) teardown_.DispatchHelperClose(now);
		}
		const bool closed = std::all_of(links_.begin(), links_.end(), [&](const Link& link) {
			return room_->Game(link.peer).state == IrohRoom::GameState::Closed;
		});
		if (closed && roomEndReceived_) {
			phase_ = Phase::Idle; ClearLinks(); teardown_ = MatchTeardownTiming();
			// The helper mappings and native socket are both retired here. Keep
			// projection release at the coordinator boundary so direct session
			// users cannot observe the prior table as a fresh match roster.
			client_.ReleaseRoomProjection();
			// A next generation may already be committed while the old helper
			// links close.  Keep it staged until the room is writable and its
			// checkpoint effects have been activated.
			if (!StartQueuedSetup()) return false;
		} else if (teardown_.HelperTimedOut(now, closed)) {
			// A missing close acknowledgement means the helper may still forward
			// packets. Fail closed and tear down the room epoch instead of clearing
			// bookkeeping and allowing a fresh match to reuse this local port.
			pendingGrant_ = nullptr;
			waitingForProjection_ = false;
			roomEndReceived_ = true;
			if (room_) room_->Leave();
			error_ = "match_teardown_timeout";
			phase_ = Phase::Failed;
			return false;
		}
		return true;
	}
	if (!StartQueuedSetup()) return false;
	if (phase_ == Phase::Idle) {
		if (waitingForProjection_ && !deadlineSuspended_ && now > deadline_) return Fail("match_setup_timeout");
		return true;
	}
	if (phase_ != Phase::Started && !deadlineSuspended_ && now > deadline_) return Fail("match_setup_timeout");
	if (phase_ == Phase::Preparing) {
		const bool waiting = std::all_of(links_.begin(), links_.end(), [&](const Link& link) {
			const auto state = room_->Game(link.peer).state;
			return link.dial || state == IrohRoom::GameState::Waiting || state == IrohRoom::GameState::Ready;
		});
		if (waiting) {
			// P1 accepts the authority's offer, which switches the start barrier
			// to the two fighters.
			json extra = json::object();
			if (slot_ == 0 && spectatorsOptional_) extra["spectators_optional"] = true;
			if (!Acknowledge("game_prepared", std::move(extra))) return false;
			phase_ = Phase::Prepared;
		}
	}
	if (phase_ == Phase::Connecting || phase_ == Phase::Started) {
		const bool optionalSpectators = slot_ == 0 && (phase_ == Phase::Started || spectatorsOptional_);
		bool fighterReady = true, spectatorsReady = true;
		for (auto link = links_.begin(); link != links_.end();) {
			const auto state = room_->Game(link->peer).state;
			const bool closed = state == IrohRoom::GameState::Closed || state == IrohRoom::GameState::Closing;
			if (closed && optionalSpectators && link->slot >= 2) {
				if (!DropSpectatorLink(link)) return false;
				continue;
			}
			if (closed) {
				if (!fighterClosedAt_) fighterClosedAt_ = now;
				if (now - fighterClosedAt_ < PeerCloseGraceMs) { fighterReady = false; ++link; continue; }
                const auto failure=room_->Game(link->peer).error;
                return Fail(failure.empty()?"gameplay_connection_lost":failure.c_str());
            }
			const bool up = state == IrohRoom::GameState::Ready;
			if (slot_ == 0 && link->slot >= 2) spectatorsReady = spectatorsReady && up;
			else fighterReady = fighterReady && up;
			++link;
		}
		if (!readySent_ && fighterReady && !ReportReady(now, spectatorsReady)) return false;
	}
	return true;
}

// Sends game_ready once this client's links allow it. False only on failure.
bool IrohMatchSession::ReportReady(ULONGLONG now, bool spectatorsReady) {
	json extra = json::object();
	if (slot_ == 0 && spectatorsOptional_) {
		if (!fighterReadyAt_) fighterReadyAt_ = now;
		if (!spectatorsReady) {
			if (now - fighterReadyAt_ < SpectatorGraceMs) return true;
			// Late spectators sit this generation out.
			for (auto link = links_.begin(); link != links_.end();) {
				if (link->slot >= 2 && room_->Game(link->peer).state != IrohRoom::GameState::Ready) {
					if (!DropSpectatorLink(link)) return false;
				} else ++link;
			}
		}
		// P1 owns the spectator links, so it reports which of them came up.
		json slots = json::array();
		for (const auto& link : links_) if (link.slot >= 2) slots.push_back(link.slot);
		extra["slots"] = std::move(slots);
	} else if (!spectatorsReady) return true;
	if (!Acknowledge("game_ready", std::move(extra))) return false;
	readySent_ = true;
	return true;
}

bool IrohMatchSession::DropSpectatorLink(std::vector<Link>::iterator& link) {
	if (!room_->EndPeer(link->peer, generation_)) return Fail("spectator_teardown_failed");
	SecureZeroMemory(link->capability.data(), link->capability.size());
	link = links_.erase(link);
	return true;
}

std::uint16_t IrohMatchSession::RemotePort(const SessionProtocol::ConnectionID& member) const {
	if (phase_ != Phase::Started && phase_ != Phase::Ending) return 0;
	for (const auto& link : links_) if (roster_[link.slot] == member) {
		const auto game = room_->Game(link.peer);
		return game.state == IrohRoom::GameState::Ready && game.generation == generation_ ? game.virtualPort : 0;
	}
	return 0;
}

} }
