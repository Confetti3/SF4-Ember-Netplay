#pragma once
// The field readers the helper-answer decoders share (TournamentAnswers.cxx,
// LevelsAnswers.cxx, ArchiveAnswers.cxx). Each reads one member of a JSON
// object; one that is missing, of the wrong type or out of bounds is an
// exception the decoder turns into "malformed", unless the reader says
// otherwise.
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace sf4e { namespace session { namespace reading {
using nlohmann::json;

// A bounded string field, or an exception the decoder turns into "malformed".
inline std::string Text(const json& object, const char* key, std::size_t maximum) {
	const auto& value = object.at(key);
	if (!value.is_string()) throw std::invalid_argument(key);
	auto text = value.get<std::string>();
	if (text.size() > maximum) throw std::invalid_argument(key);
	return text;
}

inline std::uint64_t Seconds(const json& object, const char* key) {
	const auto& value = object.at(key);
	if (!value.is_number_unsigned()) throw std::invalid_argument(key);
	return value.get<std::uint64_t>();
}

// A time the bridge may or may not send: absent or not an unsigned number is 0
// (unknown), so a newer or older bridge never costs the player a row.
inline std::uint64_t OptionalSeconds(const json& object, const char* key) {
	const auto found = object.find(key);
	return found != object.end() && found->is_number_unsigned() ? found->get<std::uint64_t>() : 0;
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

// A room name is another player's text: kept to one line.
inline std::string SingleLine(std::string text) {
	for (auto& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
	return text;
}

} } }
