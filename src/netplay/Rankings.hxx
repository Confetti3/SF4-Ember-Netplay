#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../common/Country.hxx"
#include "../common/SkillBadge.hxx"

// What the interface sees of rankings (docs/design/SKILL_BADGE.md, "Rankings"):
// the player's own standing and the board of classes, from the bridge's last
// answers. The helper answers by request id; the runtime decodes each answer
// completely into these types, or reports it as invalid.
namespace sf4e { namespace netplay { namespace rankings {

constexpr std::size_t MaxBoard = 500;
constexpr std::size_t MaxNameBytes = 32;

// The bridge's rules for a listing's name (Rust's char::is_whitespace and
// char::is_control over UTF-8): the byte length of the character at `at`, and
// whether it is whitespace or a control character.
inline std::size_t CharacterLength(const std::string& text, std::size_t at) {
	const auto lead = static_cast<unsigned char>(text[at]);
	const std::size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	return at + length <= text.size() ? length : 1;
}
inline bool WhitespaceAt(const std::string& text, std::size_t at, std::size_t length) {
	const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[at + k]); };
	switch (length) {
	case 1: return byte(0) == 0x20 || (byte(0) >= 0x09 && byte(0) <= 0x0D);
	case 2: return byte(0) == 0xC2 && (byte(1) == 0x85 || byte(1) == 0xA0);
	case 3:
		if (byte(0) == 0xE1) return byte(1) == 0x9A && byte(2) == 0x80;
		if (byte(0) == 0xE3) return byte(1) == 0x80 && byte(2) == 0x80;
		if (byte(0) == 0xE2 && byte(1) == 0x80) return byte(2) <= 0x8A || byte(2) == 0xA8 || byte(2) == 0xA9 || byte(2) == 0xAF;
		return byte(0) == 0xE2 && byte(1) == 0x81 && byte(2) == 0x9F;
	default: return false;
	}
}
inline bool ControlAt(const std::string& text, std::size_t at, std::size_t length) {
	const auto lead = static_cast<unsigned char>(text[at]);
	if (length == 1) return lead < 0x20 || lead == 0x7F;
	return length == 2 && lead == 0xC2 && static_cast<unsigned char>(text[at + 1]) <= 0x9F;
}

// The name a listing carries for a profile's display name: trimmed of
// whitespace as the bridge trims, or "Player" when nothing is left or it holds
// a control character, which the bridge refuses.
inline std::string ListingName(const std::string& profile) {
	std::size_t first = std::string::npos, end = 0;
	for (std::size_t at = 0; at < profile.size();) {
		const auto length = CharacterLength(profile, at);
		if (ControlAt(profile, at, length) && !WhitespaceAt(profile, at, length)) return "Player";
		if (!WhitespaceAt(profile, at, length)) {
			if (first == std::string::npos) first = at;
			end = at + length;
		}
		at += length;
	}
	if (first == std::string::npos) return "Player";
	const auto name = profile.substr(first, end - first);
	for (std::size_t at = 0; at < name.size(); at += CharacterLength(name, at))
		if (ControlAt(name, at, CharacterLength(name, at))) return "Player";
	return name;
}

// A name the bridge accepts on the board: 1 to MaxNameBytes bytes, no control
// characters and no whitespace at either end.
inline bool IsListingName(const std::string& name) {
	return !name.empty() && name.size() <= MaxNameBytes && ListingName(name) == name;
}

// How the board lists a player: the name and flag their game sent. A country
// the game's table lacks shows no flag.
struct Listing {
	std::string name;
	CountryCode country;
	bool operator==(const Listing& other) const { return name == other.name && country == other.country; }
	bool operator!=(const Listing& other) const { return !(*this == other); }
};

// The player's own place in ranking.
struct Standing {
	enum class Kind : std::uint8_t {
		// No Discord account is connected to the Ember ID; a rating belongs to one.
		NoAccount,
		// The player chose not to take part.
		OptedOut,
		Ranked,
	} kind = Kind::NoAccount;
	// Ranked: whether the bridge shows classes at all, and the class its next
	// room ticket carries (band 0 while placing, unsettled when faint).
	bool classesShown = false;
	SkillBadge badge;
	// Ranked: how the board lists the player, when it does.
	bool listed = false;
	Listing listing;
	// Seconds until taking part or not may change again; 0 when it may now.
	std::uint64_t changeIn = 0;
};

struct BoardEntry {
	Listing listing;
	// 1 to SkillBadge::Classes, highest first on the board.
	std::uint8_t band = 0;
	bool own = false;
};

// One kind of request's last answer: whether one is in flight, the helper's
// code when the last failed, how many have finished, answered or not, and how
// many values were installed. A failed request moves `finished` but not
// `revision`, so a reader can tell a newer value from a newer failure. Both
// counts carry on across a change of service or Ember ID (RankingRequests.hxx).
template <class Value>
struct Answered {
	Value value;
	bool loaded = false;
	bool loading = false;
	std::string error;
	std::uint64_t finished = 0, revision = 0;
	// The runtime's clock, milliseconds, when the value arrived.
	std::uint64_t installedMs = 0;
};

struct Status {
	// The service and the Ember ID the answers are for: answers belong to an
	// account on one service, so a change of either forgets them.
	std::string bridge, emberId;
	// A choice's answer is a standing too, and replaces this one.
	Answered<Standing> standing;
	// Shared and never changed once decoded, so copying the status for each
	// frame does not copy up to MaxBoard entries.
	Answered<std::shared_ptr<const std::vector<BoardEntry>>> board;
	// The identity (Command::request) of the choice in flight, 0 when none,
	// and of the last one answered, with the helper's code when it was refused.
	std::uint64_t choosing = 0, chose = 0;
	std::string choiceFailure;
	// The runtime's clock when this status was published, so a value's age
	// counts from its arrival however late the interface shows it.
	std::uint64_t nowMs = 0;
	template <class Value>
	double AgeSeconds(const Answered<Value>& answered) const {
		return nowMs > answered.installedMs ? static_cast<double>(nowMs - answered.installedMs) / 1000 : 0;
	}
};

} } }
