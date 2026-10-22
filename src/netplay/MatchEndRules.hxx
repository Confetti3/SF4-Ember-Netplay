#pragma once

// Pure rules for the end of a room match, kept game-free so they can be unit
// tested. NetplayRuntime applies them on the game thread.

#include "../session/RoomModel.hxx"

#include <algorithm>
#include <array>
#include <cstdint>

namespace sf4e { namespace netplay {

// The room delivers one committed match end to a client more than once: the
// live event, the terminal receipt replayed from a checkpoint, and resends
// until the client acknowledges it. Each copy is processed (the handlers are
// idempotent), but only the first is worth a log line. Each table remembers the
// last generation it logged, so copies for two tables can interleave.
class MatchEndLog {
public:
	bool First(std::uint8_t table, std::uint64_t generation) {
		if (table >= room::TableCount) return true;
		if (seen_[table] && generation == generation_[table]) return false;
		seen_[table] = true; generation_[table] = generation;
		return true;
	}
private:
	std::array<bool, room::TableCount> seen_{};
	std::array<std::uint64_t, room::TableCount> generation_{};
};

// Positive evidence, from the local projection of one table, that a match
// generation has ended: the table has started a newer generation, or it still
// shows this one outside play (a start sets the generation and Playing
// together). A projection that has not reached this generation yet proves
// nothing; the native grant can arrive before it. The projection can lag the
// authority but never lead it, so an ended generation has ended there too, and
// a generation-scoped Unwatch or AbortMatch for it would only be rejected with
// WrongGeneration.
inline bool GenerationEnded(const room::Table& table, std::uint64_t generation) {
	if (table.matchGeneration != generation) return table.matchGeneration > generation;
	return table.phase != room::TablePhase::Playing && table.phase != room::TablePhase::Paused;
}

// The runtime normally learns that a battle ended when the battle closes with
// its GGPO session still live. A battle can also close after the session was
// retired (a spectator whose stream was lost or played out, a fighter whose
// GGPO failed, a session that never started), and then it says so through this
// rule for the generation whose entry closed. spectatorStreamFailed says the
// session was retired by the spectator's own stream failure, as opposed to the
// game playing out or its end being committed. The verdict only decides what
// the battle does locally; the lock release a stream failure owes the room is
// SpectatorLockRelease, which does not wait for this close.
enum class SessionlessClose {
	// Not the entered generation, or something already ends it.
	Ignore,
	// The game is over for this client: end its session and send the room
	// nothing. A spectator that only played out or fell behind a finished game
	// has nothing to report, and a game whose end the room committed needs no abort.
	EndView,
	// A fighter's game was cut short with no committed result: report the abort.
	Abort,
	// A spectator's own GGPO stream failed while its link stayed up, whether or
	// not a result was committed since: end the view. The failure was recorded
	// when it happened, so its lock release is already queued and this close
	// sends the room nothing.
	LeaveGame,
};
inline SessionlessClose SessionlessCloseAction(bool entered, std::uint64_t enteredGeneration,
	std::uint64_t closedGeneration, std::uint64_t sessionGeneration, bool recovering,
	bool matchInProgress, bool spectator, bool endCommitted, bool spectatorStreamFailed) {
	if (!entered || !closedGeneration || enteredGeneration != closedGeneration ||
		sessionGeneration != closedGeneration || recovering || !matchInProgress) return SessionlessClose::Ignore;
	// A committed result that arrived after the failure does not erase it.
	if (spectator && spectatorStreamFailed) return SessionlessClose::LeaveGame;
	if (endCommitted) return SessionlessClose::EndView;
	return spectator ? SessionlessClose::EndView : SessionlessClose::Abort;
}

// The lock release a spectator owes the room after its own stream failure. It
// is armed when the failure is recorded, for that generation, and runs on its
// own: it does not wait for the battle to close and a match result committed
// meanwhile does not cancel it, because the generation-scoped Unwatch that
// releases a live game is refused (WrongGeneration) once the table has moved
// on. LockSpectating with locked=false is what the authority takes at any
// phase, so that is what is sent. It is idempotent there, and a projection that
// lags the player's own lock press costs at most one redundant unlock.
//
// Queueing the action only means it was handed to the local control link, and
// the helper can still refuse it (the control was replaced, its queue was full),
// so a queued release stays pending until the authority confirms it: an
// accepted reply to the action (Acknowledged), or a room snapshot that shows the
// member unlocked. Until then it is sent again every RetryMs, under a new action
// id each time so the member's action watermark keeps it ordered before any
// later action of the player.
//
// The release lapses when the player locks in again (Observe), when the room,
// the table or the local role changed, and after ExpiryMs.
class SpectatorLockRelease {
public:
	static constexpr std::uint64_t ExpiryMs = 30000;
	// How long a queued release waits for the authority before it is sent again.
	static constexpr std::uint64_t RetryMs = 1000;
	enum class Step {
		// Nothing armed.
		Idle,
		// The projection has not reached the failed generation yet.
		Wait,
		// Nothing left to send; the release is over.
		Drop,
		// The projection shows the member unlocked; the release is over.
		Done,
		// Queued and waiting for the authority; sent again once RetryMs has passed.
		Await,
		// Send the action written through Next.
		Send,
	};

