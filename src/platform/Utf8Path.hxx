#pragma once
// Paths to and from the UTF-8 text Ember keeps them in (manifests, journals,
// replay lists). C++20 made path::u8string() return std::u8string and
// deprecated filesystem::u8path, so the conversion differs by standard: SF4
// Ember builds as C++17 and SFV Ember as C++20, and both share this code.

#include <filesystem>
#include <string>

namespace sf4e { namespace platform {

inline std::filesystem::path PathFromUtf8(const std::string& text) {
#if defined(__cpp_lib_char8_t)
	return std::filesystem::path(std::u8string(text.begin(), text.end()));
#else
	return std::filesystem::u8path(text);
#endif
}

inline std::string PathToUtf8(const std::filesystem::path& path) {
	const auto text = path.u8string();
	return std::string(text.begin(), text.end());
}

// With '/' between the parts on every system, as manifests and journals hold them.
inline std::string GenericPathToUtf8(const std::filesystem::path& path) {
	const auto text = path.generic_u8string();
	return std::string(text.begin(), text.end());
}

} }
