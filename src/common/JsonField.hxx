#pragma once
// The one way to read a field of JSON that came from outside (a phone's order,
// a file a player can edit): the field as the type the reader wants, or nothing.
// It never throws, whatever the value holds, where nlohmann's value(key,
// default) throws type_error on a field of another type and get<int>() narrows
// a number too big for an int.

#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace sf4e {

// The member `key` of an object, or null when `object` is not an object or has no such member.
inline const nlohmann::json* JsonMember(const nlohmann::json& object, const char* key) {
	if (!object.is_object()) return nullptr;
	const auto found = object.find(key);
	return found == object.end() ? nullptr : &*found;
}

// The member `key` as a T: bool, int (a whole number an int holds), double (any number) or std::string.
template <class T> std::optional<T> JsonField(const nlohmann::json& object, const char* key);

template <> inline std::optional<bool> JsonField<bool>(const nlohmann::json& object, const char* key) {
	const auto* value = JsonMember(object, key);
	if (!value || !value->is_boolean()) return std::nullopt;
	return value->get<bool>();
}
template <> inline std::optional<int> JsonField<int>(const nlohmann::json& object, const char* key) {
	const auto* value = JsonMember(object, key);
	if (!value) return std::nullopt;
	if (value->is_number_unsigned()) {
		const auto number = value->get<std::uint64_t>();
		if (number > static_cast<std::uint64_t>(INT_MAX)) return std::nullopt;
		return static_cast<int>(number);
	}
	if (!value->is_number_integer()) return std::nullopt;
	const auto number = value->get<std::int64_t>();
	if (number < INT_MIN || number > INT_MAX) return std::nullopt;
	return static_cast<int>(number);
}
template <> inline std::optional<double> JsonField<double>(const nlohmann::json& object, const char* key) {
	const auto* value = JsonMember(object, key);
	if (!value || !value->is_number()) return std::nullopt;
	return value->get<double>();
}
template <> inline std::optional<std::string> JsonField<std::string>(const nlohmann::json& object, const char* key) {
	const auto* value = JsonMember(object, key);
	if (!value || !value->is_string()) return std::nullopt;
	return value->get<std::string>();
}

}
