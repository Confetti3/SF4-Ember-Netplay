// The archive browser's requests (netplay/ArchiveRequests.hxx): a first page, the next pages added to it, a
// filter changed while a page is on its way (the stale answer is dropped), failures, and the game download;
// when the Archive tab asks for a page (ui/ArchivePaging.hxx); and how the helper's archive_list answer is
// read (session::DecodeArchivePage).
#include "../netplay/ArchiveRequests.hxx"
#include "../session/TournamentAnswers.hxx"
#include "../ui/ArchivePaging.hxx"

#include <cstdio>
#include <cstdlib>
#include "test_support.hxx"

using namespace sf4e;
namespace ar = netplay::archive;
using ui::archivepaging::Ask;
using nlohmann::json;
using State = ar::Status::State;

static std::string Digest(unsigned n) {
	char text[80];
	std::snprintf(text, sizeof(text), "%064x", n);
	return text;
}

// A page of `count` games with digests from `first` up, listed newest first, going on from `next`.
static ar::Listing Page(unsigned first, unsigned count, const std::string& next) {
	ar::Listing page;
	for (unsigned i = 0; i < count; ++i) {
		ar::Game game;
		game.digest = Digest(first + i);
		game.url = "https://archive.example/games/" + game.digest + ".emberreplay";
		game.savedAt = 2000000000 - first - i;
		game.names[0] = "Player 1";
		game.names[1] = "Player 2";
		page.games.push_back(game);
	}
	page.next = next;
	page.events = {"Player 1's Cup"};
	page.countries = {"BE", "GB"};
	page.total = 900;
	page.matching = 800;
	return page;
}

static ar::Filter Named(const char* text) {
	ar::Filter filter;
	filter.text = text;
	return filter;
}

// The first page replaces what was listed; each next page is asked for from the cursor the last gave, once at a time.
static void FirstThenNextPages() {
	ar::Requests requests;
	EXPECT(requests.Published().state == State::Idle && requests.Cursor().empty() && requests.BeginNext() == 0);
	const auto first = requests.Begin("brg_1", Named("Player 1"));
	EXPECT(first && requests.Published().state == State::Loading && requests.Published().bridge == "brg_1" && requests.Published().filter == Named("Player 1"));
	// Nothing to go on from before the first page is heard.
	EXPECT(requests.Cursor().empty() && requests.BeginNext() == 0);
	EXPECT(requests.Finish(first, "", Page(0, 3, "10.ab")));
	const auto& heard = requests.Published();
	EXPECT(heard.state == State::Listed && heard.Loaded() && heard.listing->games.size() == 3 && heard.listing->next == "10.ab");
	EXPECT(heard.listing->events.size() == 1 && heard.listing->countries.size() == 2 && *heard.listing->matching == 800);
	EXPECT(requests.Cursor() == "10.ab");
	const auto before = heard.listing;
	const auto second = requests.BeginNext();
	EXPECT(second && second != first && requests.Published().loadingMore && requests.Cursor().empty() && requests.BeginNext() == 0);
	const auto revision = requests.Published().revision;
	// A game the next page repeats (a game whose class moved while the player paged) is kept once.
	EXPECT(requests.Finish(second, "", Page(2, 3, "")));
	const auto& more = requests.Published();
	EXPECT(!more.loadingMore && !more.moreFailed && more.listing->games.size() == 5 && more.listing->next.empty() && more.revision > revision);
	EXPECT(more.listing->games[2].digest == Digest(2) && more.listing->games[4].digest == Digest(4));
	// The last page was the last.
	EXPECT(requests.Cursor().empty() && requests.BeginNext() == 0);
	// The listing a reader holds is not changed by the page added to it: a new one is installed.
	EXPECT(before->games.size() == 3 && before->next == "10.ab");
}

