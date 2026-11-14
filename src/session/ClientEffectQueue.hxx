#pragma once
// The client queue: the room's committed effects for this PC, held at the head
// until the checkpoint of their commit applies here (IrohRoom::AuthorizedEffect
// decides each head), and the replays of applied commits. Its rules, in one
// place:
//  - A verification frame never waits for a checkpoint. A bare battle_hash or
//    battle_snapshot belongs to no commit (the leader relays it as it comes,
//    without a token), and the game only compares it with its own fight's
//    hash for that frame, so the frames go to the game first, on the next poll,
//    whatever room messages are parked. They wait in their own lane, which keeps
//    the newest MaximumQueuedVerification (QueueRoomMessage) and takes none of
//    the room messages' slots.
//  - Only the leader's stream says where a commit is. A token from any other
//    connection is checked by the effect gate like any other, but never decides
//    what this queue replaces, holds back or fails for.
//  - Room state replaces itself: a newer room_snapshot or data_update for the
//    same member from the same sender drops the older one still queued in the
//    run of state at the queue's end, except that a snapshot with the chat goes
//    only for a newer one with the chat. Any other room message ends the run,
//    so no state moves past an event.
//  - A replay goes where its live copy would have been, before the first queued
//    effect committed after it, and waits while that live copy is still queued.
//    Replays fill at most half of the room messages' slots.
//  - Limits: MaximumRoomQueueMessages and the byte budget shared with the
//    server queue, and MaximumParkedCommits past the oldest commit it has
//    waited on, which is what the journal is sure to cover (SessionRecovery.hxx).
#include "RoomMessageQueue.hxx"
#include "SessionRecovery.hxx"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>

namespace sf4e { namespace session {

// How many commits past the oldest one it still waits on a member may queue:
// the commits the journal is sure to cover at the busiest room's rate of
// effects that are not room state (GuaranteedEffectEnvelopes).
constexpr std::uint64_t MaximumParkedCommits = GuaranteedEffectEnvelopes / BusiestEffectsPerCommit;

class ClientEffectQueue {
public:
	// A receipt's notification has no place of its own in its commit: it goes
	// after everything of the commit it was applied at.
	static constexpr std::uint64_t AfterCommit = (std::numeric_limits<std::uint64_t>::max)();

	// A live effect or a verification frame; `fromLeader` when it came on the
	// leader's stream, `budget` the bytes the room's byte budget leaves this
	// queue. Anything past the limits is refused, and the room fails under the
	// result's name.
	RoomQueueResult Admit(Message message, bool fromLeader, std::uint64_t appliedRevision, std::size_t budget) {
		if (BareFrame(message)) return QueueRoomMessage(frames_, bytes_, std::move(message), budget);
		auto& tag = message.tag;
		if (!fromLeader) tag.commitTerm = tag.commitRevision = tag.commitSequence = tag.commitRecipient = 0;
		if (tag.commitRevision > appliedRevision) {
			parkedSince_ = ParkedSince(appliedRevision);
			if (!parkedSince_ || tag.commitRevision < parkedSince_) parkedSince_ = tag.commitRevision;
			if (tag.commitRevision - parkedSince_ > MaximumParkedCommits) return RoomQueueResult::CommitsBehind;
		}
		DropReplacedState(message);
		return QueueRoomMessage(messages_, bytes_, std::move(message), budget);
	}

	// The oldest commit not applied yet that the queue has waited on, 0 when
	// none. A replaced state message leaves it where it was.
	std::uint64_t ParkedSince(std::uint64_t appliedRevision) const {
		if (parkedSince_ > appliedRevision) return parkedSince_;
		const auto oldest = std::find_if(messages_.begin(), messages_.end(),
			[&](const Message& queued) { return queued.tag.commitRevision > appliedRevision; });
		return oldest == messages_.end() ? 0 : oldest->tag.commitRevision;
	}

