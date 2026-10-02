#pragma once

// Tournament match links. A browser hands Ember
//   ember://tournament/open?bridge=<bridge id>&match=<match id>
// from the page a tournament site links to, and a player may paste that page's
// own link, https://embernetplay.link/m#<bridge id>/<match id>, instead.
// A link only names a match, so it is not a secret: the bridge lets only the
// match's two assigned Ember IDs claim it, and anyone else's Ember finds no
// such match in their list. This side checks only the link's shape. A site
// can also open Ember's Connect Discord screen for its service (ParseConnectLink).
//
// Pure component: no Windows, game or helper dependencies, unit tested.

#include <string>

namespace sf4e {
namespace tournament_link {

// <prefix>_ and a lowercase version 4 UUID.
static const std::size_t IdLength = 40;
// Longer than any valid link, short enough to refuse anything odd early.
static const std::size_t MaximumUriLength = 256;

struct MatchLink {
	std::string bridgeId, matchId;
	bool Valid() const { return !bridgeId.empty() && !matchId.empty(); }
};

// An ID as the bridge writes them: a three-letter prefix, "_" and a
// lowercase version 4 UUID.
inline bool IsPrefixedId(const std::string& text, const char* prefix) {
	if (text.size() != IdLength || text.compare(0, 3, prefix) != 0 || text[3] != '_') return false;
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

inline bool IsBridgeId(const std::string& text) { return IsPrefixedId(text, "brg"); }
inline bool IsMatchId(const std::string& text) { return IsPrefixedId(text, "emt"); }

inline MatchLink Checked(const std::string& bridgeId, const std::string& matchId) {
	MatchLink link;
	if (IsBridgeId(bridgeId) && IsMatchId(matchId)) { link.bridgeId = bridgeId; link.matchId = matchId; }
	return link;
}

// Whether `text` starts with `prefix`, ignoring the case of letters.
inline bool StartsWithFolded(const std::string& text, const char* prefix) {
	std::size_t i = 0;
	for (; prefix[i]; ++i) {
		if (i == text.size()) return false;
		char symbol = text[i];
		if (symbol >= 'A' && symbol <= 'Z') symbol = static_cast<char>(symbol - 'A' + 'a');
		if (symbol != prefix[i]) return false;
	}
	return true;
}

// Printable ASCII only, without escapes, spaces or anything that could hide
// a second part; nothing longer than MaximumUriLength.
inline bool PlainText(const std::string& text) {
	if (text.empty() || text.size() > MaximumUriLength) return false;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const unsigned char character = static_cast<unsigned char>(text[i]);
		if (character < 0x21 || character > 0x7E || character == '%' || character == '+') return false;
	}
	return true;
}

// The match a browser link names, or an invalid MatchLink. Only
// ember://tournament/open with exactly the bridge and match parameters, once
// each and in either order: no user, port, fragment or other parameters. A
// browser's trailing slash after the path or at the end is ignored.
inline MatchLink ParseLink(const std::string& text) {
	static const char prefix[] = "ember://tournament/open";
	if (!PlainText(text) || text.find('#') != std::string::npos || !StartsWithFolded(text, prefix)) return MatchLink();
	std::string rest = text.substr(sizeof(prefix) - 1);
	if (rest.compare(0, 2, "/?") == 0) rest.erase(0, 1);
	if (rest.empty() || rest[0] != '?') return MatchLink();
	std::string query = rest.substr(1);
	if (!query.empty() && query[query.size() - 1] == '/') query.erase(query.size() - 1);
	const std::size_t split = query.find('&');
	if (split == std::string::npos || query.find('&', split + 1) != std::string::npos) return MatchLink();
	std::string bridge, match;
	bool bridgeSeen = false, matchSeen = false;
	const std::string parts[2] = { query.substr(0, split), query.substr(split + 1) };
	for (const auto& part : parts) {
		const std::size_t equals = part.find('=');
		if (equals == std::string::npos || part.find('=', equals + 1) != std::string::npos) return MatchLink();
		const std::string name = part.substr(0, equals), value = part.substr(equals + 1);
		if (name == "bridge" && !bridgeSeen) { bridgeSeen = true; bridge = value; }
		else if (name == "match" && !matchSeen) { matchSeen = true; match = value; }
		else return MatchLink();
	}
	return Checked(bridge, match);
}

// The service a Discord connect link names, or "". A tournament site that
// could not find a player sends them, through embernetplay.link/start, to
//   ember://discord/connect?bridge=<bridge id>
// and Ember opens its Connect Discord screen for that service. Like a match
// link it is not a secret and does nothing by itself: the player presses
// Connect there. Only that path with exactly the bridge parameter; a
// browser's trailing slash is ignored.
inline std::string ParseConnectLink(const std::string& text) {
	static const char prefix[] = "ember://discord/connect";
	if (!PlainText(text) || text.find('#') != std::string::npos || !StartsWithFolded(text, prefix)) return std::string();
	std::string rest = text.substr(sizeof(prefix) - 1);
	if (rest.compare(0, 2, "/?") == 0) rest.erase(0, 1);
	if (rest.compare(0, 8, "?bridge=") != 0) return std::string();
	std::string bridge = rest.substr(8);
	if (!bridge.empty() && bridge[bridge.size() - 1] == '/') bridge.erase(bridge.size() - 1);
	return IsBridgeId(bridge) ? bridge : std::string();
}

// The page a tournament site links to. Its fragment carries the match, so
// the web server never sees which one.
inline const char* PagePrefix() { return "https://embernetplay.link/m#"; }

// The match the page's own link names, or an invalid MatchLink.
inline MatchLink ParsePageLink(const std::string& text) {
	if (!PlainText(text) || !StartsWithFolded(text, PagePrefix())) return MatchLink();
	std::string rest = text.substr(std::char_traits<char>::length(PagePrefix()));
	if (!rest.empty() && rest[rest.size() - 1] == '/') rest.erase(rest.size() - 1);
	const std::size_t slash = rest.find('/');
	if (slash == std::string::npos) return MatchLink();
	return Checked(rest.substr(0, slash), rest.substr(slash + 1));
}

// What a player pasted: either link, with stray spaces or quotes around it.
inline MatchLink ParsePasted(const std::string& text) {
	std::string trimmed = text;
	while (!trimmed.empty() && (trimmed[0] == ' ' || trimmed[0] == '"')) trimmed.erase(0, 1);
	while (!trimmed.empty() && (trimmed[trimmed.size() - 1] == ' ' || trimmed[trimmed.size() - 1] == '"'))
		trimmed.erase(trimmed.size() - 1);
	const MatchLink link = ParseLink(trimmed);
	return link.Valid() ? link : ParsePageLink(trimmed);
}

} // namespace tournament_link
} // namespace sf4e
