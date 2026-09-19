#pragma once

// One scriptable ClientTransport for tests that drive SessionClient or
// IrohMatchSession with no helper process and no socket: an injectable inbox,
// a record of what the session sent, and a connection state the test owns.
// Self-contained on purpose, so it does not depend on the includer's CHECK
// macro the way the test bodies around it do.
#include "../session/sf4e__SessionClient.hxx"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace sf4e { namespace test {

class MockClient final : public session::ClientTransport {
public:
	session::ConnectionState state = session::ConnectionState::Connecting;
	std::vector<session::Message> incoming;
	std::vector<nlohmann::json> sent;
	std::int64_t next = 700;
	bool writable = true;
	bool Poll(std::vector<session::Message>& messages, std::size_t maximum) override {
		if (incoming.size() > maximum) { std::cerr << "MockClient inbox exceeds the poll limit\n"; std::exit(1); }
		messages.swap(incoming);
		return true;
	}
	session::ConnectionState State() const override { return state; }
	std::string PeerAddress() const override { return "127.0.0.1"; }
	session::SendResult Send(const std::string& payload, bool reliable, std::int64_t* id) override {
		if (!reliable) { std::cerr << "MockClient only carries reliable messages\n"; std::exit(1); }
		if (!writable) return session::SendResult::QueueFull;
		sent.push_back(nlohmann::json::parse(payload));
		if (id) *id = next;
		++next;
		return session::SendResult::Queued;
	}
	void Close() override { state = session::ConnectionState::Closed; }
	void Push(const nlohmann::json& message) { incoming.push_back({1, 2, message.dump(), ""}); }
};

}}
