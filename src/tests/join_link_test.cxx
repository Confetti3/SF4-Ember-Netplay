// Unit tests for room links, tournament match links and Discord connect links
// handed over by a browser (JoinLink.hxx, TournamentLink.hxx) and their
// hand-over from a second launcher to a running game (JoinLinkMailbox.hxx).

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
static const char* const Match = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";

static void TestOnlyTheTournamentLinkIsAccepted() {
	using namespace sf4e::tournament_link;
	const std::string bridge = Bridge, match = Match;
	CHECK(IsBridgeId(bridge) && IsMatchId(match) && !IsMatchId(bridge) && !IsBridgeId(match));
	const std::string link = "ember://tournament/open?bridge=" + bridge + "&match=" + match;
	const MatchLink parsed = ParseLink(link);
	CHECK(parsed.Valid() && parsed.bridgeId == bridge && parsed.matchId == match);
	CHECK(ParseLink("ember://tournament/open?match=" + match + "&bridge=" + bridge).matchId == match);
	CHECK(ParseLink("EMBER://TOURNAMENT/OPEN/?bridge=" + bridge + "&match=" + match + "/").Valid());
	const std::string refused[] = {
		"", "ember://tournament/open", "ember://tournament/open?", "ember://tournament/open?bridge=" + bridge,
		link + "&extra=1", link + "&match=" + match, "ember://tournament/open?bridge=" + bridge + "&bridge=" + bridge,
		link + "#x", "ember://tournament/open?bridge=" + bridge + "&match=" + match.substr(0, 39),
		"ember://tournament/open?bridge=" + bridge + "&match=" + bridge,
		"ember://tournament/open?bridge=" + bridge + "&match=emt%5F6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12",
		"ember://tournament/open?bridge=BRG_0DBC0598-2312-4CE3-9DF8-E160330565E6&match=" + match,
		"ember://tournament/open?bridge=brg_0dbc0598-2312-3ce3-9df8-e160330565e6&match=" + match,
		"ember://tournament/join?bridge=" + bridge + "&match=" + match, "ember://join/7K3M-0X1R-T9PZ",
		"ember://user@tournament/open?bridge=" + bridge + "&match=" + match, link + " ", link + "\n",
		"ember://tournament/open?bridge=" + bridge + "&&match=" + match, "ember://tournament/open?bridge==" + bridge + "&match=" + match,
		"ember://tournament/open?bridge=" + bridge + "&handoff=" + match, link + std::string(200, 'A'),
	};
	for (const auto& uri : refused) CHECK(!ParseLink(uri).Valid());
	std::string nul = link;
	nul.insert(nul.begin() + 30, '\0');
	CHECK(!ParseLink(nul).Valid());
	// The page's own link, as its Copy button gives it.
	const std::string page = std::string(PagePrefix()) + bridge + "/" + match;
	CHECK(ParsePageLink(page).matchId == match && ParsePageLink(page).bridgeId == bridge);
	CHECK(ParsePageLink("HTTPS://EmberNetplay.link/m#" + bridge + "/" + match + "/").Valid());
	const std::string refusedPages[] = {
		"https://embernetplay.link/m#" + bridge, "https://embernetplay.link/m#" + match + "/" + bridge,
		"https://embernetplay.link/j#" + bridge + "/" + match, "https://example.com/m#" + bridge + "/" + match,
		"http://embernetplay.link/m#" + bridge + "/" + match, page + "/x", page + " x",
	};
	for (const auto& text : refusedPages) CHECK(!ParsePageLink(text).Valid());
	// Pasted text may have spaces or quotes around either link.
	CHECK(ParsePasted("\"" + link + "\"").matchId == match);
	CHECK(ParsePasted("  " + page + " ").bridgeId == bridge);
	CHECK(!ParsePasted(match).Valid());
	CHECK(!ParsePasted("hello").Valid());
}

