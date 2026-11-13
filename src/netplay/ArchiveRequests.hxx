#pragma once
// The runtime's half of the archive browser: which page of the listing is on
// its way, and what its answer does to the Status the interface reads
// (Archive.hxx). The runtime sends the requests and decodes the answers; this
// decides, so the rules are tested without a helper (archive_requests_test.cxx).
//
// - A filter's first page replaces the listing; each next page adds its games
//   to it. Every request gets a number, and only the answer to the newest one
//   counts: a first page asked for while another page is on its way retires
//   it, and its late answer is dropped.
// - A next page is asked for only once the first has been heard, from the
//   cursor the last page gave, and never twice at once. A next page that
//   fails keeps the games so far and is asked again only when the player says so.
// - A bridge that offers no archive says so (Unsupported) and keeps no games.
// - A game's download (fetching) is separate from the listing: one at a time,
//   and its end is counted whether it was answered or lost.
#include "Archive.hxx"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace sf4e { namespace netplay { namespace archive {

class Requests {
public:
	// Pages that added no game before more are no longer asked for by themselves:
	// a filter the bridge could only answer by looking through many games.
	static constexpr unsigned MostEmptyPages = 25;

	const Status& Published() const { return status_; }

	// A request number, 0 for none.
	using Ticket = std::uint64_t;

	// The first page of `filter` on `bridge`: what was listed is forgotten, and a page on its way is retired.
	Ticket Begin(const std::string& bridge, const Filter& filter) {
		Status fresh;
		fresh.state = Status::State::Loading;
		fresh.bridge = bridge;
		fresh.filter = filter;
		fresh.revision = status_.revision + 1;
		fresh.fetching = status_.fetching;
		fresh.fetchError = status_.fetchError;
		fresh.fetched = status_.fetched;
		status_ = std::move(fresh);
		kind_ = Kind::First;
		emptyPages_ = 0;
		return ++ticket_;
	}

	// The cursor the next page is asked with, or empty when there is none to ask for now: the first page
	// is not heard yet, the last page was the last, a page is on its way, or the last next page failed
	// and `again` (the player's Retry) is not set.
	std::string Cursor(bool again = false) const {
		if (!status_.Loaded() || kind_ != Kind::None || status_.listing->next.empty()) return {};
		if (status_.moreFailed && !again) return {};
		if (emptyPages_ >= MostEmptyPages && !again) return {};
		return status_.listing->next;
	}

	// The next page of the filter on its way: its number, 0 when Cursor says there is none to ask for.
	Ticket BeginNext(bool again = false) {
		if (Cursor(again).empty()) return 0;
		status_.loadingMore = true;
		status_.moreFailed = false;
		if (again) emptyPages_ = 0;
		kind_ = Kind::Next;
		return ++ticket_;
	}

	// The answer to request `ticket`: the page, or why there is none (the helper's code, or
	// "bridge_invalid_response"). False when it was retired, which changes nothing.
	bool Finish(Ticket ticket, const std::string& failure, std::optional<Listing> page) {
		if (!ticket || ticket != ticket_ || kind_ == Kind::None) return false;
		const bool first = kind_ == Kind::First;
		kind_ = Kind::None;
		if (!failure.empty() || !page) {
			if (first) {
				status_.state = failure == Unsupported ? Status::State::Unsupported : Status::State::Failed;
				status_.error = failure.empty() ? "bridge_invalid_response" : failure;
				status_.listing.reset();
				++status_.revision;
			} else {
				status_.loadingMore = false;
				status_.moreFailed = true;
			}
			return true;
		}
		if (first) {
			status_.listing = std::make_shared<const Listing>(std::move(*page));
			status_.state = Status::State::Listed;
			status_.error.clear();
			emptyPages_ = status_.listing->games.empty() ? 1 : 0;
		} else {
			auto merged = std::make_shared<Listing>(*status_.listing);
			// A game whose class moved while the player paged can be listed twice: it is kept once.
			std::unordered_set<std::string> held;
			for (const auto& game : merged->games) held.insert(game.digest);
			std::size_t added = 0;
			for (auto& game : page->games)
				if (held.insert(game.digest).second) { merged->games.push_back(std::move(game)); ++added; }
			for (auto& set : page->sets) merged->sets[set.first] = std::move(set.second);
			merged->events = std::move(page->events);
			merged->countries = std::move(page->countries);
			merged->total = page->total;
			merged->matching = page->matching;
			merged->order = page->order;
			merged->next = std::move(page->next);
			status_.listing = std::move(merged);
			status_.loadingMore = false;
			status_.moreFailed = false;
			emptyPages_ = added ? 0 : emptyPages_ + 1;
		}
		++status_.revision;
		return true;
	}

	// A game's download for the player to watch, `digest`, handed to the helper. One at a time: false while another is on its way.
	bool BeginFetch(const std::string& digest) {
		if (!status_.fetching.empty() || digest.empty()) return false;
		status_.fetching = digest;
		status_.fetchError.clear();
		return true;
	}
	// The download's end: the helper's code when it failed, empty when the game is on disk.
	void FinishFetch(const std::string& failure) {
		status_.fetching.clear();
		status_.fetchError = failure;
		++status_.fetched;
	}

private:
	enum class Kind { None, First, Next };
	Status status_;
	Kind kind_ = Kind::None;
	Ticket ticket_ = 0;
	unsigned emptyPages_ = 0;
};

} } }
