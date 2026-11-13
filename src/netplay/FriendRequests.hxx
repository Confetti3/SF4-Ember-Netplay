#pragma once
// The runtime's half of friends: which sync or action is in flight, what its
// answer did to the state the interface reads, and when the next sync is due.
// The runtime sends the requests and decodes the answers; this decides, so the
// rules are tested without a helper (friend_requests_test.cxx).
// docs/design/FRIENDS.md, "Presence" and "Client".
//
// - One sync or action is in flight at a time. An action asked meanwhile is
//   held (one, the first), and goes as soon as the flight ends. The runtime
//   sends them through BridgeFlights, which routes the answer, gives up on a
//   lost request and ends one the helper could not take; the answer comes
//   here under the token the flight began with, so one that outlived a change
//   of service or Ember ID is nobody's.
// - A sync is due every IdleSyncMs while the game runs, every ShownSyncMs
//   while the Friends tab shows, at once when the tab opens or the player's
//   own presence changes, and SettleMs after an action. Never two within
//   MinGapMs (the bridge allows 12 a minute).
// - Answers belong to one service and one Ember ID. A change of either
//   forgets the state, the refusal and anything held; the counts carry on.
//   An action it drops (in flight or held) is refused as `Dropped`, and one
//   asked while another is already held as `BusyCode`, so whatever asked for it
//   is told it ended: no action ends without an answer or a refusal.
// - A failed request keeps the state it had. A service without friends, or a
//   player without a Discord account, installs that state and stops the
//   automatic syncs; opening the tab asks again. Friends turned off (the
//   bridge answers "friends_off", to a sync or to any action) is a state too:
//   the syncs stop, and nothing but the player's Turn on asks again, however
//   often the scope is set, until a state answers.
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
#include <variant>

namespace sf4e { namespace netplay { namespace friends {

class Requests {
public:
	static constexpr std::uint64_t IdleSyncMs = 30000, ShownSyncMs = 10000, MinGapMs = 5000, SettleMs = 2000;
	// The tab's cadence lapses this long after it was last said to show, in case the interface never said it closed.
	static constexpr std::uint64_t ShownLapseMs = 600000;
	// The helper's codes for a service without friends and for a player without a Discord account.
	static constexpr const char* Unavailable = "friends_unavailable";
	static constexpr const char* NoAccount = "no_discord_account";
	// The bridge's code for a player who turned friends off.
	static constexpr const char* Off = "friends_off";
	// This runtime's codes: an action dropped by a change of service or Ember ID, and one asked while another waits.
	static constexpr const char* Dropped = "friends_dropped";
	static constexpr const char* BusyCode = "friends_busy";

	Requests() { answered_.value = Empty(); }

	const Answer& Published() const { return answered_; }
	const std::string& Bridge() const { return bridge_; }
	const std::string& EmberId() const { return emberId_; }
	bool Visiting() const { return visiting_; }
	// Syncs are running: a service is named, and it has friends for this player and they are on.
	bool Syncing() const { return !bridge_.empty() && !stopped_; }
	// An action is in flight or held: the interface may not ask another.
	bool Busy() const { return actionInFlight() || held_.has_value(); }
	// The helper's code for the last action that failed, empty when the last did not (or none ran).
	const std::string& Refusal() const { return refusal_; }
	// How many actions have been refused, so a reader tells a new refusal from the one it saw.
	std::uint64_t Refusals() const { return refusals_; }

	// The service and Ember ID the next request is for. Starts a sync at once, as a new service has none yet,
	// unless the player turned friends off for this very account: that stays until Turn on. True when it dropped
	// an action in flight or held, which it refuses as `Dropped`.
	bool Scope(const std::string& bridge, const std::string& emberId) {
		if (bridge == bridge_ && emberId == emberId_) return false;
		const bool dropped = actionInFlight() || held_.has_value();
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
		off_ = false;
		wanted_ = true;
		nextSyncMs_ = 0;
		if (!bridge_.empty() && bridge_ == offBridge_ && emberId_ == offEmberId_) Park(State::Kind::Off, offMs_);
		if (dropped) Refuse(Dropped);
		return dropped;
	}

	// The Friends tab opened (or Settings asks): sync at once, keep the tab's
	// cadence until Leave, and ask again of a service that had none.
	void Visit(std::uint64_t nowMs) {
		visiting_ = true;
		visitedMs_ = nowMs;
		if (!off_) stopped_ = false;
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
		if (bridge_.empty() || flight_.token || held_ || stopped_) return false;
		if (synced_ && nowMs - lastSyncMs_ < MinGapMs) return false;
		return wanted_ || nowMs >= nextSyncMs_;
	}

