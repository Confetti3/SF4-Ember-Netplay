#include "RoomHost.hxx"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace sf4e { namespace roomhost {
using nlohmann::json;

namespace {
// The helper reports its home relay shortly after it is online. A room is
// listed with a region, so wait this long for one before settling for "other".
constexpr std::uint64_t RegionWaitMs = 5000;
// A checkpoint import that keeps failing leaves the authority read-only.
constexpr std::uint64_t RecoveryFailureLimitMs = 30000;

int HexDigit(char c) {
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

std::string HexLower(const std::array<std::uint8_t, 16>& bytes) {
	constexpr char digits[] = "0123456789abcdef";
	std::string text;
	for (const auto byte : bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
	return text;
}

// A printable token as the supervisor validates it (protocol.rs is_token).
bool IsToken(const std::string& text, std::size_t limit) {
	return !text.empty() && text.size() <= limit &&
		std::all_of(text.begin(), text.end(), [](char c) { return c > 0x20 && c < 0x7f; });
}
}

bool ParseConfig(const std::string& line, Config& config, std::string& error) {
	const auto fail = [&](const char* field) { error = field; return false; };
	json value;
	try { value = json::parse(line); } catch (const json::exception&) { return fail("json"); }
	if (!value.is_object()) return fail("json");
	const auto text = [&](const char* field) {
		const auto found = value.find(field);
		return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
	};
	const auto number = [&](const char* field, std::uint64_t maximum, std::uint64_t& out) {
		const auto found = value.find(field);
		if (found == value.end() || !found->is_number_unsigned()) return false;
		out = found->get<std::uint64_t>();
		return out <= maximum;
	};
	const auto roomId = text("room_id");
	if (roomId.size() != 32) return fail("room_id");
	bool zero = true;
	for (std::size_t i = 0; i < 16; ++i) {
		const int high = HexDigit(roomId[i * 2]), low = HexDigit(roomId[i * 2 + 1]);
		if (high < 0 || low < 0) return fail("room_id");
		config.roomId[i] = static_cast<std::uint8_t>(high << 4 | low);
		zero = zero && config.roomId[i] == 0;
	}
	if (zero) return fail("room_id");
	config.name = text("name");
	if (config.name.empty() || config.name.size() > 64) return fail("name");
	std::uint64_t capacity = 0, port = 0, coordinationPort = 0;
	if (!number("capacity", room::MaximumMembers, capacity) || capacity < 2) return fail("capacity");
	config.capacity = static_cast<std::uint8_t>(capacity);
	config.buildId = text("build_id");
	if (!IsToken(config.buildId, 128)) return fail("build_id");
	config.creator = text("creator");
	config.bridgeId = text("bridge_id");
	if (!IsToken(config.bridgeId, 128)) return fail("bridge_id");
	config.ticketKey = text("ticket_key");
	if (!IsToken(config.ticketKey, 256)) return fail("ticket_key");
	config.ticketKid = text("ticket_kid");
	if (!IsToken(config.ticketKid, 128)) return fail("ticket_kid");
	config.helper = text("helper");
	if (config.helper.empty()) return fail("helper");
	// Absent on a hand-written line; the supervisor always sends both.
	if (value.contains("port") && !number("port", 65535, port)) return fail("port");
	if (value.contains("coordination_port") && !number("coordination_port", 65535, coordinationPort)) return fail("coordination_port");
	config.port = static_cast<std::uint16_t>(port);
	config.coordinationPort = static_cast<std::uint16_t>(coordinationPort);
	return true;
}

std::string HostedLine(const std::string& invitation, const std::string& region) {
	return json{{"type", "hosted"}, {"invitation", invitation}, {"region", region}}.dump();
}

std::string ClosedLine(const std::string& reason) {
	return json{{"type", "closed"}, {"reason", reason}}.dump();
}

// SessionServer needs each connection's account before it processes that
// connection's join. The room's poll runs inside the server's own step, so a
// connection and its first messages can arrive in one batch: the accounts of
// everything a poll returns are supplied here, on the way through.
class RoomHost::AccountTransport final : public session::ServerTransport {
public:
	AccountTransport(std::unique_ptr<session::ServerTransport> inner, std::shared_ptr<session::IrohRoom> room,
		std::unique_ptr<SessionServer>& server) : inner_(std::move(inner)), room_(std::move(room)), server_(server) {}
	bool Listen(std::uint16_t port) override { return inner_->Listen(port); }
	bool Attach(session::Connection connection) override { return inner_->Attach(connection); }
	bool Poll(std::vector<session::Message>& messages, std::vector<session::Connection>& closed, std::size_t maximum) override {
		const bool polled = inner_->Poll(messages, closed, maximum);
		Supply(messages);
		return polled;
	}
	bool PollRecoveryPrefix(std::vector<session::Message>& messages, std::vector<session::Connection>& closed,
		std::size_t maximum, const BatchPredicate& batchable) override {
		const bool polled = inner_->PollRecoveryPrefix(messages, closed, maximum, batchable);
		Supply(messages);
		return polled;
	}
	bool Send(session::Connection connection, const std::string& payload) override { return inner_->Send(connection, payload); }
	void Close() override { inner_->Close(); }

private:
	void Supply(const std::vector<session::Message>& messages) {
		if (!server_) return;
		for (const auto& message : messages)
			server_->SetConnectionAccount(message.connection, room_->PeerAccount(message.connection));
	}
	std::unique_ptr<session::ServerTransport> inner_;
	std::shared_ptr<session::IrohRoom> room_;
	std::unique_ptr<SessionServer>& server_; // The owning RoomHost's; it outlives this transport.
};

RoomHost::RoomHost(std::shared_ptr<session::IrohRoom> room, Config config, Emit emit)
	: room_(std::move(room)), config_(std::move(config)), emit_(std::move(emit)) {}

RoomHost::~RoomHost() { server_.reset(); }

bool RoomHost::Open() {
	if (opened_) return false;
	opened_ = room_->HostPublic(config_.buildId, config_.ticketKey, config_.ticketKid, config_.bridgeId, config_.roomId, config_.creator);
	return opened_;
}

bool RoomHost::Fail(const std::string& reason) {
	if (error_.empty()) error_ = reason.empty() ? "room_failed" : reason;
	return false;
}

// The host half of the game's AttachRoom, with no local client: every
// connection is a remote member, so nothing here treats connection 1 as local.
void RoomHost::Attach() {
	auto transport = room_->Server();
	if (!transport) return;
	std::unique_ptr<session::ServerTransport> accounts(new AccountTransport(std::move(transport), room_, server_));
	// The build id is the sidecar hash a joining client must present.
	server_.reset(new SessionServer("iroh:" + HexLower(config_.roomId), config_.buildId, true, 3, {0, 99}, std::move(accounts)));
	const auto room = room_;
	server_->EnableMatchAuthorization(config_.roomId,
		[room](session::Connection connection) { return connection == 1 ? std::string() : room->PeerIdentity(connection); },
		[room](session::Connection connection) { return connection == 1 ? 0 : room->PeerIncarnation(connection); });
	if (!server_->EnableServerOwnedRooms(config_.name, config_.capacity, room_->Epoch(), room::PublicRoomRules())) {
		server_.reset();
		Fail("room_model_refused");
		return;
	}
	const auto& authority = room_->Coordination();
	if (authority.active) server_->SetAuthority(authority.term, authority.revision,
		authority.writable && authority.leaderLocal && authority.rebound);
	spdlog::info("RoomHost: authority attached room={} capacity={}", HexLower(config_.roomId).substr(0, 8), config_.capacity);
}

// `banned` is the committed moderation view. The room model has already removed
// and banned the account; the helper's ban is the outer door. It closes the
// member's control at once, so it waits a moment for the kick's commit to carry
// the removal notice to that member.
void RoomHost::ForwardBans(const std::vector<std::string>& banned, std::uint64_t nowMs) {
	constexpr std::uint64_t NoticeMs = 1000;
	for (const auto& account : banned) {
		if (forwardedBans_.count(account)) continue;
		const auto seen = pendingBans_.emplace(account, nowMs).first->second;
		if (nowMs - seen < NoticeMs || server_->HasRecoveryCandidate() || server_->PendingProposal()) continue;
		// Retried next tick when the helper's queue is full.
		if (!room_->BanAccount(account)) continue;
		forwardedBans_.insert(account);
		pendingBans_.erase(account);
		spdlog::info("RoomHost: account banned");
	}
}

void RoomHost::Report(std::uint64_t nowMs) {
	if (!room_->Invitation().empty()) invitation_ = room_->Invitation();
	if (invitation_.empty()) return;
	if (!hosted_) {
		const auto& network = room_->Network();
		if (network.relay.empty() && nowMs - readySinceMs_ < RegionWaitMs) return;
		emit_(HostedLine(invitation_, network.relay.empty() ? "other" : network.relay));
		hosted_ = true;
	}
	const auto* snapshot = server_->RoomSnapshot();
	if (!snapshot) return;
	Reported current;
	current.members = snapshot->members.size();
	for (const auto& table : snapshot->tables)
		if (table.phase == room::TablePhase::Playing || table.phase == room::TablePhase::Paused) ++current.tablesPlaying;
	current.invitation = invitation_;
	// Committed bans only: a kick still in an uncommitted candidate could be
	// discarded, and neither the bridge nor the helper can take a ban back.
	current.banned = server_->BannedAccounts();
	current.details = DetailsOf(*snapshot);
	ForwardBans(current.banned, nowMs);
	// The model can close the room on its own; treat it like a close request
	// whose commit has already been applied.
	if (snapshot->closed && !closing_) { closing_ = true; closeApplied_ = true; closeStartedMs_ = nowMs; closeReason_ = "room_closed"; }
	if (haveReported_ && current == reported_) return;
	const auto line = StatusLine(current.members, current.tablesPlaying, current.invitation, current.banned, current.details);
	// The supervisor kills a child whose line is over its limit; end the room instead.
	if (line.size() > MaximumStatusLineBytes) { Fail("status_too_large"); return; }
	emit_(line);
	reported_ = std::move(current);
	haveReported_ = true;
}

bool RoomHost::Tick(std::uint64_t nowMs) {
	if (!error_.empty()) return false;
	if (!opened_) return Fail("not_opened");
	using State = session::IrohRoom::State;
	if (!server_) {
		room_->Poll();
		const auto state = room_->GetState();
		if (state == State::Failed || state == State::Idle || state == State::Closing) return Fail(room_->Error());
		if (state != State::Ready) return true;
		readySinceMs_ = nowMs;
		Attach();
		if (!server_) return Fail("server_transport_unavailable");
	}
	// The game's order: observe coordination and import commits, run the
	// authority's timers, then let the server consume member intents.
	if (!recovery_.Tick(*server_, *room_)) {
		if (recoveryError_ != recovery_.Error()) {
			recoveryError_ = recovery_.Error();
			spdlog::warn("RoomHost: recovery failed: {}", recoveryError_);
		}
		if (!recoveryFailedSinceMs_) recoveryFailedSinceMs_ = nowMs;
		else if (nowMs - recoveryFailedSinceMs_ >= RecoveryFailureLimitMs) return Fail("recovery_failed");
	} else {
		recoveryFailedSinceMs_ = 0;
		recoveryError_.clear();
	}
	if (room_->GetState() == State::Failed) return Fail(room_->Error());
	server_->AdvanceCustomRoom(nowMs);
	if (server_->Step() != 0) return Fail(room_->Error().empty() ? "server_step_failed" : room_->Error());
	// The last member left on purpose: nobody is coming back, so the room ends
	// now and stops counting as its creator's open room. A member who dropped
	// leaves the room to the supervisor's grace instead.
	if (!closing_ && server_->ServerOwnedRoomLeftEmpty()) {
		closing_ = true; closeStartedMs_ = nowMs; closeReason_ = "last_member_left";
		spdlog::info("RoomHost: the last member left; closing the room");
	}
	// Refused while a commit is in flight; asked again next tick.
	if (closing_ && !closeApplied_) closeApplied_ = server_->CloseServerOwnedRoom();
	Report(nowMs);
	return true;
}

void RoomHost::BeginClose(std::uint64_t nowMs) {
	if (closing_) return;
	closing_ = true;
	closeStartedMs_ = nowMs;
}

// Members should see the room closed, not a host that went silent: the close
// is a commit like any other, and its effects need a moment on the wire.
bool RoomHost::CloseDelivered(std::uint64_t nowMs) {
	constexpr std::uint64_t LingerMs = 300;
	if (!server_ || !error_.empty() || nowMs - closeStartedMs_ >= CloseWaitMs) return true;
	if (!closeApplied_ || server_->HasRecoveryCandidate() || server_->PendingProposal()) return false;
	if (!closeCommittedMs_) closeCommittedMs_ = nowMs;
	return nowMs - closeCommittedMs_ >= LingerMs;
}

void RoomHost::Leave() {
	server_.reset();
	room_->Leave();
}

bool RoomHost::Leaving() {
	room_->Poll();
	return room_->GetState() == session::IrohRoom::State::Closing;
}

} }
