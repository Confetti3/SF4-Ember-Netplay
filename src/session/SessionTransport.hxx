#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sf4e {
namespace session {

using Connection = std::uint64_t;

enum class SendResult { Queued, NotConnected, InvalidPayload, QueueFull, Failed };
enum class ConnectionState { Connecting, Connected, Closed, Failed };

// Transport-owned IDs are opaque to the lobby. A transport must not reuse an
// ID while messages or close events for its previous owner remain queued.
struct Message {
	Connection connection;
	std::int64_t messageId;
	std::string payload;
	std::string peerAddress; // Legacy GGPO addressing only; empty for local peers.
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
	virtual void Close() = 0;
};


} // namespace session
} // namespace sf4e
