#pragma once

// A link or code as a chat app hands it back when copied: invisible marks
// inside it, spaces, quotes, brackets or a full stop around it, Steam's link
// filter in front of it, or its '#' written as %23. Tidied takes those off and
// nothing else; the link and invitation parsers still decide what the text is.
//
// Pure component: no Windows, game or helper dependencies, unit tested.

#include <cstddef>
#include <initializer_list>
#include <string>

namespace sf4e {
namespace pasted_link {

// The length of the invisible mark that starts at `at` (UTF-8), else 0: a
// byte order mark, zero-width space, joiners, direction marks, word joiner,
// no-break space or soft hyphen. None of them belongs in a link.
inline std::size_t InvisibleAt(const std::string& text, std::size_t at) {
	const auto byte = [&](std::size_t i) { return at + i < text.size() ? static_cast<unsigned char>(text[at + i]) : 0u; };
	if (byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) return 3;
	if (byte(0) == 0xE2 && byte(1) == 0x80 && byte(2) >= 0x8B && byte(2) <= 0x8F) return 3;
	if (byte(0) == 0xE2 && byte(1) == 0x81 && byte(2) == 0xA0) return 3;
	if (byte(0) == 0xC2 && (byte(1) == 0xA0 || byte(1) == 0xAD)) return 2;
	return 0;
}

inline bool StartsWithNoCase(const std::string& text, std::size_t at, const char* prefix) {
	for (std::size_t i = 0; prefix[i]; ++i) {
		if (at + i >= text.size()) return false;
		char symbol = text[at + i];
		if (symbol >= 'A' && symbol <= 'Z') symbol = static_cast<char>(symbol - 'A' + 'a');
		if (symbol != prefix[i]) return false;
	}
	return true;
}

inline int HexValue(char symbol) {
	if (symbol >= '0' && symbol <= '9') return symbol - '0';
	if (symbol >= 'a' && symbol <= 'f') return symbol - 'a' + 10;
	if (symbol >= 'A' && symbol <= 'F') return symbol - 'A' + 10;
	return -1;
}

// %XX escapes decoded; a '%' without two hex digits is kept as it is.
inline std::string Unescaped(const std::string& text) {
	std::string plain;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const int high = text[i] == '%' && i + 2 < text.size() ? HexValue(text[i + 1]) : -1;
		const int low = high >= 0 ? HexValue(text[i + 2]) : -1;
		if (low >= 0) { plain.push_back(static_cast<char>(high * 16 + low)); i += 2; }
		else plain.push_back(text[i]);
	}
	return plain;
}

// The address Steam's link filter (steamcommunity.com/linkfilter/?u= or ?url=)
// leads to, unescaped; the text itself when it is not one.
inline std::string Unfiltered(const std::string& text) {
	static const char* const filters[] = {"https://steamcommunity.com/linkfilter/?", "http://steamcommunity.com/linkfilter/?"};
	for (const char* filter : filters) {
		if (!StartsWithNoCase(text, 0, filter)) continue;
		std::size_t at = std::char_traits<char>::length(filter);
		while (at < text.size()) {
			const std::size_t end = text.find('&', at);
			const std::size_t stop = end == std::string::npos ? text.size() : end;
			for (const char* name : {"u=", "url="})
				if (StartsWithNoCase(text, at, name)) {
					const std::size_t from = at + std::char_traits<char>::length(name);
					return Unescaped(text.substr(from, stop - from));
				}
			at = stop + 1;
		}
	}
	return text;
}

// An embernetplay.link address with its '#' escaped as %23 gets it back.
inline std::string FragmentRestored(const std::string& text) {
	std::size_t at = 0;
	for (const char* scheme : {"https://", "http://"})
		if (StartsWithNoCase(text, 0, scheme)) { at = std::char_traits<char>::length(scheme); break; }
	if (StartsWithNoCase(text, at, "www.")) at += 4;
	if (!StartsWithNoCase(text, at, "embernetplay.link/")) return text;
	std::string restored = text;
	for (std::size_t found; (found = restored.find("%23")) != std::string::npos;) restored.replace(found, 3, "#");
	return restored;
}

inline std::string Tidied(const std::string& text) {
	std::string visible;
	for (std::size_t i = 0; i < text.size();) {
		if (const std::size_t mark = InvisibleAt(text, i)) { i += mark; continue; }
		visible.push_back(text[i++]);
	}
	// Spaces, line ends and tabs, and the quotes, brackets and sentence marks a
	// message puts around a link; no link or code starts or ends with them.
	const auto stray = [](char symbol) {
		return symbol == ' ' || symbol == '\t' || symbol == '\r' || symbol == '\n' ||
			std::string("\"'<>()[]{}.,;!?").find(symbol) != std::string::npos;
	};
	std::size_t first = 0, last = visible.size();
	while (first < last && stray(visible[first])) ++first;
	while (last > first && stray(visible[last - 1])) --last;
	return FragmentRestored(Unfiltered(visible.substr(first, last - first)));
}

} // namespace pasted_link
} // namespace sf4e
