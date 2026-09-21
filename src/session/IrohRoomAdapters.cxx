// IrohRoom: the ServerTransport and ClientTransport views handed to the session layer.
#include "IrohRoom.hxx"
#include "RoomMessageQueue.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "../common/RoomLimits.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace sf4e { namespace session {
using nlohmann::json;

class IrohRoom::ServerAdapter final : public ServerTransport {
	std::shared_ptr<IrohRoom> room;
	std::uint64_t epoch;
public:
	explicit ServerAdapter(std::shared_ptr<IrohRoom> owner) : room(std::move(owner)), epoch(room->epoch_) {}
	~ServerAdapter() override { Close(); }
	bool Listen(std::uint16_t) override { return (room->hosting_ || room->coordination_.active) && room->epoch_ == epoch; }
	bool Attach(Connection) override { return false; }
	bool Poll(std::vector<Message>& messages, std::vector<Connection>& closed, std::size_t maximum) override {
		room->Poll();
		if (room->epoch_ != epoch || room->state_ == State::Failed || room->state_ == State::Idle || room->state_ == State::Closing) return false;
        if(room->coordination_.active && (!room->coordination_.writable || !room->coordination_.leaderLocal || !room->coordination_.rebound)) return true;
		closed.swap(room->closed_);
		const auto queued=room->serverMessages_.size();
		for(std::size_t examined=0;examined<queued && messages.size()<maximum && !room->serverMessages_.empty();++examined) {
			if(room->coordination_.active && !room->PeerIncarnation(room->serverMessages_.front().connection)) {
				auto deferred=std::move(room->serverMessages_.front());room->serverMessages_.pop_front();
				room->serverMessages_.push_back(std::move(deferred));continue;
			}
			room->queuedBytes_ -= room->serverMessages_.front().payload.size();
			messages.push_back(std::move(room->serverMessages_.front())); room->serverMessages_.pop_front();
		}
		return true;
	}
	bool PollRecoveryPrefix(std::vector<Message>& messages, std::vector<Connection>& closed,
		std::size_t maximum, const BatchPredicate& batchable) override {
		room->Poll();
		if (room->epoch_ != epoch || room->state_ == State::Failed || room->state_ == State::Idle || room->state_ == State::Closing) return false;
		if (room->coordination_.active && (!room->coordination_.writable || !room->coordination_.leaderLocal || !room->coordination_.rebound)) return true;
		closed.swap(room->closed_);
		const auto queued = room->serverMessages_.size();
		bool prefixBatchable = false;
		for (std::size_t examined = 0; examined < queued && messages.size() < maximum && !room->serverMessages_.empty(); ++examined) {
			if (room->coordination_.active && !room->PeerIncarnation(room->serverMessages_.front().connection)) {
				// Preserve the batch boundary once an authenticated prefix has begun;
				// the deferred binding remains ahead of every later intent.
				if (!messages.empty()) break;
				auto deferred = std::move(room->serverMessages_.front()); room->serverMessages_.pop_front();
				room->serverMessages_.push_back(std::move(deferred)); continue;
			}
			if (!messages.empty() && (!prefixBatchable || !batchable(room->serverMessages_.front()))) break;
			if (messages.empty()) prefixBatchable = batchable(room->serverMessages_.front());
			room->queuedBytes_ -= room->serverMessages_.front().payload.size();
			messages.push_back(std::move(room->serverMessages_.front())); room->serverMessages_.pop_front();
			if (!prefixBatchable) break;
		}
		return true;
	}
	bool Send(Connection connection, const std::string& payload) override {
		if (room->epoch_ != epoch || room->state_ != State::Ready) return false;
		if (connection != 1) return room->SendRemote(connection, payload, nullptr) == SendResult::Queued;
		if (!room->localOpen_ || payload.empty() || payload.size() > MaximumPayload ||
			room->serverLocalNextId_ == (std::numeric_limits<std::int64_t>::max)()) return false;
		return room->Queue(room->clientMessages_, {1, room->serverLocalNextId_++, payload, ""});
	}
	void Close() override {
		if (room->epoch_==epoch) {
			room->serverOpen_=false;
			if (!room->coordination_.active) room->Leave();
		}
	}
};

