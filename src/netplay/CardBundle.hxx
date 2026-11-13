#pragma once
// What the bridge keeps of the player's own card, and when the runtime sends it
// (docs/design/PROFILE.md, "Runtime and view": card save). The bundle is made
// from the preferences and what the game measures; the saver says whether it
// goes now. Pure, so the rules are tested without a helper (profiles_test.cxx).
//
// - The bundle is sent when it differs from the last one the service saved for
//   this service and Ember ID: at start (nothing is saved yet) and after Edit
//   card saves (the preferences changed), and not otherwise.
// - At most one a minute, one in flight at a time. A refusal that will not
//   change soon (a service without profiles, no Discord account, the bridge's
//   rate limit) waits ten minutes before it is tried again; any other failure
//   tries again after the minute.
#include "Awards.hxx"
#include "PlayerPreferences.hxx"
#include "Profiles.hxx"
#include "Rankings.hxx"
#include "../common/Country.hxx"
#include "../common/NetworkLink.hxx"
#include "../common/NetworkRoute.hxx"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace netplay { namespace profiles {

// How the player plays as their card shows it, from their preferences and what
// the game measures: `automatic` is the device Automatic stands for
// (AutomaticDevice, none when no controller is known), `link` the measured
// connection, `region` the relay region Ember picked (empty or not a region
// code: none). The connection and region show only while the player lets them.
inline Style OwnStyle(const PlayerPreferences& preferences, Device automatic, NetworkLink link, const std::string& region) {
	Style style;
	style.device = preferences.cardDevice == 0 ? automatic : preferences.cardDevice >= 1 && preferences.cardDevice <= 4 ? static_cast<Device>(preferences.cardDevice) : Device::None;
	if (preferences.cardShowLink) style.link = link == NetworkLink::Wired ? Link::Wired : link == NetworkLink::Wireless ? Link::WiFi : Link::None;
	if (preferences.cardShowRegion && IsRelayRegionCode(region)) style.region = region;
	style.looking = LookingFromSaved(preferences.cardLooking);
	return style;
}

// What PUT /v1/cards/me carries: the listing the rankings and friends show,
// the main fighter, the award families worn and shown (those this build's
// catalog knows; a viewer's catalog works out each level from the card's
// counters), and how the player plays.
struct Bundle {
	std::string name;
	// None: the flag is hidden, or Windows names no country the table has.
	CountryCode country;
	// 0 to FighterCount - 1, or -1 for none.
	int main = -1;
	Worn worn;
	std::vector<std::string> pins;
	Style style;
	bool operator==(const Bundle& other) const {
		return name == other.name && country == other.country && main == other.main && worn == other.worn && pins == other.pins && style == other.style;
	}
	bool operator!=(const Bundle& other) const { return !(*this == other); }
};

inline Bundle BundleOf(const PlayerPreferences& preferences, const CountryCode& automaticCountry, const Style& style) {
	Bundle bundle;
	bundle.name = rankings::ListingName(preferences.displayName);
	bundle.country = ShownCountry(preferences.country, automaticCountry);
	bundle.main = preferences.mainFighter >= 0 && preferences.mainFighter < selection::FighterCount ? preferences.mainFighter : -1;
	if (awards::Find(preferences.wornTitle)) bundle.worn.title = preferences.wornTitle;
	if (awards::Find(preferences.wornIcon)) bundle.worn.icon = preferences.wornIcon;
	for (const auto& pin : preferences.cardPins) {
		if (bundle.pins.size() == MaxPins) break;
		if (awards::Find(pin) && std::find(bundle.pins.begin(), bundle.pins.end(), pin) == bundle.pins.end()) bundle.pins.push_back(pin);
	}
	bundle.style = style;
	return bundle;
}

// When the bundle goes to the service.
class Saver {
public:
	// The least time between sends, and the wait after a refusal that will not change soon.
	static constexpr std::uint64_t MinGapMs = 60000, RefusedGapMs = 600000;

	// Whether `bundle` is sent now to the service and Ember ID named: no while a
	// send is in flight or one is saved already, and within the gap of the last send.
	bool Due(const std::string& bridge, const std::string& emberId, const Bundle& bundle, std::uint64_t nowMs) const {
		if (bridge.empty() || emberId.empty() || flight_) return false;
		if (SameScope(bridge, emberId)) {
			if (saved_ && savedBundle_ == bundle) return false;
			if (sent_ && nowMs - sentMs_ < gapMs_) return false;
		}
		return true;
	}

	// `bundle` was handed to the helper.
	void Sent(const std::string& bridge, const std::string& emberId, const Bundle& bundle, std::uint64_t nowMs) {
		if (!SameScope(bridge, emberId)) { saved_ = false; sent_ = false; }
		bridge_ = bridge;
		emberId_ = emberId;
		bundle_ = bundle;
		sent_ = true;
		sentMs_ = nowMs;
		gapMs_ = MinGapMs;
		flight_ = true;
	}

	// The answer: the helper's code when the service refused or the request failed, empty when it saved.
	void Finish(const std::string& failure) {
		if (!flight_) return;
		flight_ = false;
		if (failure.empty()) {
			saved_ = true;
			savedBundle_ = bundle_;
			return;
		}
		if (failure == Unavailable || failure == "no_discord_account" || failure == "rate_limited") gapMs_ = RefusedGapMs;
	}

	// Another service or Ember ID, or no send in flight any more: forgets what was sent and saved.
	void Forget() {
		bridge_.clear();
		emberId_.clear();
		saved_ = sent_ = flight_ = false;
	}

	bool InFlight() const { return flight_; }

private:
	bool SameScope(const std::string& bridge, const std::string& emberId) const { return bridge == bridge_ && emberId == emberId_; }

	std::string bridge_, emberId_;
	Bundle bundle_, savedBundle_;
	bool saved_ = false, sent_ = false, flight_ = false;
	std::uint64_t sentMs_ = 0, gapMs_ = MinGapMs;
};

} } }
