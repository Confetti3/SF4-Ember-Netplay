// Unit tests for room links and tournament match links handed over by a
// browser (JoinLink.hxx, TournamentLink.hxx) and their hand-over from a second
// launcher to a running game (JoinLinkMailbox.hxx).

#include "../common/JoinLink.hxx"
#include "../common/TournamentLink.hxx"
#ifdef _WIN32
#include "../platform/JoinLinkMailbox.hxx"
#endif

#include <cstdio>
#include <string>

#include "test_support.hxx"

using namespace sf4e::join_link;

static void TestCodesNormalizeLikeTheHelper() {
	CHECK(ParseCode("7K3M-0X1R-T9PZ") == "7K3M0X1RT9PZ");
	CHECK(ParseCode("7k3m0x1rt9pz") == "7K3M0X1RT9PZ");
	CHECK(ParseCode("7K3M-OXIR-T9PZ") == "7K3M0X1RT9PZ");
	CHECK(ParseCode("7K3M-0XLR-T9PZ") == "7K3M0X1RT9PZ");
	const char* refused[] = { "", "7K3M-0X1R-T9P", "7K3M-0X1R-T9PZZ", "7K3M-0X1R-T9PU", "7K3M 0X1R T9PZ",
		"7K3M_0X1R_T9PZ", "7K3M-0X1R-T9P\xC3\xA9", "sf4e3:AAAA" };
	for (const char* text : refused) CHECK(ParseCode(text).empty());
	CHECK(ParseCode(std::string(65, '7')).empty());
}

static void TestOnlyTheJoinLinkIsAccepted() {
	CHECK(ParseUri("ember://join/7K3M-0X1R-T9PZ") == "7K3M0X1RT9PZ");
	CHECK(ParseUri("EMBER://JOIN/7k3m-0x1r-t9pz/") == "7K3M0X1RT9PZ");
	CHECK(ParseUri("ember://join/7K3M0X1RT9PZ") == "7K3M0X1RT9PZ");
	const char* refused[] = {
		"", "ember://join/", "ember://join", "ember:join/7K3M-0X1R-T9PZ", "ember:/join/7K3M-0X1R-T9PZ",
		"ember://tournament/7K3M-0X1R-T9PZ", "ember://join/7K3M-0X1R-T9PZ//", "ember://join/7K3M-0X1R-T9PZ?x=1",
		"ember://join/7K3M-0X1R-T9PZ#7K3M", "ember://join/7K3M%2D0X1R-T9PZ", "ember://user@join/7K3M-0X1R-T9PZ",
		"ember://join:80/7K3M-0X1R-T9PZ", "ember://join/../7K3M-0X1R-T9PZ", "ember://join/7K3M-0X1R-T9PZ extra",
		"\"ember://join/7K3M-0X1R-T9PZ\"", "https://embernetplay.link/j#7K3M-0X1R-T9PZ", "ember://join/7K3M-0X1R-T9P\xC3\xA9",
		"ember://join/7K3M-0X1R-T9PZ\n", "emberx://join/7K3M-0X1R-T9PZ",
	};
	for (const char* uri : refused) CHECK(ParseUri(uri).empty());
	std::string nul = "ember://join/7K3M-0X1R-T9PZ";
	nul.insert(nul.begin() + 14, '\0');
	CHECK(ParseUri(nul).empty());
	CHECK(ParseUri("ember://join/" + std::string(52, '-') + "7K3M0X1RT9PZ").empty());
}

static void TestTheJoinScreenGetsTheShortLink() {
	CHECK(DisplayCode("7K3M0X1RT9PZ") == "7K3M-0X1R-T9PZ");
	CHECK(ShortLink("7K3M0X1RT9PZ") == "https://embernetplay.link/j#7K3M-0X1R-T9PZ");
	CHECK(ShortLink("").empty());
	CHECK(ShortLink("7K3M").empty());
}

static const char* const Bridge = "brg_0dbc0598-2312-4ce3-9df8-e160330565e6";
static const char* const Code = "q2Zp0yH4c8Jm1bWk7nVt3xR6sL9dF5gA2eU0iO4uYwA";

