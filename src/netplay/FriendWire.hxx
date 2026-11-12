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
#include <type_traits>
#include <utility>
#include <variant>

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

namespace wire {
using nlohmann::json;
inline json Act(const char* action) {
	json act = json::object();
	act["action"] = action;
	return act;
}
// The `act` of each action: one overload per type, so a new action is one more.
inline json ActOf(const commands::FriendRequest& c) { auto act = Act("request"); act["card"] = c.card; act["label"] = c.label; return act; }
inline json ActOf(const commands::FriendRequestByCode& c) { auto act = Act("request"); act["code"] = c.code; return act; }
inline json ActOf(const commands::FriendAccept& c) { auto act = Act("accept"); act["request"] = c.request; return act; }
inline json ActOf(const commands::FriendDecline& c) {
	auto act = Act("decline"); act["request"] = c.request;
	if (c.block) act["block"] = true;
	return act;
}
inline json ActOf(const commands::FriendCancel& c) { auto act = Act("cancel"); act["request"] = c.request; return act; }
inline json ActOf(const commands::FriendRemove& c) {
	auto act = Act("remove"); act["friend"] = c.friendId;
	if (c.block) act["block"] = true;
	return act;
}
inline json ActOf(const commands::FriendBlock& c) { auto act = Act("block"); act["card"] = c.card; act["label"] = c.label; return act; }
inline json ActOf(const commands::FriendUnblock& c) { auto act = Act("unblock"); act["block"] = c.blockId; return act; }
inline json ActOf(const commands::FriendNewCode&) { return Act("new_code"); }
inline json ActOf(const commands::FriendAppearOffline& c) { auto act = Act("appear_offline"); act["on"] = c.on; return act; }
inline json ActOf(const commands::FriendTurnOff&) { return Act("forget"); }
inline json ActOf(const commands::FriendTurnOn&) { return Act("turn_on"); }
inline json ActOf(const commands::FriendDismiss& c) { auto act = Act("dismiss"); act["invite"] = c.invite; return act; }
// An invite names the room offered; without one it is not sent.
inline json ActOf(const commands::FriendInvite& c, const Offer& offer) {
	if (!offer.Any()) return nullptr;
	auto act = Act("invite"); act["friend"] = c.friendId;
	act["room"] = json::object();
	act["room"][offer.kind == Offer::Kind::PublicRoom ? "room_id" : "secret"] = offer.value;
	return act;
}
}

// The request for a bridge action, or null when it cannot be sent (an Invite
// with no room to offer).
inline nlohmann::json ActionRequest(const commands::FriendAction& action, const Offer& offer) {
	using nlohmann::json;
	const json act = std::visit([&](const auto& command) -> json {
		if constexpr (std::is_same_v<std::decay_t<decltype(command)>, commands::FriendInvite>) return wire::ActOf(command, offer);
		else return wire::ActOf(command);
	}, action);
	if (act.is_null()) return nullptr;
	json request = json::object();
	request["op"] = "friends_act";
	request["bridge_id"] = commands::BridgeOf(action);
	request["act"] = act;
	return request;
}

} } }
