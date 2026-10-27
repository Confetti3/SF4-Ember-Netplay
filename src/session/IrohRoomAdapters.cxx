// IrohRoom: the ServerTransport and ClientTransport views handed to the session layer.
#include "IrohRoom.hxx"
#include "RoomMessageQueue.hxx"
#include "RoomMessageTag.hxx"
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
	// Takes the server's bound messages, at most `maximum`. With `batchable`
	// (PollRecoveryPrefix) it stops at the end of the head's batch: the head
	// alone unless it is batchable, then the batchable messages behind it.
	bool Drain(std::vector<Message>& messages, std::vector<Connection>& closed,
		std::size_t maximum, const BatchPredicate* batchable) {
		room->Poll();
		if (room->epoch_ != epoch || room->room_.state == State::Failed || room->room_.state == State::Idle || room->room_.state == State::Closing) return false;
		if (room->room_.coordination.active && (!room->room_.coordination.writable || !room->room_.coordination.leaderLocal || !room->room_.coordination.rebound)) return true;
		closed.swap(room->room_.closed);
		const auto queued = room->room_.serverMessages.size();
		bool prefixBatchable = false;
		for (std::size_t examined = 0; examined < queued && messages.size() < maximum && !room->room_.serverMessages.empty(); ++examined) {
			if (room->room_.coordination.active && !room->PeerIncarnation(room->room_.serverMessages.front().connection)) {
				// Preserve the batch boundary once an authenticated prefix has begun;
				// the deferred binding remains ahead of every later intent.
				if (batchable && !messages.empty()) break;
				auto deferred = std::move(room->room_.serverMessages.front()); room->room_.serverMessages.pop_front();
				room->room_.serverMessages.push_back(std::move(deferred)); continue;
			}
			if (batchable) {
				if (!messages.empty() && (!prefixBatchable || !(*batchable)(room->room_.serverMessages.front()))) break;
				if (messages.empty()) prefixBatchable = (*batchable)(room->room_.serverMessages.front());
			}
			room->room_.queuedBytes -= room->room_.serverMessages.front().payload.size();
			messages.push_back(std::move(room->room_.serverMessages.front())); room->room_.serverMessages.pop_front();
			if (batchable && !prefixBatchable) break;
		}
		return true;
	}