// A filter changed while a page is in flight retires it: its late answer changes nothing.
static void StaleAnswersAreDropped() {
	ar::Requests requests;
	const auto old = requests.Begin("brg_1", Named("Player 1"));
	const auto fresh = requests.Begin("brg_1", Named("Player 2"));
	EXPECT(old != fresh && requests.Published().filter == Named("Player 2"));
	EXPECT(!requests.Finish(old, "", Page(0, 2, "")));
	EXPECT(requests.Published().state == State::Loading && !requests.Published().listing && requests.Published().filter == Named("Player 2"));
	EXPECT(requests.Finish(fresh, "", Page(10, 1, "")));
	EXPECT(requests.Published().listing->games.size() == 1 && requests.Published().listing->games[0].digest == Digest(10));
	// An answer comes once.
	EXPECT(!requests.Finish(fresh, "", Page(20, 1, "")) && requests.Published().listing->games.size() == 1);
	EXPECT(!requests.Finish(0, "", Page(20, 1, "")));

	// A next page in flight when the filter changes: its answer is the old filter's and is dropped.
	ar::Requests paging;
	const auto head = paging.Begin("brg_1", Named("Player 1"));
	paging.Finish(head, "", Page(0, 2, "5.ab"));
	const auto next = paging.BeginNext();
	const auto changed = paging.Begin("brg_1", Named("Player 3"));
	EXPECT(next && changed && paging.Published().state == State::Loading && !paging.Published().loadingMore && !paging.Published().listing);
	EXPECT(!paging.Finish(next, "", Page(2, 2, "")));
	EXPECT(paging.Published().state == State::Loading && !paging.Published().listing);
	EXPECT(paging.Finish(changed, "", Page(50, 1, "")) && paging.Published().listing->games.size() == 1);
	// A failure that arrives late is dropped too.
	const auto again = paging.Begin("brg_1", Named("Player 4"));
	EXPECT(!paging.Finish(changed, "unavailable", std::nullopt) && paging.Published().state == State::Loading);
	EXPECT(paging.Finish(again, "unavailable", std::nullopt) && paging.Published().state == State::Failed);
}

// A first page that fails says why; a service without an archive is told apart; a next page that fails keeps the games.
static void Failures() {
	ar::Requests requests;
	auto ticket = requests.Begin("brg_1", {});
	requests.Finish(ticket, "bridge_unreachable", std::nullopt);
	EXPECT(requests.Published().state == State::Failed && requests.Published().error == "bridge_unreachable" && !requests.Published().listing);
	ticket = requests.Begin("brg_1", {});
	requests.Finish(ticket, ar::Unsupported, std::nullopt);
	EXPECT(requests.Published().state == State::Unsupported && requests.Published().error == ar::Unsupported);
	// An answer that was not a listing at all.
	ticket = requests.Begin("brg_1", {});
	requests.Finish(ticket, "", std::nullopt);
	EXPECT(requests.Published().state == State::Failed && requests.Published().error == "bridge_invalid_response");
	// A new filter starts clean.
	ticket = requests.Begin("brg_1", {});
	EXPECT(requests.Published().state == State::Loading && requests.Published().error.empty());

	requests.Finish(ticket, "", Page(0, 4, "8.ab"));
	const auto next = requests.BeginNext();
	requests.Finish(next, "rate_limited", std::nullopt);
	const auto& kept = requests.Published();
	EXPECT(kept.state == State::Listed && kept.listing->games.size() == 4 && !kept.loadingMore && kept.moreFailed);
	// It is not asked again by itself; the player's Retry asks again.
	EXPECT(requests.Cursor().empty() && requests.BeginNext() == 0);
	EXPECT(requests.Cursor(true) == "8.ab");
	const auto retry = requests.BeginNext(true);
	EXPECT(retry && !requests.Published().moreFailed && requests.Published().loadingMore);
	requests.Finish(retry, "", Page(4, 1, ""));
	EXPECT(requests.Published().listing->games.size() == 5 && !requests.Published().moreFailed);
}

// A filter the bridge can only answer by looking through many games lists none for page after page: those are
// not asked for by themselves without end.
static void EmptyPagesStop() {
	ar::Requests requests;
	requests.Finish(requests.Begin("brg_1", {}), "", Page(0, 0, "3.ab"));
	for (unsigned i = 1; i < ar::Requests::MostEmptyPages; ++i) {
		const auto ticket = requests.BeginNext();
		EXPECT_MSG(ticket != 0, "page %u was not asked for", i);
		requests.Finish(ticket, "", Page(0, 0, "3.ab"));
	}
	EXPECT(requests.Cursor().empty() && requests.BeginNext() == 0);
	// Retry asks again, and a page with a game counts afresh.
	const auto again = requests.BeginNext(true);
	EXPECT(again != 0);
	requests.Finish(again, "", Page(0, 1, "4.ab"));
	EXPECT(requests.Cursor() == "4.ab");
}