	// Whether an action may go now.
	bool MayAct() const { return !bridge_.empty() && !flight_.token; }

	// Keeps an action asked while a request is in flight; false when one is
	// already held. Taken once the flight ends.
	bool Hold(commands::FriendAction action) {
		if (held_) return false;
		held_ = std::move(action);
		return true;
	}
	std::optional<commands::FriendAction> TakeHeld() {
		if (flight_.token || !held_) return std::nullopt;
		auto action = std::move(held_);
		held_.reset();
		return action;
	}

	// A sync, or an action, is about to go to the helper: returns the token its
	// answer comes back under (Finish). Nothing else may be in flight.
	std::uint64_t BeginSync(std::uint64_t nowMs) {
		wanted_ = false;
		synced_ = true;
		lastSyncMs_ = nowMs;
		return Begin(false);
	}
	// Turn on is going: the account is no longer kept as off, so a change of
	// scope that drops its answer cannot park this account in Off again when
	// the scope comes back to it (the next sync asks the bridge instead).
	void TurningOn() { offBridge_.clear(); offEmberId_.clear(); }

	std::uint64_t BeginAction() {
		refusal_.clear();
		return Begin(true);
	}

	bool Awaiting(std::uint64_t token) const { return token && token == flight_.token; }

	// The answer to the flight `token`: the state, or why there is none (the
	// helper's code, or "bridge_invalid_response"). An answer of a flight that
	// was retired (another service or Ember ID, or it already ended) is ignored.
	void Finish(std::uint64_t token, std::string failure, std::optional<State> state, std::uint64_t nowMs) {
		if (!Awaiting(token)) return;
		const bool action = flight_.isAction;
		flight_ = {};
		answered_.loading = false;
		++answered_.finished;
		// Friends that are off are the state the answer is, to a Turn off as to any other request.
		if (failure == Off) { Park(State::Kind::Off, nowMs); failure.clear(); }
		else if (state) { Install(std::move(*state), nowMs); stopped_ = false; off_ = false; offBridge_.clear(); }
		answered_.error = failure;
		if (!failure.empty()) {
			if (action) { refusal_ = failure; ++refusals_; }
			if (failure == Unavailable) StopWith(State::Kind::Unavailable, nowMs);
			else if (failure == NoAccount) StopWith(State::Kind::NoAccount, nowMs);
		}
		nextSyncMs_ = nowMs + (action && failure.empty() ? SettleMs : visiting_ ? ShownSyncMs : IdleSyncMs);
	}

	// An action that could not even be sent (nothing to offer, no Ember ID) is
	// refused as an answer would refuse it: counted, so nothing reports it done.
	void Refuse(const std::string& reason) {
		refusal_ = reason;
		++refusals_;
	}

	// Ends the tab cadence nobody renewed.
	void LapseVisit(std::uint64_t nowMs) {
		if (visiting_ && nowMs - visitedMs_ >= ShownLapseMs) visiting_ = false;
	}

private:
	struct InFlight {
		std::uint64_t token = 0;
		bool isAction = false;
	};

	bool actionInFlight() const { return flight_.token && flight_.isAction; }

	std::uint64_t Begin(bool isAction) {
		flight_ = {++tokens_, isAction};
		answered_.loading = true;
		answered_.error.clear();
		return flight_.token;
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

	// The state that says why there are no friends to list, and that nothing more is asked of its own accord.
	void Park(State::Kind kind, std::uint64_t nowMs) {
		State state;
		state.kind = kind;
		Install(std::move(state), nowMs);
		stopped_ = true;
		off_ = kind == State::Kind::Off;
		if (off_) { offBridge_ = bridge_; offEmberId_ = emberId_; offMs_ = nowMs; }
		else offBridge_.clear();
	}
	// The service has no friends, or the player no account: nothing more is asked until the tab opens.
	void StopWith(State::Kind kind, std::uint64_t nowMs) { Park(kind, nowMs); }

	std::string bridge_, emberId_, refusal_;
	// The account the player turned friends off for, kept across a change of scope back to it.
	std::string offBridge_, offEmberId_;
	Answer answered_;
	InFlight flight_;
	std::optional<commands::FriendAction> held_;
	bool visiting_ = false, stopped_ = false, wanted_ = true, synced_ = false, off_ = false;
	std::uint64_t tokens_ = 0, visitedMs_ = 0, lastSyncMs_ = 0, nextSyncMs_ = 0, presenceKey_ = 0, refusals_ = 0, offMs_ = 0;
};

} } }
