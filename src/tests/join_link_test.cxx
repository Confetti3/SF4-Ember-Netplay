// Unit tests for room links handed over by a browser (JoinLink.hxx) and their
// hand-over from a second launcher to a running game (JoinLinkMailbox.hxx).

#include "../common/JoinLink.hxx"
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
}
#endif

int main() {
	TestCodesNormalizeLikeTheHelper();
	TestOnlyTheJoinLinkIsAccepted();
	TestTheJoinScreenGetsTheShortLink();
#ifdef _WIN32
	TestARunningGameReceivesTheLink();
#endif
	std::printf("join_link_test: all tests passed\n");
	return 0;
}