// The download of a game to watch: one at a time, and its end is counted whether it was answered or lost.
static void GameDownloads() {
	ar::Requests requests;
	EXPECT(!requests.BeginFetch(""));
	EXPECT(requests.BeginFetch(Digest(1)) && requests.Published().fetching == Digest(1));
	EXPECT(!requests.BeginFetch(Digest(2)));
	requests.FinishFetch("");
	EXPECT(requests.Published().fetching.empty() && requests.Published().fetchError.empty() && requests.Published().fetched == 1);
	EXPECT(requests.BeginFetch(Digest(2)));
	requests.FinishFetch("http_404");
	EXPECT(requests.Published().fetchError == "http_404" && requests.Published().fetched == 2);
	// A listing asked for meanwhile does not forget how the downloads stand.
	EXPECT(requests.BeginFetch(Digest(3)));
	requests.Begin("brg_1", {});
	EXPECT(requests.Published().fetching == Digest(3) && requests.Published().fetched == 2);
	requests.FinishFetch("");
	EXPECT(requests.Published().fetched == 3 && requests.Published().fetchError.empty());
	EXPECT(requests.BeginFetch(Digest(4)) && requests.Published().fetchError.empty());
}

// The tab asks for the first page of its filter on its service once; for the next page once its end is near;
// for the first page again when the filter changes, or on Retry; and never while nothing is set up.
static void WhenThePagesAreAsked() {
	ar::Requests requests;
	ui::archivepaging::Memory memory;
	const auto filter = Named("Player 1");
	const auto ask = [&](const ar::Filter& wanted, bool nearEnd, bool retry = false, const std::string& bridge = "brg_1") {
		return ui::archivepaging::NextAsk(requests.Published(), bridge, wanted, nearEnd, retry, memory);
	};
	// The runtime takes the command a frame later: until its status shows the filter, it is not asked again.
	EXPECT(ask(filter, false, false, "") == Ask::None);
	EXPECT(ask(filter, false) == Ask::First && ask(filter, false) == Ask::None && ask(filter, true) == Ask::None);
	auto ticket = requests.Begin("brg_1", filter);
	EXPECT(ask(filter, true) == Ask::None);
	requests.Finish(ticket, "", Page(0, 5, "5.ab"));
	// The next page waits for the end of the list, and is asked for once.
	EXPECT(ask(filter, false) == Ask::None);
	EXPECT(ask(filter, true) == Ask::Next && ask(filter, true) == Ask::None);
	ticket = requests.BeginNext();
	EXPECT(ask(filter, true) == Ask::None);
	requests.Finish(ticket, "", Page(5, 5, "10.ab"));
	EXPECT(ask(filter, true) == Ask::Next);
	// A filter changed while the page is on its way asks for the first page of the new one at once.
	ticket = requests.BeginNext();
	const auto changed = Named("Player 2");
	EXPECT(ask(changed, true) == Ask::First);
	const auto first = requests.Begin("brg_1", changed);
	EXPECT(!requests.Finish(ticket, "", Page(10, 5, "")) && requests.Published().filter == changed);
	EXPECT(ask(changed, true) == Ask::None);
	requests.Finish(first, "", Page(100, 2, ""));
	EXPECT(ask(changed, true) == Ask::None && ask(changed, true, true) == Ask::First);
	// A failed page: Retry asks again; nothing does without it.
	ticket = requests.Begin("brg_1", changed);
	requests.Finish(ticket, "unavailable", std::nullopt);
	EXPECT(ask(changed, true) == Ask::None && ask(changed, true, true) == Ask::First);
	ticket = requests.Begin("brg_1", changed);
	requests.Finish(ticket, "", Page(0, 5, "5.ab"));
	ticket = requests.BeginNext();
	requests.Finish(ticket, "unavailable", std::nullopt);
	EXPECT(ask(changed, true) == Ask::None && ask(changed, true, true) == Ask::NextAgain);
	// A service the player did not have before is another listing.
	EXPECT(ask(changed, false, false, "brg_2") == Ask::First);
	// A queue that refused the command is asked again: the tab forgets what it asked.
	memory = ui::archivepaging::Memory();
	EXPECT(ask(Named("Player 5"), false) == Ask::First);

	// The end of the list is near for the last few rows, or any of a short list.
	using ui::archivepaging::NearEnd;
	EXPECT(NearEnd(0, 3) && NearEnd(-1, 6) && !NearEnd(-1, 40) && !NearEnd(10, 40) && NearEnd(34, 40) && NearEnd(39, 40));
}

static json GameJson(const std::string& digest) {
	return {{"digest", digest}, {"crc", "0badf00d"}, {"url", "https://archive.example/games/" + digest + ".emberreplay"}, {"saved_at", 1700000000u}, {"size", 4096u},
		{"fighters", {0, 17}}, {"names", {"Player 1", "Player 2"}}, {"countries", {"GB", ""}}, {"classes", {7, 0}}};
}

