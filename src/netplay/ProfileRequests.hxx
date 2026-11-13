#pragma once
// The runtime's half of other players' profiles and of Find a player: which
// look-up or search is on its way for the profile open and for the search the
// player typed, and what each answer does to what the interface reads. The
// runtime sends the requests and decodes the answers; this decides, so the
// rules are tested without a helper (card_requests_test.cxx).
// docs/design/PROFILE.md, "Another player's profile" and "Find a player".
//
// - One profile is open at a time: opening another replaces it, and the answer
//   to a request made for the one before is dropped (every request has a
//   ticket, and only the newest of each slot counts).
// - A profile opened by a name is searched first (the exact name, in any case)
//   and the one player found is looked up; none is "Nobody", several are
//   "Several" (the page asks which), unless the opener's country settles it.
//   Any other key is looked up at once.
// - The card on show stays while a refresh is awaited, and a refresh that
//   fails keeps it. A refresh comes 8 s after a set in the room ends, and
//   after a friends action went through (Refresh), at least MinRefreshMs
//   after the last look-up; the card the action named shows what it made of
//   them at once (Relate).
// - The Find a player search is its own slot: a newer text (or kind) drops the
//   older answer, and an empty text clears it. The same text asked again while
//   it is in flight is not asked twice.
// - A service that says it has no profiles is remembered for RecheckMs and its
//   name, request and code openers and its search are answered at once as
//   unsupported, without asking; then it is asked again, so an upgraded one is heard.
// - A new service or Ember ID forgets everything.
#include "Answered.hxx"
#include "Levels.hxx"
#include "Profiles.hxx"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay { namespace profiles {

// Whether the service offers profiles, as far as its answers have said. Missing: it said it has none (the helper's
// `profiles_unavailable`); a screen then offers no name openers and no search by name, only tokens and ids.
enum class Service : std::uint8_t { Unknown, Offered, Missing };

// The profile open, as the interface reads it.
struct Profile {
	enum class State : std::uint8_t {
		// No profile is open.
		Closed,
		// The look-up (or, for a name, the search before it) is on its way. `known` is all there is yet.
		Reading,
		// `card.value` is the card (`card.loaded`).
		Card,
		// The service answers there is no card for it: no Discord account, an expired token, or a player who blocked
		// the viewer. The page never says which.
		NoCard,
		// A name no player goes by now.
		Nobody,
		// More than one player goes by the name; `hits` lists them, newest active first.
		Several,
		// The service could not be reached or its answer could not be read; `error` is the helper's code.
		Unreachable,
		// The service cannot answer this kind of opener (no profiles, or no points at all); `error` is the code.
		Unsupported,
	} state = State::Closed;
	// How it was opened and what the opener knew; `known` gains what a search hit or the card tells.
	Key key;
	Known known;
	// The card token this profile can be acted on by (a friend request, a block): the opener's, or the search
	// hit's. Empty for a profile opened by friend id, request id or code, which act by those.
	std::string token;
	// The card. loaded: `value` is the card on show; loading: a look-up (a first or a refresh) is on its way;
	// error: the code of the last look-up that failed (a card on show stays); finished and revision move as
	// everywhere (Answered.hxx), and installedMs is the runtime's clock when the card arrived.
	Answered<levels::Card> card;
	// Several: the players going by the name, as the search listed them.
	std::vector<Hit> hits;
	// The helper's code for Unreachable and Unsupported, empty otherwise.
	std::string error;
	// Moves with each profile opened (not with a refresh), so a page knows its profile was replaced.
	std::uint64_t opened = 0;
	bool Open() const { return state != State::Closed; }
};

// The Find a player search the player last typed, and its answer.
struct SearchView {
	// The text asked about, and whether it was asked as a friend code. Empty with no search.
	std::string text;
	bool byCode = false;
	// loading: on its way for `text`. loaded: `value` answers `text` (an empty list is "nobody goes by that name").
	// error: the helper's code when it failed (rate_limited, no_discord_account, profiles_unavailable ...).
	Answered<SearchAnswer> answer;
};

// A look-up's answer as the runtime read it: nothing read (`read` false), no card for the selector, or the card.
struct Looked {
	bool read = false;
	std::optional<levels::Card> card;
};

class Requests {
public:
	using Ticket = std::uint64_t;
	// A look-up after a friends action or a set is at least this long after the last one, and a set's comes this long after it ends.
	static constexpr std::uint64_t MinRefreshMs = 5000, SetEndedMs = 8000;
	// How long a service that said it has no profiles is believed.
	static constexpr std::uint64_t RecheckMs = 600000;
	// The helper's code for a service that has no levels at all, as Cards has it.
	static constexpr const char* NoPoints = "points_unavailable";

