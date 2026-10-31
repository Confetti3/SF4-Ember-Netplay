#pragma once
// Text a player or a peer wrote, at the places it is stored, cut or drawn. One
// decoder answers every question: is this well-formed UTF-8, where may it be
// cut without splitting a character, and which code point is here. C++11, so
// the room model can include it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e {

// Bytes of the well-formed UTF-8 character starting at text[at], or 0 when the
// bytes there are not one: a stray continuation byte, an overlong form, a
// surrogate, a value past U+10FFFF, or a character cut short by the end. When
// it is one, `codePoint` (if given) gets its code point.
inline std::size_t Utf8CharacterBytes(const std::string& text, std::size_t at, std::uint32_t* codePoint = nullptr) {
	const auto* bytes = reinterpret_cast<const unsigned char*>(text.data()) + at;
	const unsigned char lead = bytes[0];
	if (lead < 0x80) { if (codePoint) *codePoint = lead; return 1; }
	std::size_t extra = 0;
	std::uint32_t code = 0, minimum = 0;
	if ((lead & 0xe0) == 0xc0) { extra = 1; code = lead & 0x1f; minimum = 0x80; }
	else if ((lead & 0xf0) == 0xe0) { extra = 2; code = lead & 0x0f; minimum = 0x800; }
	else if ((lead & 0xf8) == 0xf0) { extra = 3; code = lead & 0x07; minimum = 0x10000; }
	else return 0;
	if (at + extra >= text.size()) return 0;
	for (std::size_t i = 1; i <= extra; ++i) {
		if ((bytes[i] & 0xc0) != 0x80) return 0;
		code = (code << 6) | (bytes[i] & 0x3f);
	}
	if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
	if (codePoint) *codePoint = code;
	return extra + 1;
}

inline bool IsWellFormedUtf8(const std::string& text) {
	for (std::size_t at = 0; at < text.size();) {
		const std::size_t length = Utf8CharacterBytes(text, at);
		if (!length) return false;
		at += length;
	}
	return true;
}

// The first `maxCharacters` characters of `text`, with a byte that is not part
// of a character as one U+FFFD, as Utf8Trim does. A cut never falls inside a
// character.
inline std::string Utf8Head(const std::string& text, std::size_t maxCharacters) {
	std::string result;
	std::size_t at = 0;
	for (std::size_t characters = 0; at < text.size() && characters < maxCharacters; ++characters) {
		const std::size_t length = Utf8CharacterBytes(text, at);
		if (length) result.append(text, at, length);
		else result.append("\xEF\xBF\xBD", 3);
		at += length ? length : 1;
	}
	return result;
}

// `text` as well-formed UTF-8 of at most `maxBytes` bytes. A byte that is not
// part of a character becomes U+FFFD, and text that does not fit is cut after
// the last whole character that does, never inside one. The same input gives
// the same output on every peer.
inline std::string Utf8Trim(const std::string& text, std::size_t maxBytes) {
	std::string result;
	result.reserve((std::min)(text.size(), maxBytes));
	for (std::size_t at = 0; at < text.size();) {
		const std::size_t length = Utf8CharacterBytes(text, at);
		if (result.size() + (length ? length : 3) > maxBytes) break;
		if (length) result.append(text, at, length);
		else result.append("\xEF\xBF\xBD", 3);
		at += length ? length : 1;
	}
	return result;
}

}