	bool TakesReplay() const { return messages_.size() < MaximumRoomQueueMessages / 2; }
	bool HoldsCopy(std::uint64_t term, std::uint64_t sequence) const {
		return sequence && std::any_of(messages_.begin(), messages_.end(), [&](const Message& queued) {
			return queued.tag.commitSequence == sequence && queued.tag.commitTerm == term;
		});
	}
	// `revision` and `sequence` place the replay in commit order: S_k still
	// comes before D_k when D_k's live copy is queued.
	void InsertReplay(Message message, std::uint64_t revision, std::uint64_t sequence) {
		const auto later = std::find_if(messages_.begin(), messages_.end(), [&](const Message& queued) {
			const auto& tag = queued.tag;
			return tag.commitRevision > revision || (tag.commitRevision == revision && tag.commitSequence > sequence);
		});
		bytes_ += message.payload.size();
		messages_.insert(later, std::move(message));
	}

	// The drain, one message at a time: the frames, then the room messages from
	// their head. `size` is the payload size the message was queued with: the
	// gate rewrites a delivered payload without its token.
	bool Empty() const { return frames_.empty() && messages_.empty(); }
	std::size_t Size() const { return frames_.size() + messages_.size(); }
	// The payload bytes queued, the client's share of the room's byte budget.
	std::size_t Bytes() const { return bytes_; }
	Message& Next() { return frames_.empty() ? messages_.front() : frames_.front(); }
	void PopNext(std::size_t size) {
		bytes_ -= size;
		(frames_.empty() ? messages_ : frames_).pop_front();
	}
	// Activation's removal of notifications a newer receipt no longer needs.
	template <class Remove> void EraseIf(Remove remove) {
		for (auto queued = messages_.begin(); queued != messages_.end();) {
			if (!remove(*queued)) { ++queued; continue; }
			bytes_ -= queued->payload.size();
			queued = messages_.erase(queued);
		}
	}
	void Clear() { frames_.clear(); messages_.clear(); bytes_ = 0; parkedSince_ = 0; }
	// What is queued, oldest first, for diagnostics and tests: the room
	// messages, whose head is what the queue waits on, and the frames.
	const std::deque<Message>& Messages() const { return messages_; }
	const std::deque<Message>& Frames() const { return frames_; }

private:
	// A verification frame without a commit token or a replay marker: the gate
	// passes it on its sender alone. One with either waits with the room
	// messages, for the gate to decide at their head.
	static bool BareFrame(const Message& message) {
		return !message.tag.committed && !message.tag.terminalReplay && IsRoomVerificationMessage(message);
	}

	void DropReplacedState(const Message& incoming) {
		const auto& newer = incoming.tag;
		if (newer.projection == RoomProjection::None || !newer.commitRevision) return;
		auto run = messages_.end();
		while (run != messages_.begin()) {
			const auto& previous = *std::prev(run);
			if (previous.tag.projection == RoomProjection::None) break;
			--run;
		}
		const auto snapshot = [](RoomProjection kind) {
			return kind == RoomProjection::Snapshot || kind == RoomProjection::SnapshotWithChat;
		};
		const auto replaced = [&](const Message& queued) {
			const auto& older = queued.tag;
			return queued.connection == incoming.connection && older.commitRecipient == newer.commitRecipient &&
				(older.projection == newer.projection || (snapshot(older.projection) && snapshot(newer.projection))) &&
				older.commitRevision < newer.commitRevision &&
				(older.projection != RoomProjection::SnapshotWithChat || newer.projection == RoomProjection::SnapshotWithChat);
		};
		for (auto queued = run; queued != messages_.end();) {
			if (!replaced(*queued)) { ++queued; continue; }
			bytes_ -= queued->payload.size();
			queued = messages_.erase(queued);
		}
	}

	std::deque<Message> frames_, messages_;
	std::size_t bytes_ = 0;
	std::uint64_t parkedSince_ = 0;
};

} }