public:
	explicit ServerAdapter(std::shared_ptr<IrohRoom> owner) : room(std::move(owner)), epoch(room->epoch_) {}
	~ServerAdapter() override { Close(); }
	bool Listen(std::uint16_t) override { return (room->room_.hosting || room->room_.coordination.active) && room->epoch_ == epoch; }
	bool Attach(Connection) override { return false; }
	bool Poll(std::vector<Message>& messages, std::vector<Connection>& closed, std::size_t maximum) override {
		return Drain(messages, closed, maximum, nullptr);
	}
	bool PollRecoveryPrefix(std::vector<Message>& messages, std::vector<Connection>& closed,
		std::size_t maximum, const BatchPredicate& batchable) override {
		return Drain(messages, closed, maximum, &batchable);
	}
	bool Send(Connection connection, const std::string& payload) override {
		if (room->epoch_ != epoch || room->room_.state != State::Ready) return false;
		if (connection != 1) {
			// A member whose control has closed, or who announced its departure,
			// gets nothing more from this session: the helper may already hold
			// that endpoint's next session, which would reject the numbering.
			// Committed effects reach a returning member through the journal,
			// and the server treats a refused send as transport loss.
			if (room->PeerControlClosed(connection)) return true;
			return room->SendRemote(connection, payload, nullptr) == SendResult::Queued;
		}
		if (!room->room_.localOpen || payload.empty() || payload.size() > MaximumPayload ||
			room->room_.serverLocalNextId == (std::numeric_limits<std::int64_t>::max)()) return false;
		// Classified once here, for the queue and the local client.
		return room->Queue(room->room_.clientMessages, {1, room->room_.serverLocalNextId++, payload, "", ClassifyRoomMessage(payload)});
	}
	void Close() override {
		if (room->epoch_==epoch) {
			room->room_.serverOpen=false;
			if (!room->room_.coordination.active) room->Leave();
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
		if (closed || room->epoch_ != epoch || room->room_.state == IrohRoom::State::Idle || room->room_.state == IrohRoom::State::Closing) return ConnectionState::Closed;
		if (room->room_.state == IrohRoom::State::Failed) return ConnectionState::Failed;
		return room->room_.state == IrohRoom::State::Ready || room->room_.state == IrohRoom::State::Degraded ? ConnectionState::Connected : ConnectionState::Connecting;
	}
	std::string PeerAddress() const override { return {}; }
	LatencyRoute LocalLatency() const override {
		return closed || room->epoch_ != epoch ? LatencyRoute{} : room->room_.localLatency;
	}
	bool Poll(std::vector<Message>& messages, std::size_t maximum) override {
		room->Poll();
		if (State() == ConnectionState::Failed || State() == ConnectionState::Closed) return false;
        room->PumpCommittedEffects();
		while (messages.size() < maximum && !room->room_.clientMessages.empty()) {
            const auto bytes=room->room_.clientMessages.front().payload.size();
            const auto verdict=room->AuthorizedEffect(room->room_.clientMessages.front());
            if(verdict==EffectVerdict::Park) break;
            if(verdict==EffectVerdict::Drop) {
                room->room_.queuedBytes-=bytes;
                room->room_.clientMessages.pop_front();
                continue;
            }
            room->room_.queuedBytes-=bytes;
            // A bare hello_resp is only the CID half of an admission
            // exchange; rejected custom admissions send join_rej
            // immediately afterward. Keep the pending fence through
            // that response. A committed hello_resp is terminal and
            // carries its authenticated membership in the commit
            // envelope, while join_rej is always terminal. Read from the
            // payload as it is delivered: the body AuthorizedEffect
            // parsed, else the tag of the unchanged payload.
            auto& head=room->room_.clientMessages.front();
            std::string type;
            bool committed=false;
            if(head.body) {
                if(head.body->is_object()) {
                    const auto field=head.body->find("type");
                    if(field!=head.body->end() && field->is_string()) type=field->get<std::string>();
                    committed=head.body->contains("_commit");
                }
            } else {
                const auto tag=TagOf(head);
                type=tag.type; committed=tag.committed;
            }
            if(type=="join_rej" || (type=="hello_resp" && committed))
                room->room_.pendingAdmission=false;
            messages.push_back(std::move(head));
            room->room_.clientMessages.pop_front();
		}
		return true;
	}
	SendResult Send(const std::string& payload, bool reliable, std::int64_t* id) override {
		if (State() != ConnectionState::Connected || room->room_.state == IrohRoom::State::Degraded) return SendResult::NotConnected;
		// Gameplay belongs to the separately authorized raw UDP bridge. Never
		// put legacy GGPO frame payloads on the reliable room connection.
		if (!reliable || payload.empty() || payload.size() > MaximumPayload) return SendResult::InvalidPayload;
		if (!room->room_.hosting) {
			if (room->room_.peers.empty()) return SendResult::NotConnected;
			const auto owner=room->room_.coordination.active ? room->ConnectionForIdentity(room->room_.coordination.leader) : room->room_.peers.begin()->first;
			return owner ? room->SendRemote(owner,payload,id) : SendResult::NotConnected;
		}
		if (room->room_.localNextId == (std::numeric_limits<std::int64_t>::max)()) return SendResult::Failed;
		const auto messageId = room->room_.localNextId++;
		// Classified once here; the server queue and SessionServer read the tag.
		if (!room->Queue(room->room_.serverMessages, {1, messageId, payload, "", ClassifyRoomMessage(payload)})) return SendResult::QueueFull;
		if (id) *id = messageId;
		return SendResult::Queued;
	}
	// The admission fence opens before the hello is sent, as it did when the
	// hello was recognised inside Send: under the same conditions, ahead of the
	// hosting and peer checks, so a hello refused as NotConnected and retried
	// has already opened it.
	void BeginAdmission() override {
		if (State() == ConnectionState::Connected && room->room_.state != IrohRoom::State::Degraded) room->room_.pendingAdmission=true;
	}
	void Close() override { if (!closed && room->epoch_ == epoch) room->Leave(); closed = true; }
};

std::unique_ptr<ServerTransport> IrohRoom::Server() {
	if ((!room_.hosting && !room_.coordination.active) || room_.state == State::Idle || room_.serverOpen) return {};
	room_.serverOpen = true;
	return std::unique_ptr<ServerTransport>(new ServerAdapter(shared_from_this()));
}
std::unique_ptr<ClientTransport> IrohRoom::Client() {
	if (room_.localOpen || room_.state == State::Idle) return {};
	room_.localOpen = true;
	return std::unique_ptr<ClientTransport>(new ClientAdapter(shared_from_this()));
}

} }
