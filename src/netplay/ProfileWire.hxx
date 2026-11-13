#pragma once
// The helper requests for profiles (docs/design/PROFILE.md, "Helper ops";
// rust/sf4-net tournament/levels.rs): saving the player's own card, finding a
// player, and looking a card up by one selector. Pure builders, so the exact
// bytes are tested without a helper (profiles_test.cxx). The helper checks every
// request again and refuses unknown members, and answers `profiles_unavailable`
// for a service that does not offer profiles, so nothing here asks the service.
#include "CardBundle.hxx"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay { namespace profiles {

// card_save: the bundle as PUT /v1/cards/me carries it. The listing and main
// are what friends sync sends; worn and pins are family ids, only those the
// bundle holds; the style always carries device, link and looking as small
// numbers (0: not shown) and the region only when there is one.
inline nlohmann::json CardSaveRequest(const std::string& bridgeId, const Bundle& bundle) {
	using nlohmann::json;
	json listing = json::object();
	listing["name"] = bundle.name;
	if (bundle.country.Present()) listing["country"] = bundle.country.ToString();
	json worn = json::object();
	if (!bundle.worn.title.empty()) worn["title"] = bundle.worn.title;
	if (!bundle.worn.icon.empty()) worn["icon"] = bundle.worn.icon;
	json style = json::object();
	style["device"] = static_cast<unsigned>(bundle.style.device);
	style["link"] = static_cast<unsigned>(bundle.style.link);
	if (!bundle.style.region.empty()) style["region"] = bundle.style.region;
	style["looking"] = static_cast<unsigned>(bundle.style.looking);
	json card = json::object();
	card["listing"] = std::move(listing);
	if (bundle.main >= 0) card["main"] = bundle.main;
	card["worn"] = std::move(worn);
	card["pins"] = bundle.pins;
	card["style"] = std::move(style);
	json request = json::object();
	request["op"] = "card_save";
	request["bridge_id"] = bridgeId;
	request["card"] = std::move(card);
	return request;
}

// cards_search: a name (2 to 32 bytes) or a friend code, one of the two.
inline nlohmann::json SearchRequest(const std::string& bridgeId, const std::string& text, bool byCode) {
	nlohmann::json request = nlohmann::json::object();
	request["op"] = "cards_search";
	request["bridge_id"] = bridgeId;
	request[byCode ? "code" : "name"] = text;
	return request;
}

// cards_lookup of one card by its selector: a card token, a friend id, a request
// received or a friend code. Kind::Name and Kind::None have no selector.
inline nlohmann::json LookupRequest(const std::string& bridgeId, Key::Kind selector, const std::string& value) {
	using nlohmann::json;
	const char* member = selector == Key::Kind::Card ? "card" : selector == Key::Kind::Friend ? "friend" :
		selector == Key::Kind::Request ? "request" : selector == Key::Kind::Code ? "code" : nullptr;
	if (!member) return nullptr;
	json one = json::object();
	one[member] = value;
	json request = json::object();
	request["op"] = "cards_lookup";
	request["bridge_id"] = bridgeId;
	request["cards"] = json::array({std::move(one)});
	return request;
}

} } }
