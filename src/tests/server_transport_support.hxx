#pragma once
// A server transport the session server tests drive by hand: pushed messages
// arrive on the next Step, and everything sent is kept in order.
#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../session/SessionTransport.hxx"
#include "test_support.hxx"

class MockTransport final : public sf4e::session::ServerTransport {
public:
	std::vector<sf4e::session::Message> incoming;
	std::vector<sf4e::session::Connection> disconnected;
	std::vector<std::pair<sf4e::session::Connection, nlohmann::json>> outgoing;
	bool writable = true;
	bool closed = false;
	bool Listen(std::uint16_t) override { return true; }
	bool Attach(sf4e::session::Connection) override { return true; }
	bool Poll(std::vector<sf4e::session::Message>& messages,
		std::vector<sf4e::session::Connection>& departed, std::size_t maximum) override {
		CHECK(maximum > 0);
		const auto count = (std::min)(incoming.size(), maximum);
		messages.insert(messages.end(), std::make_move_iterator(incoming.begin()),
			std::make_move_iterator(incoming.begin() + count));
		incoming.erase(incoming.begin(), incoming.begin() + count);
		departed.swap(disconnected);
		return !closed;
	}
	bool PollRecoveryPrefix(std::vector<sf4e::session::Message>& messages,
		std::vector<sf4e::session::Connection>& departed, std::size_t maximum,
		const BatchPredicate& batchable) override {
		CHECK(maximum > 0);
		std::size_t count = incoming.empty() ? 0 : 1;
		if (count && batchable(incoming.front()))
			while (count < incoming.size() && count < maximum && batchable(incoming[count])) ++count;
		messages.insert(messages.end(), std::make_move_iterator(incoming.begin()),
			std::make_move_iterator(incoming.begin() + count));
		incoming.erase(incoming.begin(), incoming.begin() + count);
		departed.swap(disconnected);
		return !closed;
	}
	bool Send(sf4e::session::Connection connection, const std::string& payload) override {
		if (!writable) return false;
		outgoing.emplace_back(connection, nlohmann::json::parse(payload));
		return true;
	}
	void Close() override { closed = true; }
	void Push(sf4e::session::Connection connection, nlohmann::json payload, std::int64_t id = 2) {
		incoming.push_back({connection, id, payload.dump(), ""});
	}
	bool Contains(const char* type) const {
		for (const auto& sent : outgoing) if (sent.second.at("type") == type) return true;
		return false;
	}
};

