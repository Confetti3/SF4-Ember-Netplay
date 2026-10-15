// Unit tests for SteamIdentity.hxx: persona-name sanitization and SteamID64
// screening, without steam_api.dll.

#include "../netplay/SteamIdentity.hxx"

#include <string>

#include "test_support.hxx"

using namespace sf4e::steam_identity;

static void TestSanitizePersonaName() {
	CHECK(SanitizePersonaName("Ryu") == "Ryu");
	CHECK(SanitizePersonaName("").empty());
	// Control characters and DEL are stripped outright.
	CHECK(SanitizePersonaName(std::string("Ry\x01u\x7F!")) == "Ryu!");
	CHECK(SanitizePersonaName("\x01\x02\x1F").empty());
	// Exactly at Ember's limit (31 bytes keeps it under the 32-byte exclusive bound).
	const std::string exact31(31, 'a');
	CHECK(SanitizePersonaName(exact31) == exact31);
	CHECK(SanitizePersonaName(exact31).size() < MaximumDisplayNameBytes);
	// 32 ASCII bytes must be trimmed, never split: stops at 31.
	const std::string exact32(32, 'a');
	CHECK(SanitizePersonaName(exact32).size() == 31);
	// A multibyte code point sitting on the boundary is dropped whole, not
	// split in half.
	std::string boundary(30, 'a');
	boundary += "\xE2\x82\xAC"; // one 3-byte euro sign, would land at byte 33
	CHECK(SanitizePersonaName(boundary) == std::string(30, 'a'));
	// A lone/garbled continuation byte is not valid UTF-8 and is dropped.
	CHECK(SanitizePersonaName(std::string("Ryu\x80ko")) == "Ryuko");
	// A code point truncated by the input itself (no trailing bytes) stops
	// rather than guesses.
	CHECK(SanitizePersonaName(std::string("Ryu\xE2\x82")) == "Ryu");
	// Leading/trailing spaces are kept as-is: Ember does not define a casing
	// or trimming policy for persona names beyond control-character removal.
	CHECK(SanitizePersonaName(" Ryu ") == " Ryu ");
}

static void TestSteamIdValidation() {
	// A real individual, public-universe SteamID64 (account type 1, universe 1).
	const std::uint64_t individual = (std::uint64_t(1) << 56) | (std::uint64_t(1) << 52) | 123456789ULL;
	CHECK(IsValidIndividualSteamId(individual));
	CHECK(!IsValidIndividualSteamId(0));
	// Wrong account type (e.g. a clan/group account, type 7).
	const std::uint64_t clan = (std::uint64_t(1) << 56) | (std::uint64_t(7) << 52) | 123456789ULL;
	CHECK(!IsValidIndividualSteamId(clan));
	// Wrong universe (e.g. beta, universe 2).
	const std::uint64_t beta = (std::uint64_t(2) << 56) | (std::uint64_t(1) << 52) | 123456789ULL;
	CHECK(!IsValidIndividualSteamId(beta));
	CHECK(SteamAccountSubject(individual) == "steam:" + std::to_string(individual));
	CHECK(SteamAccountSubject(0).empty());
	CHECK(SteamAccountSubject(clan).empty());
}

int main() {
	TestSanitizePersonaName();
	TestSteamIdValidation();
	std::printf("steam_identity_test: all tests passed\n");
	return 0;
}