static void ReadingTheAnswer() {
	// Everything the page carries.
	json full = {{"games", json::array({GameJson(Digest(1)), GameJson(Digest(2))})}, {"sets", {{"emt_a", {{"score", {2, 1}}, {"pack", "https://archive.example/sets/emt_a.emberpack"}}}}},
		{"events", {"Player 1's Cup"}}, {"countries", {"BE", "GB"}}, {"total", 120u}, {"matching", 2u}, {"order", "class"}, {"more", true}, {"next", "9.1700000000.ab"}};
	full["games"][0]["set"] = "emt_a";
	full["games"][0]["event"] = "Player 1's Cup";
	full["games"][0]["round"] = "Top 8";
	full["games"][0]["phase"] = "Winners";
	auto page = session::DecodeArchivePage(full);
	CHECK(page);
	EXPECT(page->games.size() == 2 && page->games[0].digest == Digest(1) && page->games[0].set == "emt_a" && page->games[0].round == "Top 8" && page->games[0].phase == "Winners");
	EXPECT(page->games[0].fighters[1] == 17 && page->games[0].classes[0] == 7 && page->games[0].countries[0] == "GB" && page->games[0].countries[1].empty());
	EXPECT(page->games[0].names[0] == "Player 1" && page->games[0].savedAt == 1700000000u && page->games[0].size == 4096u);
	EXPECT(page->sets.size() == 1 && page->sets["emt_a"].score[0] == 2 && page->sets["emt_a"].score[1] == 1 && !page->sets["emt_a"].pack.empty());
	EXPECT(page->events.size() == 1 && page->countries.size() == 2 && *page->total == 120 && *page->matching == 2);
	EXPECT(page->order == ar::Order::Class && page->next == "9.1700000000.ab");

	// The least a page holds.
	page = session::DecodeArchivePage({{"games", json::array()}});
	CHECK(page);
	EXPECT(page->games.empty() && page->sets.empty() && page->events.empty() && !page->total && !page->matching && page->next.empty() && page->order == ar::Order::Newest);
	json minimal = {{"games", json::array({json{{"digest", Digest(3)}, {"url", "https://archive.example/g"}}})}};
	page = session::DecodeArchivePage(minimal);
	CHECK(page && page->games.size() == 1);
	EXPECT(page->games[0].fighters[0] == -1 && page->games[0].classes[1] == 0 && page->games[0].names[0].empty() && page->games[0].savedAt == 0);

	// Members this build does not know are ignored, and values it does not know are none.
	json odd = full;
	odd["banner"] = "hello";
	odd["order"] = "sideways";
	odd["next"] = "not a cursor";
	odd["games"][0]["tags"] = {"a"};
	odd["games"][0]["classes"] = {11, 3};
	odd["games"][0]["fighters"] = {300, 2};
	odd["games"][0]["countries"] = {"gb", 5};
	page = session::DecodeArchivePage(odd);
	CHECK(page && page->games.size() == 2);
	EXPECT(page->order == ar::Order::Newest && page->next.empty() && page->games[0].classes[0] == 0 && page->games[0].classes[1] == 3);
	EXPECT(page->games[0].fighters[0] == -1 && page->games[0].fighters[1] == 2 && page->games[0].countries[0].empty());

	// A game that cannot be fetched is left out; the others stay.
	json broken = {{"games", json::array({json{{"digest", "short"}, {"url", "u"}}, json{{"url", "u"}}, "text", GameJson(Digest(4))})}};
	page = session::DecodeArchivePage(broken);
	CHECK(page);
	EXPECT(page->games.size() == 1 && page->games[0].digest == Digest(4));

	// Not a listing at all.
	EXPECT(!session::DecodeArchivePage(json::object()) && !session::DecodeArchivePage(json::array()) && !session::DecodeArchivePage({{"games", "none"}}));

	// A page holds at most 400 games, and its text is one short line.
	json many = {{"games", json::array()}};
	for (unsigned i = 0; i < 420; ++i) many["games"].push_back(GameJson(Digest(i)));
	many["games"][0]["names"] = {std::string(500, 'n'), "A\nB"};
	page = session::DecodeArchivePage(many);
	CHECK(page);
	EXPECT(page->games.size() == 400 && page->games[0].names[0].size() == 96 && page->games[0].names[1] == "A B");

	// Its answers reach the runtime as tournament answers.
	for (const char* op : {"archive_list", "archive_fetch"})
		EXPECT(session::ReadTournamentAnswer({{"op", op}, {"request_id", 7u}, {"ok", true}, {"data", json::object()}}).has_value());
}

int main() {
	FirstThenNextPages();
	StaleAnswersAreDropped();
	Failures();
	EmptyPagesStop();
	GameDownloads();
	WhenThePagesAreAsked();
	ReadingTheAnswer();
	if (TEST_FAILURES()) { std::printf("%d failed\n", TEST_FAILURES()); return 1; }
	std::printf("ok\n");
	return 0;
}
