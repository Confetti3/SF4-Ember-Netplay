#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "../common/LatencyRoute.hxx"

namespace sf4e {
namespace session {

using Connection = std::uint64_t;

enum class SendResult { Queued, NotConnected, InvalidPayload, QueueFull, Failed };
enum class ConnectionState { Connecting, Connected, Closed, Failed };

struct EffectEnvelope; // SessionRecovery.hxx

// What the edge that queued a room message read from its one parse, so the
// room queues and the session layer read this instead of parsing again. A
// transport that does not classify leaves `classified` false, and every
// reader then parses the payload itself (ClassifyRoomMessage, RoomMessageTag.hxx).
struct MessageTag {
	bool classified = false;
	// The payload's "type" when it is a string; empty otherwise, and when the
	// payload is not a JSON object.
	std::string type;
	bool committed = false;      // a top-level "_commit" token
	bool terminalReplay = false; // a top-level "_terminal_replay" marker
	bool verification = false;   // battle_hash or battle_snapshot
	// The payload reads as a room_action message: its action's kind (the
	// room::ActionKind value) and protocol version.
	bool roomAction = false;
	std::uint8_t actionKind = 0;
	std::uint32_t protocolVersion = 0;
};

// Transport-owned IDs are opaque to the lobby. A transport must not reuse an
// ID while messages or close events for its previous owner remain queued.
struct Message {
	Connection connection;
	std::int64_t messageId;
	std::string payload;
	std::string peerAddress; // Legacy GGPO addressing only; empty for local peers.
	MessageTag tag;
	// A committed effect the room's effect gate has parked: its token as
	// decoded on first sight, when its digest was checked against these same
	// payload bytes. Later polls only re-test the activation gate.
	std::shared_ptr<const EffectEnvelope> checkedCommit;
	// The payload already parsed, attached by a transport only as it hands
	// the message over, never while the message is queued. A reader may take
	// it instead of parsing `payload`; it equals json::parse(payload).
	std::shared_ptr<nlohmann::json> body;
};

// Called only by the session owner thread. Poll and Send must never wait for
// network progress. Implementations bound their queues and report saturation.
class ServerTransport {
public:
	using BatchPredicate = std::function<bool(const Message&)>;
	virtual ~ServerTransport() = default;
	virtual bool Listen(std::uint16_t port) = 0;
	virtual bool Attach(Connection connection) = 0;
	virtual bool Poll(std::vector<Message>& messages,
		std::vector<Connection>& closed, std::size_t maximum) = 0;
	// Recovery normally consumes one intent per private candidate. Transports
	// which can preserve their queue head may additionally return one ordered
	// prefix of a narrowly validated idempotent message kind. The default keeps
	// ordinary transports on the one-intent behavior.
	virtual bool PollRecoveryPrefix(std::vector<Message>& messages,
		std::vector<Connection>& closed, std::size_t maximum, const BatchPredicate&) {
		return Poll(messages, closed, 1);
	}
	virtual bool Send(Connection connection, const std::string& payload) = 0;
	virtual void Close() = 0;
};


class ClientTransport {
public:
	virtual ~ClientTransport() = default;
	virtual bool Poll(std::vector<Message>& messages, std::size_t maximum) = 0;
	virtual ConnectionState State() const = 0;
	virtual SendResult Send(const std::string& payload, bool reliable,
		std::int64_t* messageId) = 0;
	virtual std::string PeerAddress() const = 0;
	// This member's latency route for its join request; none by default.
	virtual LatencyRoute LocalLatency() const { return {}; }
	// Called just before the client sends its hello, whether or not that send
	// succeeds: an admission exchange may answer from now on. A wrapping
	// transport forwards it. Nothing to do by default.
	virtual void BeginAdmission() {}
	virtual void Close() = 0;
};


} // namespace session
} // namespace sf4e
