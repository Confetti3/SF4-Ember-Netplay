// IrohRoom: the host side of a server-owned (public) room. The room host
// process opens the room under the id the bridge chose, learns each member's
// Ember ID from the helper's ticket check, and bans by that ID.
#include "IrohRoom.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace sf4e { namespace session {
using nlohmann::json;

bool IrohRoom::HostPublic(const std::string& build, const std::string& ticketKey, const std::string& ticketKid,
	const std::string& bridgeId, const std::array<std::uint8_t, 16>& roomId, const std::string& creator) {
	if (build.empty() || build.size() > 128 || ticketKey.empty() || ticketKid.empty() || bridgeId.empty() || creator.empty() ||
		std::all_of(roomId.begin(), roomId.end(), [](std::uint8_t byte) { return byte == 0; })) return false;
	if (!Begin(true)) return false;
	// The tickets already name this id, so the helper is told rather than asked.
	room_ = roomId;
	publicHost_ = true;
	roomCommandQueued_ = Command(json{{"type", "host_public"}, {"epoch", epoch_}, {"build", build}, {"room", roomId},
		{"ticket_key", ticketKey}, {"ticket_kid", ticketKid}, {"bridge_id", bridgeId}, {"creator", creator}}.dump());
	return roomCommandQueued_;
}

std::string IrohRoom::PeerAccount(Connection connection) const {
	const auto peer = peers_.find(connection);
	return peer == peers_.end() ? std::string() : peer->second.account;
}

bool IrohRoom::BanAccount(const std::string& account) {
	if (!hosting_ || account.empty() || (state_ != State::Ready && state_ != State::Degraded)) return false;
	// Not Command(): a full helper queue delays the ban, it does not fail the room.
	return helper_.Send(json{{"type", "ban_account"}, {"epoch", epoch_}, {"account", account}}.dump());
}

} }