class IrohRoom::ClientAdapter final : public ClientTransport {
	std::shared_ptr<IrohRoom> room;
	std::uint64_t epoch;
	bool closed = false;
public:
	explicit ClientAdapter(std::shared_ptr<IrohRoom> owner) : room(std::move(owner)), epoch(room->epoch_) {}
	~ClientAdapter() override { Close(); }
	ConnectionState State() const override {
		if (closed || room->epoch_ != epoch || room->state_ == IrohRoom::State::Idle || room->state_ == IrohRoom::State::Closing) return ConnectionState::Closed;
		if (room->state_ == IrohRoom::State::Failed) return ConnectionState::Failed;
		return room->state_ == IrohRoom::State::Ready || room->state_ == IrohRoom::State::Degraded ? ConnectionState::Connected : ConnectionState::Connecting;
	}
	std::string PeerAddress() const override { return {}; }
	bool Poll(std::vector<Message>& messages, std::size_t maximum) override {
		room->Poll();
		if (State() == ConnectionState::Failed || State() == ConnectionState::Closed) return false;
        room->PumpCommittedEffects();
		while (messages.size() < maximum && !room->clientMessages_.empty()) {
            const auto bytes=room->clientMessages_.front().payload.size();
            const int allowed=room->AuthorizedEffect(room->clientMessages_.front());
            if(allowed==0) break;
            if(allowed<0) {
                room->queuedBytes_-=bytes;
                room->clientMessages_.pop_front();
                continue;
            }
            room->queuedBytes_-=bytes;
            if(allowed>0) {
                try {
                    const auto decoded=json::parse(room->clientMessages_.front().payload);
                    const auto type=decoded.value("type",std::string());
                    // A bare hello_resp is only the CID half of an admission
                    // exchange; rejected custom admissions send join_rej
                    // immediately afterward. Keep the pending fence through
                    // that response. A committed hello_resp is terminal and
                    // carries its authenticated membership in the commit
                    // envelope, while join_rej is always terminal.
                    if(type=="join_rej" || (type=="hello_resp" && decoded.contains("_commit")))
                        room->pendingAdmission_=false;
                } catch(const json::exception&) {}
                messages.push_back(std::move(room->clientMessages_.front()));
            }
            room->clientMessages_.pop_front();
		}
		return true;
	}
	SendResult Send(const std::string& payload, bool reliable, std::int64_t* id) override {
		if (State() != ConnectionState::Connected || room->state_ == IrohRoom::State::Degraded) return SendResult::NotConnected;
		// Gameplay belongs to the separately authorized raw UDP bridge. Never
		// put legacy GGPO frame payloads on the reliable room connection.
		if (!reliable || payload.empty() || payload.size() > MaximumPayload) return SendResult::InvalidPayload;
		try {
			const auto decoded=json::parse(payload);
			if(decoded.value("type",std::string())=="hello") room->pendingAdmission_=true;
		} catch(const json::exception&) {}
		if (!room->hosting_) {
			if (room->peers_.empty()) return SendResult::NotConnected;
			const auto owner=room->coordination_.active ? room->ConnectionForIdentity(room->coordination_.leader) : room->peers_.begin()->first;
			return owner ? room->SendRemote(owner,payload,id) : SendResult::NotConnected;
		}
		if (room->localNextId_ == (std::numeric_limits<std::int64_t>::max)()) return SendResult::Failed;
		const auto messageId = room->localNextId_++;
		if (!room->Queue(room->serverMessages_, {1, messageId, payload, ""})) return SendResult::QueueFull;
		if (id) *id = messageId;
		return SendResult::Queued;
	}
	void Close() override { if (!closed && room->epoch_ == epoch) room->Leave(); closed = true; }
};

std::unique_ptr<ServerTransport> IrohRoom::Server() {
	if ((!hosting_ && !coordination_.active) || state_ == State::Idle || serverOpen_) return {};
	serverOpen_ = true;
	return std::unique_ptr<ServerTransport>(new ServerAdapter(shared_from_this()));
}
std::unique_ptr<ClientTransport> IrohRoom::Client() {
	if (localOpen_ || state_ == State::Idle) return {};
	localOpen_ = true;
	return std::unique_ptr<ClientTransport>(new ClientAdapter(shared_from_this()));
}

} }