static void TestOnlyTheTournamentLinkIsAccepted() {
	using namespace sf4e::tournament_link;
	const std::string bridge = Bridge, code = Code;
	CHECK(IsHandoffCode(code) && IsBridgeId(bridge));
	const std::string link = "ember://tournament/open?bridge=" + bridge + "&handoff=" + code;
	Handoff parsed = ParseLink(link);
	CHECK(parsed.Valid() && parsed.bridgeId == bridge && parsed.code == code);
	CHECK(ParseLink("ember://tournament/open?handoff=" + code + "&bridge=" + bridge).code == code);
	CHECK(ParseLink("EMBER://TOURNAMENT/OPEN/?bridge=" + bridge + "&handoff=" + code + "/").Valid());
	const std::string refused[] = {
		"", "ember://tournament/open", "ember://tournament/open?", "ember://tournament/open?bridge=" + bridge,
		link + "&extra=1", link + "&handoff=" + code, "ember://tournament/open?bridge=" + bridge + "&bridge=" + bridge,
		link + "#x", "ember://tournament/open?bridge=" + bridge + "&handoff=" + code.substr(0, 42),
		"ember://tournament/open?bridge=" + bridge + "&handoff=" + code.substr(0, 42) + "B",
		"ember://tournament/open?bridge=" + bridge + "&handoff=" + code.substr(0, 20) + "%2B" + code.substr(23),
		"ember://tournament/open?bridge=BRG_0DBC0598-2312-4CE3-9DF8-E160330565E6&handoff=" + code,
		"ember://tournament/open?bridge=brg_0dbc0598-2312-3ce3-9df8-e160330565e6&handoff=" + code,
		"ember://tournament/join?bridge=" + bridge + "&handoff=" + code, "ember://join/7K3M-0X1R-T9PZ",
		"ember://user@tournament/open?bridge=" + bridge + "&handoff=" + code, link + " ", link + "\n",
		"ember://tournament/open?bridge=" + bridge + "&&handoff=" + code, "ember://tournament/open?bridge==" + bridge + "&handoff=" + code,
		link + std::string(200, 'A'),
	};
	for (const auto& uri : refused) CHECK(!ParseLink(uri).Valid());
	std::string nul = link;
	nul.insert(nul.begin() + 30, '\0');
	CHECK(!ParseLink(nul).Valid());
	// A pasted code goes to the selected service; a pasted link names its own.
	CHECK(ParsePasted(" " + code + " ", bridge).bridgeId == bridge);
	CHECK(ParsePasted("\"" + link + "\"", "").code == code);
	CHECK(!ParsePasted(code, "").Valid());
	CHECK(!ParsePasted("hello", bridge).Valid());
}

#ifdef _WIN32
// The launcher's hand-over to a running game, under names of the test's own.
static void TestARunningGameReceivesTheLink() {
	using sf4e::platform::DeliverJoinLink;
	using sf4e::platform::JoinLinkMailbox;
	const std::wstring id = std::to_wstring(GetCurrentProcessId());
	const std::wstring section = L"Local\\SF4EmberJoinLinkTest" + id, ready = L"Local\\SF4EmberJoinLinkTestReady" + id;
	// No game listening: the launcher hears so.
	CHECK(!DeliverJoinLink("7K3M0X1RT9PZ", section.c_str(), ready.c_str()));
	JoinLinkMailbox mailbox(section.c_str(), ready.c_str());
	CHECK(mailbox.Open());
	CHECK(mailbox.Take().empty());
	CHECK(DeliverJoinLink("7K3M0X1RT9PZ", section.c_str(), ready.c_str()));
	CHECK(mailbox.Take() == "7K3M0X1RT9PZ");
	CHECK(mailbox.Take().empty());
	// The newest of two wins; nothing but a canonical code is sent.
	CHECK(DeliverJoinLink("7K3M0X1RT9PZ", section.c_str(), ready.c_str()));
	CHECK(DeliverJoinLink("ABCDEFGHJKMN", section.c_str(), ready.c_str()));
	CHECK(mailbox.Take() == "ABCDEFGHJKMN");
	CHECK(mailbox.Take().empty());
	CHECK(!DeliverJoinLink("7K3M-0X1R-T9PZ", section.c_str(), ready.c_str()));
	CHECK(!DeliverJoinLink("", section.c_str(), ready.c_str()));
	CHECK(mailbox.Take().empty());
	// A tournament handoff travels in its own slot, checked on both ends.
	using sf4e::platform::DeliverTournamentHandoff;
	using sf4e::platform::TournamentHandoffMailbox;
	const std::wstring handoffSection = L"Local\\SF4EmberHandoffTest" + id, handoffReady = L"Local\\SF4EmberHandoffTestReady" + id;
	sf4e::tournament_link::Handoff handoff;
	handoff.bridgeId = Bridge;
	handoff.code = Code;
	CHECK(!DeliverTournamentHandoff(handoff, handoffSection.c_str(), handoffReady.c_str()));
	TournamentHandoffMailbox handoffs(handoffSection.c_str(), handoffReady.c_str());
	CHECK(handoffs.Open() && !handoffs.Take().Valid());
	CHECK(DeliverTournamentHandoff(handoff, handoffSection.c_str(), handoffReady.c_str()));
	const auto taken = handoffs.Take();
	CHECK(taken.bridgeId == Bridge && taken.code == Code);
	CHECK(!handoffs.Take().Valid());
	sf4e::tournament_link::Handoff bad = handoff;
	bad.code = "not-a-code";
	CHECK(!DeliverTournamentHandoff(bad, handoffSection.c_str(), handoffReady.c_str()));
	// Another process of the player's could write the slot; a malformed one is dropped.
	sf4e::platform::TournamentHandoffSlot forged = {};
	std::memcpy(forged.bridge, Bridge, 40);
	std::memcpy(forged.code, "x", 1);
	CHECK(sf4e::platform::DeliverSlot(forged, handoffSection.c_str(), handoffReady.c_str()));
	CHECK(!handoffs.Take().Valid());
}
#endif

int main() {
	TestCodesNormalizeLikeTheHelper();
	TestOnlyTheJoinLinkIsAccepted();
	TestTheJoinScreenGetsTheShortLink();
	TestOnlyTheTournamentLinkIsAccepted();
#ifdef _WIN32
	TestARunningGameReceivesTheLink();
#endif
	std::printf("join_link_test: all tests passed\n");
	return 0;
}
