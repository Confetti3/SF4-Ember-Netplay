#pragma once
// The field readers the helper-answer decoders share (TournamentAnswers.cxx,
// LevelsAnswers.cxx, ArchiveAnswers.cxx, SocialAnswers.cxx). Each reads one member of a JSON
// object; one that is missing, of the wrong type or out of bounds is an
// exception the decoder turns into "malformed", unless the reader says
// otherwise.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "../netplay/Levels.hxx"

namespace sf4e { namespace session { namespace reading {
using nlohmann::json;

// An object, for the answers that ignore members they do not know: a field a later bridge adds
// must not break a build already out, so only the keys read are checked.
inline const json& Object(const json& value) {
	if (!value.is_object()) throw std::invalid_argument("object");
	return value;
}

// A bounded string field, or an exception the decoder turns into "malformed".
inline std::string Text(const json& object, const char* key, std::size_t maximum) {
	const auto& value = object.at(key);
	if (!value.is_string()) throw std::invalid_argument(key);
	auto text = value.get<std::string>();
	if (text.size() > maximum) throw std::invalid_argument(key);
	return text;
}

// A bounded string field that holds something.
inline std::string NonEmptyText(const json& object, const char* key, std::size_t maximum) {
	auto text = Text(object, key, maximum);
	if (text.empty()) throw std::invalid_argument(key);
	return text;
}

// A counter or a time of any size, as the answers carry them.
inline std::uint64_t Count(const json& object, const char* key) {
	const auto& value = object.at(key);
	if (!value.is_number_unsigned()) throw std::invalid_argument(key);
	return value.get<std::uint64_t>();
}

// A count the bridge may leave out: absent is 0, and one that is there must be an unsigned number.
inline std::uint64_t OptionalCount(const json& object, const char* key) {
	return object.find(key) == object.end() ? 0 : Count(object, key);
}

// A count the bridge may or may not send, up to `maximum`: absent is 0 (a
// bridge from before it), one that is there must be an unsigned number, and
// one past `maximum` is `maximum`.
inline std::uint64_t CappedCount(const json& object, const char* key, std::uint64_t maximum) {
	return (std::min)(OptionalCount(object, key), maximum);
}

// A count or time the bridge may or may not send: absent or not an unsigned number is 0
// (unknown), so a newer or older bridge never costs the player a row.
inline std::uint64_t LenientCount(const json& object, const char* key) {
	const auto found = object.find(key);
	return found != object.end() && found->is_number_unsigned() ? found->get<std::uint64_t>() : 0;
}

// A finite number, or an exception the decoder turns into "malformed".
inline double Finite(const json& object, const char* key) {
	const auto& value = object.at(key);
	if (!value.is_number() || !std::isfinite(value.get<double>())) throw std::invalid_argument(key);
	return value.get<double>();
}

// A finite number the bridge may leave out: absent is none.
inline std::optional<double> OptionalFinite(const json& object, const char* key) {
	if (object.find(key) == object.end()) return std::nullopt;
	return Finite(object, key);
}

inline bool IsHex(const std::string& text, std::size_t length) {
	return text.size() == length && std::all_of(text.begin(), text.end(),
		[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A bounded unsigned number, or an exception the decoder turns into "malformed".
inline unsigned Small(const json& object, const char* key, std::uint64_t maximum) {
	const auto& value = object.at(key);
	if (!value.is_number_unsigned() || value.get<std::uint64_t>() > maximum) throw std::invalid_argument(key);
	return static_cast<unsigned>(value.get<std::uint64_t>());
}

// A frame tier as sent: an unsigned number, and one past the tiers this build draws (a later
// bridge's table) is the last it knows. `beyond` says it was past.
inline std::uint8_t Tier(const json& object, bool* beyond = nullptr) {
	const auto tier = Count(object, "tier");
	const auto last = static_cast<std::uint64_t>(netplay::levels::TierCount - 1);
	if (beyond) *beyond = tier > last;
	return static_cast<std::uint8_t>((std::min)(tier, last));
}

// A room name is another player's text: kept to one line.
inline std::string SingleLine(std::string text) {
	for (auto& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
	return text;
}

} } }
