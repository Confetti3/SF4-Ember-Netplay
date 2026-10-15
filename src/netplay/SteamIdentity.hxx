#pragma once

// Pure helpers for the two Steam identity values the wrapper ever reads:
// the persona name (display only, never an identity) and the SteamID64
// (a stable external account subject, but never ranking authentication on
// its own; see docs/design/STEAM_IDENTITY_AND_INVITES.md, "Trust boundary
// for ranking"). No Windows, game or Steam SDK dependencies: this is tested
// without steam_api.dll in src/tests/steam_identity_test.cxx.

#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e {
namespace steam_identity {

// Ember's display name limit (see PlayerPreferences::Valid): nonempty and
// strictly fewer than 32 bytes.
static const std::size_t MaximumDisplayNameBytes = 32;

// Steam allows a persona name far larger than Ember's display name field and
// places no restriction on control characters. This copies `name` into a
// sanitized Ember-sized name by: dropping ASCII control characters and DEL;
// never splitting a UTF-8 code point; stopping strictly below
// MaximumDisplayNameBytes; and returning "" if nothing safe remains, so the
// caller keeps the player's existing name instead of writing an empty one.
// This does not canonicalize Unicode or claim the result is unique: it is
// display preparation, not an identity.
inline std::string SanitizePersonaName(const std::string& name) {
	std::string out;
	for (std::size_t i = 0; i < name.size();) {
		const unsigned char lead = static_cast<unsigned char>(name[i]);
		if (lead < 0x20 || lead == 0x7F) { ++i; continue; }
		std::size_t width = 1;
		if ((lead & 0xF8) == 0xF0) width = 4;
		else if ((lead & 0xF0) == 0xE0) width = 3;
		else if ((lead & 0xE0) == 0xC0) width = 2;
		else if ((lead & 0x80) != 0) { ++i; continue; } // a stray continuation byte: not valid UTF-8, drop it
		if (i + width > name.size()) break; // a code point cut off by the input: stop rather than split it
		if (out.size() + width >= MaximumDisplayNameBytes) break;
		out.append(name, i, width);
		i += width;
	}
	return out;
}

// GetSteamID().ConvertToUint64() bit layout (Valve's CSteamID): bits 0-31
// account ID, bits 32-51 instance, bits 52-55 account type, bits 56-63
// universe. This screens a value the Steam client itself reported; Ember
// never manufactures a CSteamID from room or network input.
inline bool IsValidIndividualSteamId(std::uint64_t steamId64) {
	if (steamId64 == 0) return false;
	const unsigned accountType = static_cast<unsigned>((steamId64 >> 52) & 0xF);
	const unsigned universe = static_cast<unsigned>((steamId64 >> 56) & 0xFF);
	const unsigned kAccountTypeIndividual = 1, kUniversePublic = 1;
	return accountType == kAccountTypeIndividual && universe == kUniversePublic;
}

// The non-identity textual form Ember may log or display, such as
// "Steam account ending in 1234": never a username, never shown as one.
inline std::string SteamAccountSubject(std::uint64_t steamId64) {
	if (!IsValidIndividualSteamId(steamId64)) return std::string();
	return "steam:" + std::to_string(steamId64);
}

} // namespace steam_identity
} // namespace sf4e
