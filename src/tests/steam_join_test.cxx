// Unit tests for SteamJoin.hxx: the connect-string forms the legacy Steam
// API moves around, all defined strictly in terms of JoinLink.hxx.

#include "../common/SteamJoin.hxx"

#include <cstring>
#include <string>
#include <vector>

#include "test_support.hxx"

using namespace sf4e::steam_join;

static void TestConnectStringRoundTripsTheCode() {
	CHECK(ConnectString("7K3M0X1RT9PZ") == "ember://join/7K3M-0X1R-T9PZ");
	CHECK(ParseConnectString(ConnectString("7K3M0X1RT9PZ")) == "7K3M0X1RT9PZ");
	CHECK(ConnectString("").empty());
	CHECK(ConnectString("7K3M").empty());
	// The connect string Steam carries is well under its 256-byte Rich
	// Presence value limit.
	CHECK(ConnectString("7K3M0X1RT9PZ").size() < 32);
}

static void TestParseConnectStringIsTheStrictUriParser() {
	// Canonical form, lowercase scheme and code, trailing slash, O/I/L
	// aliases: all already covered by join_link::ParseUri; this only checks
	// SteamJoin forwards to it rather than adding a second grammar.
	CHECK(ParseConnectString("ember://join/7K3M-0X1R-T9PZ") == "7K3M0X1RT9PZ");
	CHECK(ParseConnectString("EMBER://JOIN/7k3m-0x1r-t9pz/") == "7K3M0X1RT9PZ");
	CHECK(ParseConnectString("ember://join/7K3M-OXIR-T9PZ") == "7K3M0X1RT9PZ");
	const char* refused[] = {
		"", "ember://join/", "ember://join", "ember://join/7K3M-0X1R-T9PZ?x=1",
		"ember://join/7K3M-0X1R-T9PZ#7K3M", "ember://user@join/7K3M-0X1R-T9PZ",
		"ember://join:80/7K3M-0X1R-T9PZ", "ember://join/7K3M-0X1R-T9PZ extra",
		"ember://join/7K3M%2D0X1R-T9PZ", "ember://join/7K3M-0X1R-T9P\xC3\xA9",
	};
	for (const char* text : refused) CHECK(ParseConnectString(text).empty());
}

static void TestFromShortLinkAcceptsOnlyTheRoomsOwnLink() {
	CHECK(FromShortLink("https://embernetplay.link/j#7K3M-0X1R-T9PZ") == "7K3M0X1RT9PZ");
	CHECK(FromShortLink("https://embernetplay.link/j#7k3m0x1rt9pz") == "7K3M0X1RT9PZ");
	const std::string refused[] = {
		"", "https://embernetplay.link/j#", "https://embernetplay.link/j",
		"https://embernetplay.link/m#7K3M-0X1R-T9PZ", "http://embernetplay.link/j#7K3M-0X1R-T9PZ",
		"https://example.com/j#7K3M-0X1R-T9PZ", "https://embernetplay.link/j#7K3M-0X1R-T9P",
		"https://embernetplay.link/j#7K3M-0X1R-T9PZZ", " https://embernetplay.link/j#7K3M-0X1R-T9PZ",
		"https://embernetplay.link/j#7K3M-0X1R-T9PZ ", "ember://join/7K3M-0X1R-T9PZ",
		"https://embernetplay.link/j#" + std::string(100, '7'),
	};
	for (const auto& text : refused) CHECK(FromShortLink(text).empty());
}

static void TestFromFixedBufferRequiresANulAndAValidUri() {
	const char good[] = "ember://join/7K3M-0X1R-T9PZ";
	CHECK(FromFixedBuffer(good, sizeof(good)) == "7K3M0X1RT9PZ");
	// No NUL anywhere within the callback's buffer: refused outright, never
	// read past the bound.
	char noNul[8] = { 'e', 'm', 'b', 'e', 'r', ':', '/', '/' };
	CHECK(FromFixedBuffer(noNul, sizeof(noNul)).empty());
	// A NUL followed by trailing garbage still only ever looks at the bytes
	// before the NUL.
	char withTrailer[40] = {};
	std::memcpy(withTrailer, good, sizeof(good));
	std::memset(withTrailer + sizeof(good), 'X', sizeof(withTrailer) - sizeof(good) - 1);
	CHECK(FromFixedBuffer(withTrailer, sizeof(withTrailer)) == "7K3M0X1RT9PZ");
	// A buffer that is all NUL, or a malformed URI before its NUL.
	char empty[8] = {};
	CHECK(FromFixedBuffer(empty, sizeof(empty)).empty());
	char malformed[] = "not-a-join-link";
	CHECK(FromFixedBuffer(malformed, sizeof(malformed)).empty());
	CHECK(FromFixedBuffer(nullptr, 10).empty());
	CHECK(FromFixedBuffer(good, 0).empty());
}

static void TestFromLauncherRemainderAcceptsExactlyOneUri() {
	CHECK(FromLauncherRemainder({ "ember://join/7K3M-0X1R-T9PZ" }) == "7K3M0X1RT9PZ");
	// The executable path and ordinary USF4 arguments are ignored; the one
	// valid URI may appear anywhere, including after %command%'s expansion.
	CHECK(FromLauncherRemainder({ "C:\\Games\\SSFIV.exe", "-dvdcheck", "ember://join/7K3M-0X1R-T9PZ" }) == "7K3M0X1RT9PZ");
	CHECK(FromLauncherRemainder({ "ember://join/7K3M-0X1R-T9PZ", "-dvdcheck" }) == "7K3M0X1RT9PZ");
	// No valid URI at all.
	CHECK(FromLauncherRemainder({ "C:\\Games\\SSFIV.exe", "-dvdcheck" }).empty());
	CHECK(FromLauncherRemainder({}).empty());
	// A malformed candidate before or after the one valid URI does not
	// prevent acceptance.
	CHECK(FromLauncherRemainder({ "ember://join/bad", "ember://join/7K3M-0X1R-T9PZ", "garbage" }) == "7K3M0X1RT9PZ");
	// Two valid URIs are ambiguous and refused outright.
	CHECK(FromLauncherRemainder({ "ember://join/7K3M-0X1R-T9PZ", "ember://join/ABCDEFGHJKMN" }).empty());
}

int main() {
	TestConnectStringRoundTripsTheCode();
	TestParseConnectStringIsTheStrictUriParser();
	TestFromShortLinkAcceptsOnlyTheRoomsOwnLink();
	TestFromFixedBufferRequiresANulAndAValidUri();
	TestFromLauncherRemainderAcceptsExactlyOneUri();
	std::printf("steam_join_test: all tests passed\n");
	return 0;
}