	void Arm(std::uint8_t table, std::uint64_t generation, std::uint64_t roomEpoch, std::uint64_t nowMs) {
		pending_ = true; table_ = table; generation_ = generation; roomEpoch_ = roomEpoch; armedAtMs_ = nowMs;
		queued_ = false; actionId_ = 0; queuedAtMs_ = 0;
	}
	bool Pending() const { return pending_; }
	std::uint64_t Generation() const { return generation_; }
	void Cancel() { pending_ = false; }
	// An explicit lock-in is newer than the failure and stands.
	void Observe(const room::Action& action) {
		if (action.kind == room::ActionKind::LockSpectating && action.locked) Cancel();
	}
	// The release was queued to the room under actionId. It stays pending.
	void Queued(std::uint64_t actionId, std::uint64_t nowMs) {
		if (!pending_) return;
		queued_ = true; actionId_ = actionId; queuedAtMs_ = nowMs;
	}
	// The authority's reply to a room action. Only an accepted reply to the
	// latest queued release ends it; a refusal or silence leaves it to be sent
	// again, and an earlier copy that was accepted shows in the snapshot.
	void Acknowledged(std::uint64_t actionId) {
		if (pending_ && queued_ && actionId_ == actionId) Cancel();
	}

	Step Next(const room::Snapshot& snapshot, std::uint64_t nowMs, room::Action* action) const {
		if (!pending_) return Step::Idle;
		if (nowMs - armedAtMs_ >= ExpiryMs || snapshot.roomEpoch != roomEpoch_ || table_ >= room::TableCount) return Step::Drop;
		const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(),
			[&](const room::Member& item) { return item.id == snapshot.localMember; });
		if (member == snapshot.members.end() || member->seat >= 0 || member->table != static_cast<std::int8_t>(table_)) return Step::Drop;
		const auto& table = snapshot.tables[table_];
		if (!room::WatchesByChoice(table, member->id)) return Step::Drop;
		if (!member->spectatorLocked) return Step::Done;
		if (table.matchGeneration < generation_) return Step::Wait;
		if (queued_ && nowMs - queuedAtMs_ < RetryMs) return Step::Await;
		if (action) {
			room::Action release;
			release.kind = room::ActionKind::LockSpectating;
			release.locked = false;
			release.roomEpoch = snapshot.roomEpoch; release.revision = snapshot.revision;
			release.table = table_; release.tableRevision = table.revision;
			release.matchGeneration = generation_;
			*action = release;
		}
		return Step::Send;
	}
private:
	bool pending_ = false, queued_ = false;
	std::uint8_t table_ = 0;
	std::uint64_t generation_ = 0, roomEpoch_ = 0, armedAtMs_ = 0, actionId_ = 0, queuedAtMs_ = 0;
};

// Whether a local failure of an admitted match owes the spectator's lock release:
// the setup of its gameplay connection failed, or its stream did, whether or not
// a battle was ever entered. The generation is the one the grant admitted. A game
// whose end is already known needs none: a spectator that only fell behind a
// finished game has no failure to report and keeps its lock-in, and a fighter
// has no spectator lock.
inline bool SpectatorFailureOwesLockRelease(bool spectator, std::uint64_t admittedGeneration, bool generationFinished) {
	return spectator && admittedGeneration != 0 && !generationFinished;
}

// The game a spectator's live stream shows: the table it was admitted at and
// the generation it was admitted to. It belongs to the local match, not to the
// table, which the room moves on (back to Waiting, a queued member rotated
// into a seat, the next generation) while the delayed stream still plays.
// Empty while this PC watches nothing.
struct WatchedGame {
	int table = -1;
	std::uint64_t generation = 0;
	bool Active() const { return generation != 0 && table >= 0 && table < static_cast<int>(room::TableCount); }
	bool operator==(const WatchedGame& other) const { return table == other.table && generation == other.generation; }
};

// What leaving the watched game asks of the room. While the room still plays
// that game with the member among its spectators, a generation-scoped Unwatch
// for it: a queued member leaves that game alone and keeps its place
// (LeaveGame, an Unwatch that keeps watching), anyone else leaves the table
// (Unwatch). Once the room has finished the game, or no longer lists the
// member there, the room needs nothing, and only the rest of the local stream
// ends (EndView). None while nothing is watched, at a tournament's table,
// where the room takes no seating change, and for a fighter of the table's
// live game, whatever its own match says. The shell's row and the runtime,
// which decides when the leave runs, both read this.
enum class WatchedLeave { None, EndView, Unwatch, LeaveGame };
inline WatchedLeave LeaveOfWatchedGame(const room::Snapshot& room, const WatchedGame& game) {
	if (!game.Active() || (room.tournament.Active() && game.table == room::TournamentTable)) return WatchedLeave::None;
	const auto& table = room.tables[game.table];
	const auto* member = room::FindMember(room, room.localMember);
	const auto listed = [&](const std::vector<room::MemberId>& list) {
		return std::find(list.begin(), list.end(), member->id) != list.end();
	};
	const bool tableLive = table.phase == room::TablePhase::Playing || table.phase == room::TablePhase::Paused;
	if (member && member->table == game.table && member->seat >= 0 && tableLive) return WatchedLeave::None;
	if (GenerationEnded(table, game.generation) || !member || member->seat >= 0 || member->table != game.table ||
		!listed(table.spectators)) return WatchedLeave::EndView;
	return listed(table.queue) ? WatchedLeave::LeaveGame : WatchedLeave::Unwatch;
}

// Where the room stands when a parked room action may go again.
struct RetryGate {
	bool controlHealthy = false;
	// The authority takes mutations now (or the room is not coordinated).
	bool authorityWritable = false;
	bool recoveringMatch = false;
	// This PC watches a game whose stream is live.
	bool spectatorInGame = false;
	// At the main menu with no match in hand.
	bool atMenuIdle = false;
};
// Whether a room action parked for its gate is sent again now. Nothing goes
// over an unhealthy control or during a match recovery, and every action also
// waits for a writable authority. Applying rules closes the affected game, so
// its retry cannot wait for that game to end first. Other actions wait for the
// main menu, except a live spectator's Unwatch, Unqueue and chat.
inline bool ParkedActionRetryable(const room::Action& action, const RetryGate& gate) {
	if (!gate.controlHealthy || gate.recoveringMatch || !gate.authorityWritable) return false;
	const bool goesMidGame = action.kind == room::ActionKind::Unwatch || action.kind == room::ActionKind::Unqueue ||
		action.kind == room::ActionKind::Chat;
	return gate.atMenuIdle || action.kind == room::ActionKind::SetRules || (gate.spectatorInGame && goesMidGame);
}
// Whether a parked leave of the watched game is decided again now. Once the
// stream is gone it goes at once, to end as nothing left to leave rather than
// run out its budget. While the stream plays it waits for a healthy control
// and no recovery, but not for a writable authority: the Unwatch it may send
// is scoped by the game's generation, not by the table revision a checkpoint
// catch-up is still settling.
inline bool ParkedWatchedLeaveRetryable(const RetryGate& gate) {
	return !gate.spectatorInGame || (gate.controlHealthy && !gate.recoveringMatch);
}

// A grant for a newer generation while the runtime still holds the entry of an
// older one whose battle is gone. The older entry can never end by itself, and
// left in place it makes the controller refuse the new grant.
inline bool StaleMatchEntry(bool sessionLive, bool entered, std::uint64_t enteredGeneration,
	std::uint64_t sessionGeneration, bool ggpoLive) {
	return sessionLive && entered && enteredGeneration != sessionGeneration && !ggpoLive;
}

} }