static void TestOnlyTheConnectLinkIsAccepted() {
	using namespace sf4e::tournament_link;
	const std::string bridge = Bridge, link = "ember://discord/connect?bridge=" + bridge;
	CHECK(ParseConnectLink(link) == bridge);
	CHECK(ParseConnectLink("EMBER://DISCORD/CONNECT/?bridge=" + bridge + "/") == bridge);
	const std::string refused[] = {
		"", "ember://discord/connect", "ember://discord/connect?", "ember://discord/connect?bridge=",
		link + "&extra=1", link + "&bridge=" + bridge, link + "#x", link + " ", link + "\n",
		"ember://discord/connect?bridge=" + bridge.substr(0, 39), "ember://discord/connect?bridge=" + std::string(Match),
		"ember://discord/connect?bridge=BRG_0DBC0598-2312-4CE3-9DF8-E160330565E6",
		"ember://discord/connect?bridge=brg%5F0dbc0598-2312-4ce3-9df8-e160330565e6",
		"ember://discord/connect?Bridge=" + bridge, "ember://discord/connect?match=" + bridge,
		"ember://discord/connectx?bridge=" + bridge, "ember://discord/remove?bridge=" + bridge,
		"ember://user@discord/connect?bridge=" + bridge, "ember://tournament/open?bridge=" + bridge,
		"https://embernetplay.link/start#" + bridge, link + std::string(200, 'A'),
	};
	for (const auto& uri : refused) CHECK(ParseConnectLink(uri).empty());
	std::string nul = link;
	nul.insert(nul.begin() + 30, '\0');
	CHECK(ParseConnectLink(nul).empty());
	// A connect link is neither a room nor a match link.
	CHECK(ParseUri(link).empty() && !ParseLink(link).Valid());
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
	// A match link travels in its own slot, checked on both ends.
	using sf4e::platform::DeliverMatchLink;
	using sf4e::platform::MatchLinkMailbox;
	const std::wstring matchSection = L"Local\\SF4EmberMatchLinkTest" + id, matchReady = L"Local\\SF4EmberMatchLinkTestReady" + id;
	const auto link = sf4e::tournament_link::Checked(Bridge, Match);
	CHECK(!DeliverMatchLink(link, matchSection.c_str(), matchReady.c_str()));
	MatchLinkMailbox links(matchSection.c_str(), matchReady.c_str());
	CHECK(links.Open() && !links.Take().Valid());
	CHECK(DeliverMatchLink(link, matchSection.c_str(), matchReady.c_str()));
	const auto taken = links.Take();
	CHECK(taken.bridgeId == Bridge && taken.matchId == Match);
	CHECK(!links.Take().Valid());
	sf4e::tournament_link::MatchLink bad = link;
	bad.matchId = "not-a-match";
	CHECK(!DeliverMatchLink(bad, matchSection.c_str(), matchReady.c_str()));
	// Another process of the player's could write the slot; a malformed one is dropped.
	sf4e::platform::MatchLinkSlot forged = {};
	std::memcpy(forged.bridge, Bridge, 40);
	std::memcpy(forged.match, "x", 1);
	CHECK(sf4e::platform::DeliverSlot(forged, matchSection.c_str(), matchReady.c_str()));
	CHECK(!links.Take().Valid());
	// A connect link travels in a slot of its own too.
	using sf4e::platform::ConnectLinkMailbox;
	using sf4e::platform::DeliverConnectLink;
	const std::wstring connectSection = L"Local\\SF4EmberConnectLinkTest" + id, connectReady = L"Local\\SF4EmberConnectLinkTestReady" + id;
	CHECK(!DeliverConnectLink(Bridge, connectSection.c_str(), connectReady.c_str()));
	ConnectLinkMailbox connects(connectSection.c_str(), connectReady.c_str());
	CHECK(connects.Open() && connects.Take().empty());
	CHECK(DeliverConnectLink(Bridge, connectSection.c_str(), connectReady.c_str()));
	CHECK(connects.Take() == Bridge && connects.Take().empty());
	CHECK(!DeliverConnectLink(Match, connectSection.c_str(), connectReady.c_str()));
	sf4e::platform::ConnectLinkSlot forgedConnect = {};
	std::memcpy(forgedConnect.bridge, Match, 40);
	CHECK(sf4e::platform::DeliverSlot(forgedConnect, connectSection.c_str(), connectReady.c_str()));
	CHECK(connects.Take().empty());
}
#endif

int main() {
	TestCodesNormalizeLikeTheHelper();
	TestOnlyTheJoinLinkIsAccepted();
	TestTheJoinScreenGetsTheShortLink();
	TestOnlyTheTournamentLinkIsAccepted();
	TestOnlyTheConnectLinkIsAccepted();
#ifdef _WIN32
	TestARunningGameReceivesTheLink();
#endif
	std::printf("join_link_test: all tests passed\n");
	return 0;
}
