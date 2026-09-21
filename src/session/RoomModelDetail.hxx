#pragma once

// Validation helpers shared by the room model translation units. Not part of the
// public room interface.

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>

#include "RoomModel.hxx"

namespace sf4e { namespace room { namespace detail {

constexpr std::uint64_t ResultDisputeTimeoutMs = 30000;

template <typename T>
bool InRange(T value, T low, T high) { return value >= low && value <= high; }

inline bool IsValidUtf8(const std::string& text) {
	const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
	std::size_t i = 0;
	while (i < text.size()) {
		const unsigned char c = bytes[i++];
		if (c == 0) return false;
		if (c < 0x80) continue;
		std::size_t count = 0;
		std::uint32_t code = 0;
		std::uint32_t minimum = 0;
		if ((c & 0xe0) == 0xc0) { count = 1; code = c & 0x1f; minimum = 0x80; }
		else if ((c & 0xf0) == 0xe0) { count = 2; code = c & 0x0f; minimum = 0x800; }
		else if ((c & 0xf8) == 0xf0) { count = 3; code = c & 0x07; minimum = 0x10000; }
		else return false;
		if (i + count > text.size()) return false;
		for (std::size_t j = 0; j < count; ++j) {
			const unsigned char continuation = bytes[i++];
			if ((continuation & 0xc0) != 0x80) return false;
			code = (code << 6) | (continuation & 0x3f);
		}
		if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
	}
	return true;
}

inline bool IsSingleLineText(const std::string& text) {
	for (const unsigned char byte : text) if (byte < 0x20 || byte == 0x7f) return false;
	return true;
}

inline bool IsTableAction(ActionKind kind) {
	return kind == ActionKind::Queue || kind == ActionKind::Unqueue ||
		kind == ActionKind::Watch || kind == ActionKind::Unwatch ||
	kind == ActionKind::Ready || kind == ActionKind::Unready ||
	kind == ActionKind::SetRules || kind == ActionKind::RecordResult ||
		kind == ActionKind::MatchFinished || kind == ActionKind::CancelResult || kind == ActionKind::AbortMatch ||
		kind == ActionKind::AcknowledgeTerminal;
}

inline int ReadInt(const nlohmann::json& object, const char* key, int low, int high) {
	const auto& value = object.at(key);
	if (!value.is_number_integer()) throw std::invalid_argument("room integer field");
	if(value.is_number_unsigned()&&value.get<std::uint64_t>()>static_cast<std::uint64_t>(high))
		throw std::out_of_range("room integer field");
	const long long parsed = value.get<long long>();
	if (parsed < low || parsed > high) throw std::out_of_range("room integer field");
	return static_cast<int>(parsed);
}

inline std::uint64_t ReadU64(const nlohmann::json& object, const char* key) {
	const auto& value = object.at(key);
	if (!value.is_number_unsigned()) throw std::invalid_argument("room unsigned field");
	return value.get<std::uint64_t>();
}

inline std::uint64_t ReadU64Range(const nlohmann::json& object, const char* key, std::uint64_t maximum) {
	const auto value = ReadU64(object, key);
	if (value > maximum) throw std::out_of_range("room bounded unsigned field");
	return value;
}

inline std::string ReadText(const nlohmann::json& object, const char* key, std::size_t maximum, bool allowEmpty = true) {
	const auto& value = object.at(key);
	if (!value.is_string()) throw std::invalid_argument("room text field");
	const auto text = value.get<std::string>();
	if ((!allowEmpty && text.empty()) || text.size() > maximum || !IsValidUtf8(text) || !IsSingleLineText(text)) {
		throw std::invalid_argument("room text bounds");
	}
	return text;
}

template <typename T>
void ReadMemberList(const nlohmann::json& object, const char* key, std::vector<T>& value, std::size_t maximum) {
	const auto& input = object.at(key);
	if (!input.is_array() || input.size() > maximum) throw std::out_of_range("room member list bounds");
	input.get_to(value);
}

inline void ValidateMemberIds(const std::vector<MemberId>& ids) {
	std::set<MemberId> seen;
	for (const auto id : ids) if (id == 0 || !seen.insert(id).second) throw std::invalid_argument("room duplicate member");
}

} } }
