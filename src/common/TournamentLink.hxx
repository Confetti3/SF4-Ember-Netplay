#pragma once

// Tournament match links a browser hands to Ember (spec 12.1):
//   ember://tournament/open?bridge=<bridge id>&handoff=<code>
// and the code alone, which a player may paste instead. The handoff is a
// one-use, one-minute code that only the expected player's Ember ID can
// redeem, and it names a match, nothing more; it is still never logged. This
// side checks only the link's shape: the bridge decides what it means.
//
// Pure component: no Windows, game or helper dependencies, unit tested.

#include <string>

namespace sf4e {
namespace tournament_link {

// 32 random bytes as unpadded base64url.
static const std::size_t HandoffLength = 43;
// brg_ and a lowercase version 4 UUID.
static const std::size_t BridgeIdLength = 40;
// Longer than any valid link, short enough to refuse anything odd early.
static const std::size_t MaximumUriLength = 256;

struct Handoff {
	std::string bridgeId, code;
	bool Valid() const { return !bridgeId.empty() && !code.empty(); }
};

inline int Base64UrlValue(char symbol) {
	if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
	if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
	if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
	if (symbol == '-') return 62;
	if (symbol == '_') return 63;
	return -1;
}

// A canonical handoff code: 43 base64url symbols whose last one carries no
// stray bits, as the bridge writes them.
inline bool IsHandoffCode(const std::string& text) {
	if (text.size() != HandoffLength) return false;
	for (std::size_t i = 0; i < text.size(); ++i)
		if (Base64UrlValue(text[i]) < 0) return false;
	return (Base64UrlValue(text[text.size() - 1]) & 0x03) == 0;
}

inline bool IsBridgeId(const std::string& text) {
	if (text.size() != BridgeIdLength || text.compare(0, 4, "brg_") != 0) return false;
	for (std::size_t i = 4; i < text.size(); ++i) {
		const std::size_t at = i - 4;
		const char symbol = text[i];
		const bool dash = at == 8 || at == 13 || at == 18 || at == 23;
		if (dash) { if (symbol != '-') return false; continue; }
		const bool hex = (symbol >= '0' && symbol <= '9') || (symbol >= 'a' && symbol <= 'f');
		if (!hex) return false;
		if (at == 14 && symbol != '4') return false;
		if (at == 19 && symbol != '8' && symbol != '9' && symbol != 'a' && symbol != 'b') return false;
	}
	return true;
}

// The bridge and code in a link the browser handed over, or an invalid
// Handoff. Only ember://tournament/open with exactly the bridge and handoff
// parameters, once each and in either order: no user, port, fragment,
// percent escapes, other parameters or characters. A browser's trailing slash
// after the path or at the end is ignored.
inline Handoff ParseLink(const std::string& text) {
	Handoff none;
	if (text.empty() || text.size() > MaximumUriLength) return none;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const unsigned char character = static_cast<unsigned char>(text[i]);
		if (character < 0x21 || character > 0x7E || character == '%' || character == '#' || character == '+') return none;
	}
	static const char prefix[] = "ember://tournament/open";
	const std::size_t prefixLength = sizeof(prefix) - 1;
	if (text.size() <= prefixLength) return none;
	for (std::size_t i = 0; i < prefixLength; ++i) {
		char symbol = text[i];
		if (symbol >= 'A' && symbol <= 'Z') symbol = static_cast<char>(symbol - 'A' + 'a');
		if (symbol != prefix[i]) return none;
	}
	std::string rest = text.substr(prefixLength);
	if (rest.compare(0, 2, "/?") == 0) rest.erase(0, 1);
	if (rest.empty() || rest[0] != '?') return none;
	std::string query = rest.substr(1);
	if (!query.empty() && query[query.size() - 1] == '/') query.erase(query.size() - 1);
	const std::size_t split = query.find('&');
	if (split == std::string::npos || query.find('&', split + 1) != std::string::npos) return none;
	Handoff found;
	bool bridgeSeen = false, handoffSeen = false;
	const std::string parts[2] = { query.substr(0, split), query.substr(split + 1) };
	for (std::size_t i = 0; i < 2; ++i) {
		const std::size_t equals = parts[i].find('=');
		if (equals == std::string::npos || parts[i].find('=', equals + 1) != std::string::npos) return none;
		const std::string name = parts[i].substr(0, equals), value = parts[i].substr(equals + 1);
		if (name == "bridge" && !bridgeSeen) { bridgeSeen = true; found.bridgeId = value; }
		else if (name == "handoff" && !handoffSeen) { handoffSeen = true; found.code = value; }
		else return none;
	}
	if (!IsBridgeId(found.bridgeId) || !IsHandoffCode(found.code)) return none;
	return found;
}

// What a player pasted: the whole link, or the code alone for `bridgeId`.
inline Handoff ParsePasted(const std::string& text, const std::string& bridgeId) {
	std::string trimmed = text;
	while (!trimmed.empty() && (trimmed[0] == ' ' || trimmed[0] == '"')) trimmed.erase(0, 1);
	while (!trimmed.empty() && (trimmed[trimmed.size() - 1] == ' ' || trimmed[trimmed.size() - 1] == '"'))
		trimmed.erase(trimmed.size() - 1);
	if (IsHandoffCode(trimmed) && IsBridgeId(bridgeId)) {
		Handoff handoff;
		handoff.bridgeId = bridgeId;
		handoff.code = trimmed;
		return handoff;
	}
	return ParseLink(trimmed);
}

} // namespace tournament_link
} // namespace sf4e
