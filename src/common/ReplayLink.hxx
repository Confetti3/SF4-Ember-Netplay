#pragma once

// A link that asks Ember to play an archived replay:
//   ember://replay/open?file=<percent-encoded path>
// Another program on this PC (usf4-replay-saver) opens it; the launcher hands
// it to the running game, or starts the game with it. Only a plain path to a
// .emberreplay or .usf4replay file is accepted, so the link can name a file
// and nothing else.

#include <cctype>
#include <cstdio>
#include <string>

namespace sf4e { namespace replay_link {

constexpr std::size_t kLongestPath = 1024;

inline bool StartsWithFoldedReplay(const std::string& text, const char* prefix) {
	std::size_t i = 0;
	for (; prefix[i]; i++) {
		if (i >= text.size() || std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
	}
	return true;
}

// The file the link names, or "" for anything that is not such a link.
inline std::string ParseReplayLink(const std::string& text) {
	static const char prefix[] = "ember://replay/open?file=";
	if (text.size() > kLongestPath * 3 || !StartsWithFoldedReplay(text, prefix)) return std::string();
	std::string path;
	for (std::size_t i = sizeof(prefix) - 1; i < text.size(); i++) {
		char c = text[i];
		if (c == '%') {
			if (i + 2 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[i + 1])) || !std::isxdigit(static_cast<unsigned char>(text[i + 2]))) return std::string();
			c = static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
			i += 2;
		}
		if (static_cast<unsigned char>(c) < 0x20 || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|') return std::string();
		path += c;
	}
	if (path.empty() || path.size() > kLongestPath) return std::string();
	const auto dot = path.rfind('.');
	std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
	for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext == ".emberreplay" || ext == ".usf4replay" ? path : std::string();
}

// Percent-encodes a path for the link; letters, digits and a few safe marks stay.
inline std::string MakeReplayLink(const std::string& path) {
	std::string link = "ember://replay/open?file=";
	for (unsigned char c : path) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') link += static_cast<char>(c);
		else { char hex[4]; std::snprintf(hex, sizeof(hex), "%%%02X", c); link += hex; }
	}
	return link;
}

} }