	// What the runtime sends next: nothing, a cards_search (a Name, or a Code, in `value`) or a cards_lookup
	// by `selector` (Card, Friend, Request or Code). `ticket` is what the answer is returned under.
	struct Ask {
		enum class Kind : std::uint8_t { None, Search, Lookup } kind = Kind::None;
		Ticket ticket = 0;
		Key::Kind selector = Key::Kind::None;
		std::string value;
		explicit operator bool() const { return kind != Kind::None; }
	};

	Profile Published() const { return profile_; }
	SearchView SearchPublished() const { return search_; }
	Service Offering() const { return service_; }
	const std::string& Bridge() const { return bridge_; }
	const std::string& EmberId() const { return emberId_; }

	// The service and Ember ID the requests are for; another one forgets everything.
	void Scope(const std::string& bridge, const std::string& emberId) {
		if (bridge == bridge_ && emberId == emberId_) return;
		bridge_ = bridge;
		emberId_ = emberId;
		Forget();
	}

	// Believes a service has no profiles, or has them, from what an answer said (a card save or a search
	// as much as a look-up).
	void NoteService(Service service, std::uint64_t nowMs) {
		service_ = service;
		if (service == Service::Missing) missingMs_ = nowMs;
	}

	// Asks again of a service that said it has no profiles, once RecheckMs have passed.
	void Tick(std::uint64_t nowMs) {
		if (service_ == Service::Missing && nowMs - missingMs_ >= RecheckMs) service_ = Service::Unknown;
	}

	// ---- The profile ----

	// Opens `key` (replacing the profile open), knowing what `known` says. The same key opened again while its
	// card is on show only refreshes it.
	void Open(const Key& key, const Known& known, std::uint64_t nowMs) {
		if (!key.Present()) return;
		Tick(nowMs);
		if (profile_.state == Profile::State::Card && profile_.key == key) {
			profile_.known = known;
			stale_ = true;
			return;
		}
		Profile fresh;
		fresh.key = key;
		fresh.known = known;
		fresh.opened = ++opened_;
		fresh.card.finished = profile_.card.finished;
		fresh.card.revision = profile_.card.revision;
		fresh.card.loading = true;
		fresh.state = Profile::State::Reading;
		fresh.token = key.kind == Key::Kind::Card ? key.value : std::string();
		profile_ = std::move(fresh);
		profileTicket_ = 0;
		stale_ = overtaken_ = sentOnce_ = false;
		resolved_ = key.kind == Key::Kind::Name ? Key() : key;
		const bool needsProfiles = key.kind == Key::Kind::Name || key.kind == Key::Kind::Request || key.kind == Key::Kind::Code;
		if (needsProfiles && service_ == Service::Missing) {
			Fail(Profile::State::Unsupported, Unavailable);
			return;
		}
		stage_ = key.kind == Key::Kind::Name ? Stage::WantSearch : Stage::WantLookup;
	}

	// The profile is closed: its late answers are dropped.
	void Close() {
		const auto opened = profile_.opened;
		const auto finished = profile_.card.finished, revision = profile_.card.revision;
		profile_ = Profile();
		profile_.opened = opened;
		profile_.card.finished = finished;
		profile_.card.revision = revision;
		profileTicket_ = 0;
		stage_ = Stage::Idle;
		stale_ = overtaken_ = false;
	}

	// A friends action went through: the card is looked up again, and a look-up in flight, asked before the action,
	// does not stand for the card as it is now.
	void Refresh() {
		if (profile_.state == Profile::State::Card || profile_.state == Profile::State::NoCard) stale_ = true;
		if (stage_ == Stage::Looking) overtaken_ = true;
	}

	// The action named the player whose card token is `token` (Add friend, Block): their card shows
	// `relation` until the look-up Refresh asked for answers. Nothing for a profile without a card on show.
	void Relate(const std::string& token, levels::Relation relation) {
		if (profile_.state != Profile::State::Card || token.empty() || token != profile_.token) return;
		// A request to a player who had asked first is accepted by the bridge: they are friends.
		if (relation == levels::Relation::Sent && profile_.card.value.relation == levels::Relation::Received) relation = levels::Relation::Friend;
		profile_.card.value.relation = relation;
		++profile_.card.revision;
		stale_ = true;
	}

