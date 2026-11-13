#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The public replay archive as the game's own browser reads it
// (docs/design/REPLAY_ARCHIVE.md, "Listing"): the filter a player sets, the
// games and tournament sets a page of the bridge's listing holds, and what the
// Archive tab of Replays reads of the listing so far (Status, ShellView::archiveBrowse).
// The helper's archive_list answer is decoded into these (session::DecodeArchivePage);
// ArchiveRequests.hxx decides what each answer does to the Status.
namespace sf4e { namespace netplay { namespace archive {

// The helper's code for a service that does not offer an archive.
constexpr const char* Unsupported = "replays_unsupported";

// The order a listing runs in: newest first, or the higher class of the two
// sides first (newest first within a class).
enum class Order : std::uint8_t { Newest, Class };

// The rounds the listing can be narrowed to, as the bridge names them.
constexpr std::size_t RoundCount = 5;
constexpr const char* const Rounds[RoundCount] = {"top8", "pools", "winners", "losers", "grand"};
// The event a filter names to list games of no tournament set.
constexpr const char* OnlineEvent = "online";
// The longest text a filter or a command holds (ember_protocol::archive::MAX_QUERY, MAX_URL), and a service's id.
constexpr std::size_t MaxText = 128, MaxUrl = 512, MaxBridge = 256;
// What a page holds at most, as ember_protocol::archive limits it, and the longest name or title the game keeps of one.
constexpr std::size_t MaxGames = 400, MaxEvents = 100, MaxCountries = 300, MaxCursor = 200, MaxShown = 96;

// What the browser asks the listing for: each field one value, all of them
// together. An empty text, no fighter (-1) and class 0 are no narrowing.
struct Filter {
	// Part of a player's name or an event's title.
	std::string text;
	int fighter = -1;
	// A class (band 1 to 10) either side has.
	int band = 0;
	// A country's two upper-case letters.
	std::string country;
	// An event's title, or OnlineEvent.
	std::string event;
	// One of Rounds.
	std::string round;
	Order order = Order::Newest;
	// A side's exact name: the sets of one player, as a profile lists them
	// (docs/design/PROFILE.md, "Sets by name"). Sent as `name` with `text`
	// beside it (or the name itself as `text` when none is typed), and the
	// games kept are those with a side of this name (KeepNamed), so a bridge
	// that ignores `name` lists the same sets.
	std::string name;

	// Whether anything but the order narrows the listing.
	bool Active() const { return ActiveBeyondName() || !name.empty(); }
	// The same, not counting `name`: whether a profile's list has anything to clear.
	bool ActiveBeyondName() const { return !text.empty() || fighter >= 0 || band != 0 || !country.empty() || !event.empty() || !round.empty(); }
	bool operator==(const Filter& other) const {
		return text == other.text && fighter == other.fighter && band == other.band && country == other.country && event == other.event &&
			round == other.round && order == other.order && name == other.name;
	}
	bool operator!=(const Filter& other) const { return !(*this == other); }
	// The sets of one player, by their exact name.
	static Filter Named(std::string player) { Filter filter; filter.name = std::move(player); return filter; }
	// The bounds a command carrying it keeps.
	bool Valid() const {
		return text.size() <= MaxText && name.size() <= MaxText && event.size() <= MaxText && country.size() <= 2 && round.size() <= 16 && band >= 0 && band <= 10;
	}
	std::size_t Bytes() const { return text.size() + name.size() + event.size() + country.size() + round.size(); }
	// The same narrowing in the other order.
	Filter Ordered(Order next) const { Filter copy = *this; copy.order = next; return copy; }
};

// One listed game. A side nobody named has an empty name; fighters are native
// ids, -1 when unknown; classes are bands 1 to 10, 0 for none.
struct Game {
	std::string digest, crc, url;
	std::uint64_t savedAt = 0, size = 0;
	std::array<int, 2> fighters{{-1, -1}};
	std::array<std::string, 2> names, countries;
	std::array<int, 2> classes{{0, 0}};
	// The tournament set it was a game of, and that set's event, round and phase; empty for an online game.
	std::string set, event, round, phase;
};

// A tournament set the listing's games name: its score by wins, and its pack's address once the bridge holds one.
struct SetInfo {
	std::array<unsigned, 2> score{{0, 0}};
	std::string pack;
};

// The listing as the pages so far hold it, in the order the bridge sent.
struct Listing {
	std::vector<Game> games;
	std::map<std::string, SetInfo> sets;
	// The filter's choices: events newest first and countries as their codes.
	std::vector<std::string> events, countries;
	// Every game the archive holds, and the games the filter keeps (not known for a filter too wide to count).
	std::optional<std::uint64_t> total, matching;
	// The order the bridge used, which is the newest while it hides classes.
	Order order = Order::Newest;
	// Where the next page starts; empty when this is the last.
	std::string next;
};

// Keeps the games of `page` in which a side's name is exactly `name`, in their
// order, and returns how many it dropped. A bridge that lists by name sends
// only such games and drops none; one that ignores `name` (an older one) sends
// the games of the other filters, and the rest go here. When any went, the
// page's count of matching games was the bridge's for a wider filter, so it is
// not kept. An empty `name` keeps everything.
inline std::size_t KeepNamed(Listing& page, const std::string& name) {
	if (name.empty()) return 0;
	const auto before = page.games.size();
	page.games.erase(std::remove_if(page.games.begin(), page.games.end(),
		[&](const Game& game) { return game.names[0] != name && game.names[1] != name; }), page.games.end());
	const auto dropped = before - page.games.size();
	if (dropped) page.matching.reset();
	return dropped;
}

// What the tab reads: where the listing of `filter` on `bridge` stands.
// Idle until a first page is asked for. Unsupported: the service offers no
// archive. The listing keeps what an earlier page held while a later one loads or fails.
struct Status {
	enum class State : std::uint8_t { Idle, Loading, Listed, Failed, Unsupported } state = State::Idle;
	std::string bridge;
	Filter filter;
	// The helper's code when the first page failed.
	std::string error;
	// A page after the first is on its way, or failed (the games so far stay).
	bool loadingMore = false, moreFailed = false;
	std::shared_ptr<const Listing> listing;
	// Moves each time games are installed, so a reader regroups only then.
	std::uint64_t revision = 0;
	// The game being downloaded to watch (its digest), the helper's code when the last download failed,
	// and how many downloads have ended, answered or not.
	std::string fetching, fetchError;
	std::uint64_t fetched = 0;
	bool Loaded() const { return state == State::Listed && listing; }
	// Whether this is the listing of `filter` on `bridge`, heard or on its way.
	bool Answers(const std::string& wanted, const Filter& asked) const { return state != State::Idle && bridge == wanted && filter == asked; }
};

} } }
