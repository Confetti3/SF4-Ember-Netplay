#pragma once

// The single place that turns Ember's room capability (the browser join
// link) into the few text forms the legacy Steam API moves around: the Rich
// Presence "connect" value, the InviteUserToGame() connect string, an
// incoming GameRichPresenceJoinRequested_t callback buffer, and a closed
// game's Steam launch command. Everything here reuses join_link::ParseUri and
// join_link::ParseCode (JoinLink.hxx) so there is exactly one definition of
// what a join link looks like; this file adds no new link grammar.
//
// Pure component: no Windows, game, Steam or helper dependencies, unit
// tested in src/tests/steam_join_test.cxx.

#include "JoinLink.hxx"

#include <cstddef>
#include <string>
#include <vector>

namespace sf4e {
namespace steam_join {

// The exact value to hand to SetRichPresence("connect", ...) and
// InviteUserToGame(), or "" if `code` is not a canonical 12-symbol code.
// Comfortably below the legacy Rich Presence 256-byte value limit.
inline std::string ConnectString(const std::string& code) {
	const std::string shown = join_link::DisplayCode(code);
	return shown.empty() ? std::string() : "ember://join/" + shown;
}

// The canonical code carried by a Rich Presence join request or a direct
// invite's connect string. Strict: only ember://join/<code>, nothing else.
inline std::string ParseConnectString(const std::string& text) {
	return join_link::ParseUri(text);
}

// The room's short https link (IrohRoom::ShortInvitation()), turned back into
// the connect string, or "" if it is not exactly that one known link shape.
// Only the expected origin, path and fragment are accepted; nothing else is
// ever treated as a join capability.
inline std::string FromShortLink(const std::string& link) {
	static const char prefix[] = "https://embernetplay.link/j#";
	const std::size_t prefixLength = sizeof(prefix) - 1;
	if (link.size() <= prefixLength) return std::string();
	if (link.size() - prefixLength > join_link::MaximumUriLength) return std::string();
	if (link.compare(0, prefixLength, prefix) != 0) return std::string();
	return join_link::ParseCode(link.substr(prefixLength));
}

// GameRichPresenceJoinRequested_t::m_rgchConnect is a fixed `size`-byte array
// that Steam does not promise to NUL-terminate inside its bound. A NUL must
// be found within the buffer; the bytes before it must parse as one
// canonical join URI. Anything else (no NUL, garbage after a NUL, a partial
// or decorated URI) is refused rather than guessed at.
inline std::string FromFixedBuffer(const char* buffer, std::size_t size) {
	if (buffer == nullptr || size == 0) return std::string();
	std::size_t length = 0;
	while (length < size && buffer[length] != '\0') ++length;
	if (length == size) return std::string(); // no NUL within the callback's buffer
	return join_link::ParseUri(std::string(buffer, length));
}

// Steam's `"<Ember install>\Launcher.exe" %command%` appends the accepted
// connect string to the launch command when the game was closed. This scans
// the launcher's unparsed remainder (CLI11's app.remaining(), never a
// recombined shell string) and accepts the join link only when exactly one
// argument parses as a canonical URI. The game's own executable path and
// ordinary arguments are ignored; two or more valid URIs are refused as
// ambiguous rather than picking one.
inline std::string FromLauncherRemainder(const std::vector<std::string>& remainder) {
	std::string found;
	for (std::size_t i = 0; i < remainder.size(); ++i) {
		const std::string code = join_link::ParseUri(remainder[i]);
		if (code.empty()) continue;
		if (!found.empty()) return std::string(); // more than one valid URI: ambiguous
		found = code;
	}
	return found;
}

} // namespace steam_join
} // namespace sf4e
