#pragma once
// The runtime's half of other players' cards in a public room: which tokens
// the room's members carry, which of them have been looked up, and when the
// next look-up may go. The runtime sends the requests and decodes the answers;
// this decides, so the rules are tested without a helper
// (card_requests_test.cxx). docs/design/FIGHTER_LEVELS.md, "Client".
//
// - A token is looked up when a member carrying it joins, and every token
//   again 8 s after any set in the room ends. A look-up is up to MaxLookup
//   tokens, at most one in flight, and at most one every MinGapMs.
// - A token the bridge answers null for has "no card" until the next trigger.
//   An old card stays on show while its refresh is awaited.
// - The friend the Friends tab has focused is wanted too (WantFriend), by their friend id ("fr_...", which the bridge's
//   look-up takes as a selector): it rides in the same look-up as the members' tokens, newest wanted first, and the last
//   MaxFriends are kept. A look-up of friends alone may come FriendGapMs after the last, so a card shows soon after the
//   focus lands; one that includes a member's token, or follows a failure (a refusal for too many), waits MinGapMs.
// - A friends action that went through changes how cards stand with the
//   player: every card is looked up again at the next look-up (Refresh), and
//   the card the action named shows what it made of the member at once
//   (Relate), until that look-up says how it really stands.
// - A token no member of the room carries is dropped. A new room (another
//   epoch), another service, or no public room forgets everything, and so
//   ends a stop for an answer of `Unavailable`.
#include "Levels.hxx"
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay { namespace levels {

class Cards {
public:
	// Token to card, for the tokens with a card.
	using Map = CardMap;
	// The helper bounds its own bridge requests; this only ends a wait for one it lost.
	static constexpr std::uint64_t GiveUpMs = 60000;
	// The least time between look-ups, and the wait after a set ends.
	static constexpr std::uint64_t MinGapMs = 10000, SetEndedMs = 8000;
	// The least time before a look-up of friends' cards alone, and how many friends are kept wanted.
	static constexpr std::uint64_t FriendGapMs = 3000;
	static constexpr std::size_t MaxFriends = 16;
	// The helper's code for a bridge that has no levels.
	static constexpr const char* Unavailable = "points_unavailable";

	Cards() : published_(std::make_shared<const Map>()) {}

	// Whether a look-up key is a friend id (the bridge's "fr_..."), not a member's card token ("c1.").
	static bool IsFriendKey(const std::string& key) { return key.compare(0, 3, "fr_") == 0; }

	// The cards on show: never null, and replaced (never changed) when they change.
	const std::shared_ptr<const Map>& Published() const { return published_; }
	const std::string& Bridge() const { return bridge_; }
	const std::string& Error() const { return error_; }

	// The service the look-ups go to; another one forgets everything.
	void Scope(const std::string& bridge) {
		if (bridge == bridge_) return;
		// Friend ids belong to the service they were asked of; one asked for before any service was set stays.
		if (!bridge_.empty()) friends_.clear();
		bridge_ = bridge;
		Forget();
	}

	// The Friends tab has the friend `id` focused: their card is looked up with the others, and kept while they are among
	// the last MaxFriends wanted. A friend wanted again moves to the front, so a look-up asks for the newest first.
	void WantFriend(const std::string& id) {
		if (!IsFriendKey(id)) return;
		const auto at = std::find(friends_.begin(), friends_.end(), id);
		if (at != friends_.end()) friends_.erase(at);
		friends_.insert(friends_.begin(), id);
		if (friends_.size() > MaxFriends) friends_.resize(MaxFriends);
	}

	// The room as it is now: its epoch (0 when none, or not a public room),
	// the tokens its members carry, and the generation of each table's last
	// set. The first look at a room only notes what already ended.
	void Observe(std::uint64_t epoch, const std::vector<std::string>& tokens, const std::vector<std::uint64_t>& sets, std::uint64_t nowMs) {
		if (epoch != epoch_) {
			Forget();
			epoch_ = epoch;
		}
		tokens_.clear();
		for (const auto& token : tokens)
			if (!Wanted(token)) tokens_.push_back(token);
		bool dropped = false;
		for (auto entry = entries_.begin(); entry != entries_.end();) {
			if (Wanted(entry->first)) { ++entry; continue; }
			dropped = dropped || entry->second.card;
			entry = entries_.erase(entry);
		}
		if (dropped) Publish();
		if (sets.size() == sets_.size())
			for (std::size_t table = 0; table < sets.size(); ++table)
				if (sets[table] && sets[table] != sets_[table]) refreshAtMs_ = nowMs + SetEndedMs;
		sets_ = sets;
		if (refreshAtMs_ && nowMs >= refreshAtMs_) {
			for (auto& entry : entries_) entry.second.stale = true;
			refreshAtMs_ = 0;
		}
	}

	bool Awaiting(std::uint64_t id) const { return id && id == flight_.id; }

	// The tokens to look up now, up to MaxLookup of those without an answer
	// (or whose answer is stale); none while a look-up is in flight, after
	// `Unavailable`, or within MinGapMs of the last one.
	std::vector<std::string> Take(std::uint64_t nowMs) const {
		std::vector<std::string> batch;
		if (flight_.id || unavailable_ || bridge_.empty()) return batch;
		const auto asked = [&](const std::string& key) {
			const auto entry = entries_.find(key);
			return entry == entries_.end() || entry->second.stale;
		};
		for (const auto& token : tokens_) {
			if (!asked(token)) continue;
			batch.push_back(token);
			if (batch.size() == MaxLookup) break;
		}
		// Members first: the room's cards are the ones the board waits on. Friends fill what is left of the look-up.
		const bool members = !batch.empty();
		for (const auto& id : friends_) {
			if (batch.size() == MaxLookup) break;
			if (asked(id)) batch.push_back(id);
		}
		// Friends alone wait the shorter gap, unless the last look-up failed (the budget the bridge keeps may be spent).
		const std::uint64_t gap = members || !error_.empty() ? MinGapMs : FriendGapMs;
		if (sent_ && nowMs - lastSentMs_ < gap) batch.clear();
		return batch;
	}

	// `batch` (from Take) handed to the helper as `id`; 0 when it could not be, which ends it now.
	void Sent(std::uint64_t id, const std::vector<std::string>& batch, std::uint64_t nowMs) {
		sent_ = true;
		lastSentMs_ = nowMs;
		batch_ = batch;
		flight_ = {id, nowMs};
		if (!id) Finish("helper_unavailable", std::nullopt);
	}

	// The answer: one entry per token of the batch, in order, or why there is
	// none (the helper's code, or "bridge_invalid_response"). Tokens left
	// unanswered are asked again after the gap.
	void Finish(const std::string& failure, std::optional<std::vector<std::optional<Card>>> answers) {
		const auto batch = std::move(batch_);
		const auto overtaken = std::move(overtaken_);
		batch_.clear();
		overtaken_.clear();
		flight_ = {};
		error_ = failure;
		if (failure == Unavailable) unavailable_ = true;
		if (!answers || answers->size() != batch.size()) return;
		for (std::size_t i = 0; i < batch.size(); ++i) {
			if (!Wanted(batch[i])) continue;
			// An answer asked before a friends action went through is older than what the card shows: kept, and asked again.
			if (overtaken.count(batch[i]) && entries_.count(batch[i])) { entries_[batch[i]].stale = true; continue; }
			entries_[batch[i]] = {std::move((*answers)[i]), overtaken.count(batch[i]) != 0};
		}
		Publish();
	}

	// Ends the look-up the helper never answered.
	void Expire(std::uint64_t nowMs) {
		if (flight_.id && nowMs - flight_.sentMs >= GiveUpMs) Finish("unavailable", std::nullopt);
	}

	// A friends action went through: every card is looked up again at the next look-up, and a look-up in flight,
	// asked before the action, does not stand for the cards it names.
	void Refresh() {
		for (auto& entry : entries_) entry.second.stale = true;
		overtaken_.insert(batch_.begin(), batch_.end());
	}
	// The action named the member carrying `token` (Add friend, Accept, Block): their card shows `relation` until
	// the look-up Refresh asked for answers. Nothing for a token without a card.
	void Relate(const std::string& token, Relation relation) {
		const auto entry = entries_.find(token);
		if (entry == entries_.end() || !entry->second.card) return;
		entry->second.card->relation = relation;
		entry->second.stale = true;
		Publish();
	}

private:
	struct Entry {
		// None: the bridge has no card for the token.
		std::optional<Card> card;
		bool stale = false;
	};
	struct InFlight { std::uint64_t id = 0, sentMs = 0; };

	bool Wanted(const std::string& key) const {
		return std::find(tokens_.begin(), tokens_.end(), key) != tokens_.end() || std::find(friends_.begin(), friends_.end(), key) != friends_.end();
	}

	void Publish() {
		auto cards = std::make_shared<Map>();
		for (const auto& entry : entries_)
			if (entry.second.card) cards->emplace(entry.first, *entry.second.card);
		published_ = std::move(cards);
	}

	// Everything about a room: its tokens, answers, schedule and stop.
	void Forget() {
		const bool shown = !published_->empty();
		tokens_.clear();
		entries_.clear();
		sets_.clear();
		batch_.clear();
		overtaken_.clear();
		flight_ = {};
		error_.clear();
		unavailable_ = false;
		refreshAtMs_ = 0;
		if (shown) Publish();
	}

	std::string bridge_, error_;
	std::uint64_t epoch_ = 0;
	std::vector<std::string> tokens_;
	// The friends the Friends tab focused lately, newest first (WantFriend).
	std::vector<std::string> friends_;
	std::map<std::string, Entry> entries_;
	std::vector<std::uint64_t> sets_;
	std::vector<std::string> batch_;
	// The tokens of the look-up in flight that a friends action overtook (Refresh).
	std::set<std::string> overtaken_;
	InFlight flight_;
	bool sent_ = false, unavailable_ = false;
	std::uint64_t lastSentMs_ = 0, refreshAtMs_ = 0;
	std::shared_ptr<const Map> published_;
};

} } }