	// The room as it is now: its epoch (0 when none, or not a public room) and the generation of each table's last
	// set. A set that ends in the room makes the card be looked up again SetEndedMs later (what a game between the
	// two changed). The first look at a room only notes what already ended.
	void Observe(std::uint64_t epoch, const std::vector<std::uint64_t>& sets, std::uint64_t nowMs) {
		if (epoch != epoch_) {
			epoch_ = epoch;
			sets_.clear();
			refreshAtMs_ = 0;
		}
		if (sets.size() == sets_.size())
			for (std::size_t table = 0; table < sets.size(); ++table)
				if (sets[table] && sets[table] != sets_[table]) refreshAtMs_ = nowMs + SetEndedMs;
		sets_ = sets;
		if (refreshAtMs_ && nowMs >= refreshAtMs_) {
			refreshAtMs_ = 0;
			Refresh();
		}
	}

	// ---- The search ----

	// Find a player's text: a name, or with `byCode` a friend code. An empty text clears the search. A newer text,
	// or the same text as the other kind, drops the answer of the one before.
	void Search(const std::string& text, bool byCode, std::uint64_t nowMs) {
		Tick(nowMs);
		if (text.empty()) {
			ClearSearch();
			return;
		}
		if (searchStage_ != SearchStage::Idle && search_.text == text && search_.byCode == byCode) return;
		SearchView fresh;
		fresh.text = text;
		fresh.byCode = byCode;
		fresh.answer.finished = search_.answer.finished;
		fresh.answer.revision = search_.answer.revision;
		search_ = std::move(fresh);
		searchTicket_ = 0;
		if (service_ == Service::Missing) {
			search_.answer.error = Unavailable;
			++search_.answer.finished;
			searchStage_ = SearchStage::Idle;
			return;
		}
		search_.answer.loading = true;
		searchStage_ = SearchStage::Want;
	}

	// ---- Sending and answering ----

	// The request to send now, if any: the profile's first, then the search's. It is in flight from here: its
	// answer comes back through FinishSearch or FinishLookup under the ticket, or a refusal ends it the same way.
	Ask Take(std::uint64_t nowMs) {
		if (stage_ == Stage::Done && stale_ && sentOnce_ && resolved_.Present() && nowMs - lastLookupMs_ >= MinRefreshMs) {
			stale_ = false;
			stage_ = Stage::WantLookup;
		}
		Ask ask;
		if (stage_ == Stage::WantSearch) {
			stage_ = Stage::Searching;
			ask.kind = Ask::Kind::Search;
			ask.selector = Key::Kind::Name;
			ask.value = profile_.key.value;
			ask.ticket = profileTicket_ = ++ticket_;
			return ask;
		}
		if (stage_ == Stage::WantLookup) {
			stage_ = Stage::Looking;
			sentOnce_ = true;
			lastLookupMs_ = nowMs;
			profile_.card.loading = true;
			ask.kind = Ask::Kind::Lookup;
			ask.selector = resolved_.kind;
			ask.value = resolved_.value;
			ask.ticket = profileTicket_ = ++ticket_;
			return ask;
		}
		if (searchStage_ == SearchStage::Want) {
			searchStage_ = SearchStage::Flight;
			ask.kind = Ask::Kind::Search;
			ask.selector = search_.byCode ? Key::Kind::Code : Key::Kind::Name;
			ask.value = search_.text;
			ask.ticket = searchTicket_ = ++ticket_;
		}
		return ask;
	}

	// A search's answer: the players, or why there are none (the helper's code, or "bridge_invalid_response").
	// False when it was retired (a newer search, another profile, a closed one), which changes nothing.
	bool FinishSearch(Ticket ticket, const std::string& failure, const std::optional<SearchAnswer>& answer, std::uint64_t nowMs) {
		if (!ticket) return false;
		const std::string code = !failure.empty() ? failure : answer ? std::string() : std::string("bridge_invalid_response");
		if (ticket == searchTicket_ && searchStage_ == SearchStage::Flight) {
			searchTicket_ = 0;
			searchStage_ = SearchStage::Idle;
			auto& heard = search_.answer;
			heard.loading = false;
			++heard.finished;
			heard.error = code;
			heard.loaded = code.empty();
			heard.value = code.empty() ? *answer : SearchAnswer();
			if (code.empty()) { ++heard.revision; heard.installedMs = nowMs; NoteService(Service::Offered, nowMs); }
			else if (code == Unavailable) NoteService(Service::Missing, nowMs);
			return true;
		}
		if (ticket == profileTicket_ && stage_ == Stage::Searching) {
			profileTicket_ = 0;
			if (!code.empty()) {
				if (code == Unavailable) NoteService(Service::Missing, nowMs);
				Fail(code == Unavailable ? Profile::State::Unsupported : Profile::State::Unreachable, code);
				return true;
			}
			NoteService(Service::Offered, nowMs);
			const auto named = NamedHits(answer->players, profile_.key.value);
			const int chosen = ChooseHit(named, profile_.key.country);
			if (named.empty()) {
				Settle(Profile::State::Nobody);
			} else if (chosen < 0) {
				profile_.hits = named;
				Settle(Profile::State::Several);
			} else {
				const auto& hit = named[static_cast<std::size_t>(chosen)];
				profile_.token = hit.card;
				Learn(hit);
				resolved_ = Key::OfCard(hit.card);
				stage_ = Stage::WantLookup;
			}
			return true;
		}
		return false;
	}

