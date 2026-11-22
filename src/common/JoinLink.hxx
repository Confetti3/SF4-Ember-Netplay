#pragma once

// Room links opened from a browser: ember://join/XXXX-XXXX-XXXX, the custom
// scheme the /j page on embernetplay.link hands a short code to. The code is
// the same 12-symbol Crockford base32 code the helper reads from a short link
// (rust/sf4-net/src/short_invite.rs); this side only checks its shape and
// turns it back into the https link the Join screen shows. Nothing here joins
// a room; what the shell does with a link, a direct join when the player is
// free, is ApplicationShell::UpdateJoinLink's.
//
// Pure component: no Windows, game or helper dependencies, unit tested.

#include <string>

namespace sf4e {
namespace join_link {

static const std::size_t CodeSymbols = 12;
// Longer than any valid link, short enough to refuse anything odd early.
static const std::size_t MaximumUriLength = 64;

inline const char* Alphabet() { return "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; }

// The canonical code in `text`, or "": 12 symbols, upper case, no dashes, and
// no spaces unless `skipSpaces`. Case does not matter, and O, I and L read as
// 0, 1, 1. No length cap here; more than 12 symbols is refused as it is read.
inline std::string ReadCode(const std::string& text, bool skipSpaces) {
	std::string code;
	for (std::size_t i = 0; i < text.size(); ++i) {
		char symbol = text[i];
		if (symbol == '-' || (skipSpaces && symbol == ' ')) continue;
		if (symbol >= 'a' && symbol <= 'z') symbol = static_cast<char>(symbol - 'a' + 'A');
		if (symbol == 'O') symbol = '0';
		else if (symbol == 'I' || symbol == 'L') symbol = '1';
		bool known = false;
		for (const char* allowed = Alphabet(); *allowed; ++allowed) known = known || *allowed == symbol;
		if (!known || code.size() == CodeSymbols) return std::string();
		code.push_back(symbol);
	}
	return code.size() == CodeSymbols ? code : std::string();
}

// The canonical code (12 symbols, upper case, no dashes) in `text`, or "".
// Dashes are ignored, case does not matter, and O, I and L read as 0, 1, 1.
inline std::string ParseCode(const std::string& text) {
	if (text.size() > MaximumUriLength) return std::string();
	return ReadCode(text, false);
}

// The code in a link the browser handed over (UTF-8), or "". Only
// ember://join/<code> with an optional trailing slash: no query, fragment,
// user, port, percent escapes or other characters, and nothing longer than
// MaximumUriLength.
inline std::string ParseUri(const std::string& text) {
	if (text.empty() || text.size() > MaximumUriLength) return std::string();
	for (std::size_t i = 0; i < text.size(); ++i) {
		const unsigned char character = static_cast<unsigned char>(text[i]);
		if (character < 0x21 || character > 0x7E) return std::string();
	}
	static const char prefix[] = "ember://join/";
	const std::size_t prefixLength = sizeof(prefix) - 1;
	if (text.size() <= prefixLength) return std::string();
	for (std::size_t i = 0; i < prefixLength; ++i) {
		char symbol = text[i];
		if (symbol >= 'A' && symbol <= 'Z') symbol = static_cast<char>(symbol - 'A' + 'a');
		if (symbol != prefix[i]) return std::string();
	}
	std::string rest = text.substr(prefixLength);
	if (!rest.empty() && rest[rest.size() - 1] == '/') rest.erase(rest.size() - 1);
	return ParseCode(rest);
}

// The code in a pasted invitation short link, or "": the embernetplay.link
// forms the helper reads (rust/sf4-net/src/short_invite.rs parse), with `#` or
// `/` after /j, with or without the scheme or www., in any case, and with
// spaces and dashes inside the code, which the helper skips. A bare code is
// not accepted: any 12-letter word is one, and a paste of only a code is Join
// Room's. Tidy the paste first (pasted_link::Tidied). Only the shape is
// checked, to route the paste; the helper still resolves it.
inline std::string ParsePastedInvitation(const std::string& text) {
	if (text.empty() || text.size() > 96) return std::string();
	std::string rest;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const char symbol = text[i];
		rest.push_back(symbol >= 'A' && symbol <= 'Z' ? static_cast<char>(symbol - 'A' + 'a') : symbol);
	}
	const auto strip = [&rest](const char* prefix) {
		const std::string head(prefix);
		if (rest.compare(0, head.size(), head) != 0) return false;
		rest.erase(0, head.size());
		return true;
	};
	if (!strip("https://")) strip("http://");
	strip("www.");
	if (!strip("embernetplay.link")) return std::string();
	if (!strip("/j#") && !strip("/j/#") && !strip("/j/")) return std::string();
	while (!rest.empty() && rest[rest.size() - 1] == '/') rest.erase(rest.size() - 1);
	return ReadCode(rest, true);
}

// XXXX-XXXX-XXXX.
inline std::string DisplayCode(const std::string& code) {
	if (code.size() != CodeSymbols) return std::string();
	return code.substr(0, 4) + "-" + code.substr(4, 4) + "-" + code.substr(8, 4);
}

// The short link the helper reads, for the Join screen's text box.
inline std::string ShortLink(const std::string& code) {
	const std::string shown = DisplayCode(code);
	return shown.empty() ? std::string() : "https://embernetplay.link/j#" + shown;
}

} // namespace join_link
} // namespace sf4e
