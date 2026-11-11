#pragma once
// The runtime's half of friends: which sync or action is in flight, what its
// answer did to the state the interface reads, and when the next sync is due.
// The runtime sends the requests and decodes the answers; this decides, so the
// rules are tested without a helper (friend_requests_test.cxx).
// docs/design/FRIENDS.md, "Presence" and "Client".
//
// - One sync or action is in flight at a time. An action asked meanwhile is
//   held (one, the first), and goes as soon as the flight ends.
// - A sync is due every IdleSyncMs while the game runs, every ShownSyncMs
//   while the Friends tab shows, at once when the tab opens or the player's
//   own presence changes, and SettleMs after an action. Never two within
//   MinGapMs (the bridge allows 12 a minute).
// - Answers belong to one service and one Ember ID. A change of either
//   forgets the state, the refusal and anything held; the counts carry on.
// - A failed request keeps the state it had. A service without friends, or a
//   player without a Discord account, installs that state and stops the
//   automatic syncs; opening the tab asks again. So does turning friends off
//   (Forget), until the tab is opened again.
// - The last refusal of an action is kept, as the helper's code, until the
//   next action starts.
#include "Answered.hxx"
#include "FriendCommands.hxx"
#include "Friends.hxx"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace sf4e { namespace netplay { namespace friends {

class Requests {
public:
	// The helper bounds its own bridge requests; this only ends a wait for one it lost.
	static constexpr std::uint64_t GiveUpMs = 60000;
	static constexpr std::uint64_t IdleSyncMs = 30000, ShownSyncMs = 10000, MinGapMs = 5000, SettleMs = 2000;
	// The tab's cadence lapses this long after it was last said to show, in case the interface never said it closed.
	static constexpr std::uint64_t ShownLapseMs = 600000;
	// The helper's codes for a service without friends and for a player without a Discord account.
	static constexpr const char* Unavailable = "friends_unavailable";
	static constexpr const char* NoAccount = "no_discord_account";

	Requests() { answered_.value = Empty(); }

	const Answer& Published() const { return answered_; }
	const std::string& Bridge() const { return bridge_; }
	const std::string& EmberId() const { return emberId_; }
	bool Visiting() const { return visiting_; }
	// Syncs are running: a service is named, and it has friends for this player and they are on.
	bool Syncing() const { return !bridge_.empty() && !stopped_; }
	// An action is in flight or held: the interface may not ask another.
	bool Busy() const { return actionInFlight() || held_.has_value(); }
	// The action in flight, or nothing.
	std::optional<Command::Kind> Acting() const {
		return actionInFlight() ? std::optional<Command::Kind>(flight_.action) : std::nullopt;
	}
	// The helper's code for the last action that failed, empty when the last did not (or none ran).
	const std::string& Refusal() const { return refusal_; }
	// How many actions have been refused, so a reader tells a new refusal from the one it saw.
	std::uint64_t Refusals() const { return refusals_; }

	// The service and Ember ID the next request is for. Starts a sync at once, as a new service has none yet.
	void Scope(const std::string& bridge, const std::string& emberId) {
		if (bridge == bridge_ && emberId == emberId_) return;
		bridge_ = bridge;
		emberId_ = emberId;
		Answer fresh;
		fresh.value = Empty();
		fresh.finished = answered_.finished;
		fresh.revision = answered_.revision;
		answered_ = std::move(fresh);
		flight_ = {};
		held_.reset();
		refusal_.clear();
		stopped_ = false;
		wanted_ = true;
		nextSyncMs_ = 0;
	}

	// The Friends tab opened (or Settings asks): sync at once, keep the tab's
	// cadence until Leave, and ask again of a service that had none.
	void Visit(std::uint64_t nowMs) {
		visiting_ = true;
		visitedMs_ = nowMs;
		stopped_ = false;
		wanted_ = true;
	}
	void Leave() { visiting_ = false; }

	// The player's own presence, as a key that changes when it does: a change
	// asks for a sync at once.
	void NotePresence(std::uint64_t key) {
		if (key == presenceKey_) return;
		presenceKey_ = key;
		wanted_ = true;
	}

