#pragma once
// The helper room and the native game messages that the Iroh match session
// tests and the spectator runtime tests share.
#include "../session/IrohRoom.hxx"
#include "../session/MatchAuthority.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "../platform/HelperClient.hxx"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sf4e { namespace test {


// IrohMatchSession only calls the nine IrohRoom methods overridden below.
// Recording their calls lets a test assert on dial behaviour without a real
// helper process; every other IrohRoom method is left untouched.
class FakeIrohRoom final : public session::IrohRoom {
public:
	explicit FakeIrohRoom(platform::HelperClient& helper) : IrohRoom(helper) {}
	std::array<std::uint8_t, 16> roomId{};
	std::string localIdentity;
	bool ready = true;
	struct PrepareCall { std::string peer; std::uint64_t generation; std::uint16_t localPort; std::size_t maxPacket; bool dial; };
	std::vector<PrepareCall> prepareCalls;
	std::vector<std::uint64_t> endMatchCalls;
	int leaveCalls = 0;

	bool ReadyForMatch() const override { return ready; }
	std::array<std::uint8_t, 16> RoomId() const override { return roomId; }
	const std::string& LocalIdentity() const override { return localIdentity; }
	const CoordinationSnapshot& Coordination() const override { return coordination_; }
	const GameSnapshot* FindGame(const std::string& peer) const override {
		const auto found = snapshots_.find(peer);
		return found == snapshots_.end() ? nullptr : &found->second;
	}
	bool PrepareGame(const std::string& peer, std::uint64_t generation, const std::array<std::uint8_t, 32>&,
		std::uint16_t localPort, std::size_t maxPacket, bool dial) override {
		prepareCalls.push_back({peer, generation, localPort, maxPacket, dial});
		GameSnapshot snapshot; snapshot.state = GameState::Preparing; snapshot.generation = generation; snapshot.maxPacket = maxPacket;
		snapshots_[peer] = snapshot;
		return true;
	}
	// The peers whose link EndPeer or TryEndPeer ended. refuseEndPeer refuses
	// both. busyEndPeer is how many calls find the helper's queue full: a
	// TryEndPeer is then Busy, and an EndPeer fails the room, as the real one
	// does, which failedOnFullQueue records.
	std::vector<std::string> endPeerCalls;
	bool refuseEndPeer = false, failedOnFullQueue = false;
	int busyEndPeer = 0;
	bool EndPeer(const std::string& peer, std::uint64_t) override {
		if (refuseEndPeer) return false;
		if (busyEndPeer > 0) { failedOnFullQueue = true; return false; }
		endPeerCalls.push_back(peer); snapshots_.erase(peer); return true;
	}
	PeerEnd TryEndPeer(const std::string& peer, std::uint64_t generation) override {
		if (busyEndPeer > 0) { --busyEndPeer; return PeerEnd::Busy; }
		return EndPeer(peer, generation) ? PeerEnd::Ended : PeerEnd::Refused;
	}
	bool EndMatch(std::uint64_t generation) override { endMatchCalls.push_back(generation); return true; }
	void AbandonMatch(std::uint64_t generation) override {
		for (auto& snapshot : snapshots_) if (snapshot.second.generation == generation) snapshot.second.state = GameState::Closed;
	}
	void Leave(bool) override { ++leaveCalls; }
	// The game links of `generation` are up.
	void LinkUp(std::uint64_t generation) {
		for (auto& snapshot : snapshots_) if (snapshot.second.generation == generation) snapshot.second.state = GameState::Ready;
	}
	// The room's control is coordinated, so a leave waits for the session to retire.
	void Coordinate() { coordination_.active = true; }
private:
	CoordinationSnapshot coordination_;
	std::map<std::string, GameSnapshot> snapshots_;
};

// The identity of P1, whose link every grant below dials.
inline std::string P1Peer() { return std::string(64, 'A'); }
// game_prepare for `generation`: `roster` in slot order, this PC at `slot`.
inline nlohmann::json GameGrant(const FakeIrohRoom& room, std::uint64_t generation,
	const std::vector<SessionProtocol::ConnectionID>& roster, std::size_t slot) {
	std::array<std::uint8_t, 32> capability{}; capability[0] = 7;
	const nlohmann::json link = {{"slot", 0}, {"peer", P1Peer()}, {"capability", capability}, {"dial", true}};
	return nlohmann::json{{"type", "game_prepare"}, {"generation", generation}, {"version", 1}, {"room", room.roomId},
		{"local_identity", room.localIdentity}, {"max_packet", session::GgpoMaximumPacket},
		{"roster", roster}, {"slot", slot}, {"links", nlohmann::json::array({link})}};
}
inline nlohmann::json GameConnect(std::uint64_t generation) { return nlohmann::json{{"type", "game_connect"}, {"generation", generation}}; }
inline nlohmann::json GameEnd(std::uint64_t generation) { return nlohmann::json{{"type", "game_end"}, {"generation", generation}}; }

} }
