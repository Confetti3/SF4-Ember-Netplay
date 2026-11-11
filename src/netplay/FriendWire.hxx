#pragma once
// The helper requests for friends (docs/design/FRIENDS.md, "Wire contracts";
// the helper's `friends_sync` and `friends_act` ops, rust/sf4-net
// tournament/friends.rs): the player's presence in, and one action on the
// list. Pure builders, so the exact bytes are tested without a helper
// (friend_requests_test.cxx). The helper checks every request again and
// refuses unknown members, so a member added here only ever goes where the
// helper reads it.
#include "FriendCommands.hxx"
#include "FriendPresence.hxx"

#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay { namespace friends {

// What friends see of this player.
struct Listing {
	std::string name;
	// Two upper-case letters, or empty for no flag.
	std::string country;
	// The main fighter (0 to FighterCount - 1), or -1 for none.
	int main = -1;
};

inline nlohmann::json SyncRequest(const std::string& bridgeId, const std::string& buildId, const Listing& listing, const Report& report) {
	using nlohmann::json;
	json shown = json::object();
	shown["name"] = listing.name;
	if (!listing.country.empty()) shown["country"] = listing.country;
	json sync = json::object();
	sync["build_id"] = buildId;
	sync["listing"] = std::move(shown);
	if (listing.main >= 0) sync["main"] = listing.main;
	sync["presence"] = report.ToJson();
	json request = json::object();
	request["op"] = "friends_sync";
	request["bridge_id"] = bridgeId;
	request["sync"] = std::move(sync);
	return request;
}

namespace detail {
inline nlohmann::json Act(const char* action) {
	nlohmann::json act = nlohmann::json::object();
	act["action"] = action;
	return act;
}
}

// The request for a command that is one of the bridge's actions, or null for
// any other. An Invite names the room offered; without one it is not sent.
inline nlohmann::json ActionRequest(const Command& command, const Offer& offer) {
	using nlohmann::json;
	json act;
	switch (command.kind) {
	case Command::Kind::Request:
		act = detail::Act("request");
		if (command.ByCard()) { act["card"] = command.card; act["label"] = command.label; }
		else act["code"] = command.code;
		break;
	case Command::Kind::Accept: act = detail::Act("accept"); act["request"] = command.id; break;
	case Command::Kind::Decline:
		act = detail::Act("decline"); act["request"] = command.id;
		if (command.flag) act["block"] = true;
		break;
	case Command::Kind::Cancel: act = detail::Act("cancel"); act["request"] = command.id; break;
	case Command::Kind::Remove:
		act = detail::Act("remove"); act["friend"] = command.id;
		if (command.flag) act["block"] = true;
		break;
	case Command::Kind::Block: act = detail::Act("block"); act["card"] = command.card; act["label"] = command.label; break;
	case Command::Kind::Unblock: act = detail::Act("unblock"); act["block"] = command.id; break;
	case Command::Kind::NewCode: act = detail::Act("new_code"); break;
	case Command::Kind::AppearOffline: act = detail::Act("appear_offline"); act["on"] = command.flag; break;
	case Command::Kind::Forget: act = detail::Act("forget"); break;
	case Command::Kind::Dismiss: act = detail::Act("dismiss"); act["invite"] = command.id; break;
	case Command::Kind::Invite:
		if (!offer.Any()) return nullptr;
		act = detail::Act("invite"); act["friend"] = command.id;
		act["room"] = json::object();
		act["room"][offer.kind == Offer::Kind::PublicRoom ? "room_id" : "secret"] = offer.value;
		break;
	default:
		return nullptr;
	}
	json request = json::object();
	request["op"] = "friends_act";
	request["bridge_id"] = command.bridgeId;
	request["act"] = std::move(act);
	return request;
}

} } }