	// Whether a sync is due and nothing is in flight or waiting to go first.
	bool SyncDue(std::uint64_t nowMs) const {
		if (bridge_.empty() || flight_.id || held_ || stopped_) return false;
		if (synced_ && nowMs - lastSyncMs_ < MinGapMs) return false;
		return wanted_ || nowMs >= nextSyncMs_;
	}

	// Whether an action may go now.
	bool MayAct() const { return !bridge_.empty() && !flight_.id; }

	// Keeps an action asked while a request is in flight; false when one is
	// already held. Taken once the flight ends.
	bool Hold(const Command& command) {
		if (held_) return false;
		held_ = command;
		return true;
	}
	std::optional<Command> TakeHeld() {
		if (flight_.id || !held_) return std::nullopt;
		auto command = std::move(held_);
		held_.reset();
		return command;
	}

	// A sync, or an action of `kind`, handed to the helper as `id`; 0 when it
	// could not be, which ends it now.
	void SentSync(std::uint64_t id, std::uint64_t nowMs) {
		wanted_ = false;
		synced_ = true;
		lastSyncMs_ = nowMs;
		Begin(id, Command::Kind::Visit, false, nowMs);
	}
	void SentAction(std::uint64_t id, Command::Kind kind, std::uint64_t nowMs) {
		refusal_.clear();
		Begin(id, kind, true, nowMs);
	}

	bool Awaiting(std::uint64_t id) const { return id && id == flight_.id; }

	// The answer: the state, or why there is none (the helper's code, or
	// "bridge_invalid_response").
	void Finish(const std::string& failure, std::optional<State> state, std::uint64_t nowMs) {
		const bool action = flight_.isAction;
		const auto kind = flight_.action;
		flight_ = {};
		answered_.loading = false;
		answered_.error = failure;
		++answered_.finished;
		if (state) Install(std::move(*state), nowMs);
		if (failure.empty()) {
			if (action && kind == Command::Kind::Forget) stopped_ = true;
		} else {
			if (action) { refusal_ = failure; ++refusals_; }
			if (failure == Unavailable) StopWith(State::Kind::Unavailable, nowMs);
			else if (failure == NoAccount) StopWith(State::Kind::NoAccount, nowMs);
		}
		nextSyncMs_ = nowMs + (action && failure.empty() ? SettleMs : visiting_ ? ShownSyncMs : IdleSyncMs);
	}

	// Ends the request the helper never answered, and the tab cadence nobody renewed.
	void Expire(std::uint64_t nowMs) {
		if (flight_.id && nowMs - flight_.sentMs >= GiveUpMs) Finish("unavailable", std::nullopt, nowMs);
		if (visiting_ && nowMs - visitedMs_ >= ShownLapseMs) visiting_ = false;
	}

private:
	struct InFlight {
		std::uint64_t id = 0, sentMs = 0;
		bool isAction = false;
		Command::Kind action = Command::Kind::Visit;
	};

	bool actionInFlight() const { return flight_.id && flight_.isAction; }

	void Begin(std::uint64_t id, Command::Kind kind, bool isAction, std::uint64_t nowMs) {
		flight_ = {id, nowMs, isAction, kind};
		answered_.loading = true;
		answered_.error.clear();
		if (!id) Finish("helper_unavailable", std::nullopt, nowMs);
	}

	static const std::shared_ptr<const State>& Empty() {
		static const std::shared_ptr<const State> empty = std::make_shared<const State>();
		return empty;
	}

	void Install(State state, std::uint64_t nowMs) {
		answered_.value = std::make_shared<const State>(std::move(state));
		answered_.loaded = true;
		answered_.installedMs = nowMs;
		++answered_.revision;
	}

	// The service has no friends, or the player no account: that is the state, and nothing more is asked until the tab opens.
	void StopWith(State::Kind kind, std::uint64_t nowMs) {
		State state;
		state.kind = kind;
		Install(std::move(state), nowMs);
		stopped_ = true;
	}

	std::string bridge_, emberId_, refusal_;
	Answer answered_;
	InFlight flight_;
	std::optional<Command> held_;
	bool visiting_ = false, stopped_ = false, wanted_ = true, synced_ = false;
	std::uint64_t visitedMs_ = 0, lastSyncMs_ = 0, nextSyncMs_ = 0, presenceKey_ = 0, refusals_ = 0;
};

} } }