	// A look-up's answer. False when it was retired, which changes nothing.
	bool FinishLookup(Ticket ticket, const std::string& failure, const Looked& looked, std::uint64_t nowMs) {
		if (!ticket || ticket != profileTicket_ || stage_ != Stage::Looking) return false;
		profileTicket_ = 0;
		stage_ = Stage::Done;
		profile_.card.loading = false;
		const std::string code = !failure.empty() ? failure : looked.read ? std::string() : std::string("bridge_invalid_response");
		if (!code.empty()) {
			const bool noProfiles = code == Unavailable;
			if (noProfiles) NoteService(Service::Missing, nowMs);
			// A card on show stays; a refresh that failed is not the card gone.
			if (profile_.state == Profile::State::Card) { profile_.card.error = code; ++profile_.card.finished; }
			else Fail(noProfiles || code == NoPoints ? Profile::State::Unsupported : Profile::State::Unreachable, code);
			return true;
		}
		++profile_.card.finished;
		profile_.error.clear();
		profile_.card.error.clear();
		if (!looked.card) {
			profile_.state = Profile::State::NoCard;
			profile_.card.value = levels::Card();
			profile_.card.loaded = false;
			return true;
		}
		profile_.state = Profile::State::Card;
		profile_.card.value = *looked.card;
		profile_.card.loaded = true;
		profile_.card.installedMs = nowMs;
		++profile_.card.revision;
		if (looked.card->hasSince) NoteService(Service::Offered, nowMs);
		if (overtaken_) { stale_ = true; overtaken_ = false; }
		return true;
	}

private:
	enum class Stage : std::uint8_t { Idle, WantSearch, Searching, WantLookup, Looking, Done };
	enum class SearchStage : std::uint8_t { Idle, Want, Flight };

	// What the profile has no more to wait for: its state, with nothing in flight.
	void Settle(Profile::State state) {
		profile_.state = state;
		profile_.card.loading = false;
		++profile_.card.finished;
		profile_.error.clear();
		stage_ = Stage::Done;
	}

	void Fail(Profile::State state, const std::string& code) {
		Settle(state);
		profile_.error = code;
	}

	// What a search hit says of the player that the opener did not.
	void Learn(const Hit& hit) {
		auto& known = profile_.known;
		if (known.name.empty()) known.name = hit.name;
		if (!known.country.Present()) known.country = hit.country;
		if (!known.badge.Present()) known.badge = hit.badge;
		if (known.fighter < 0 && hit.hasMain) known.fighter = hit.main;
	}

	void ClearSearch() {
		const auto finished = search_.answer.finished, revision = search_.answer.revision;
		search_ = SearchView();
		search_.answer.finished = finished;
		search_.answer.revision = revision;
		searchTicket_ = 0;
		searchStage_ = SearchStage::Idle;
	}

	// Everything about the service the requests were for.
	void Forget() {
		Close();
		profile_.opened = opened_;
		ClearSearch();
		service_ = Service::Unknown;
		missingMs_ = 0;
		sets_.clear();
		epoch_ = 0;
		refreshAtMs_ = 0;
		sentOnce_ = false;
	}

	std::string bridge_, emberId_;
	Service service_ = Service::Unknown;
	std::uint64_t missingMs_ = 0;
	Profile profile_;
	// What the look-up asks by: the opener's key, or the search hit's card token for a name.
	Key resolved_;
	Stage stage_ = Stage::Idle;
	Ticket profileTicket_ = 0;
	// The card is looked up again at the next Take (stale_), and the answer to the look-up in flight predates a
	// friends action (overtaken_). sentOnce_ and lastLookupMs_ pace a refresh.
	bool stale_ = false, overtaken_ = false, sentOnce_ = false;
	std::uint64_t lastLookupMs_ = 0;
	std::uint64_t opened_ = 0;
	SearchView search_;
	SearchStage searchStage_ = SearchStage::Idle;
	Ticket searchTicket_ = 0;
	Ticket ticket_ = 0;
	std::uint64_t epoch_ = 0, refreshAtMs_ = 0;
	std::vector<std::uint64_t> sets_;
};

} } }
