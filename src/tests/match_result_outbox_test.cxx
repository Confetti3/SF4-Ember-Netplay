#include "../netplay/MatchResultOutbox.hxx"
#include "../session/RoomModel.hxx"
#include "../common/NativeMatchResult.hxx"

#include <algorithm>
#include <cstdio>
#include "test_support.hxx"

using namespace sf4e;

static room::Action ActionFor(const room::RoomAuthority& authority, room::MemberId member,
    room::ActionKind kind, std::uint64_t actionId) {
    room::Action action;
    action.kind = kind;
    action.roomEpoch = authority.SnapshotView().roomEpoch;
    action.revision = authority.SnapshotView().revision;
    action.table = 0;
    action.tableRevision = authority.SnapshotView().tables[0].revision;
    action.actionId = actionId;
    return action;
}

static room::MemberId Join(room::RoomAuthority& authority, const char* name, int endpoint, bool host) {
    const auto joined = authority.Join(name, {"host", std::to_string(endpoint)}, host);
    EXPECT(joined.accepted);
    return joined.accepted ? joined.snapshot.members.back().id : 0;
}

static void TestDegradedCaptureAndLostReplyRetry() {
    room::RoomAuthority authority("Result outbox", 2, 91);
    const auto p1 = Join(authority, "P1", 1, true);
    const auto p2 = Join(authority, "P2", 2, false);
    EXPECT(authority.Apply(p1, ActionFor(authority, p1, room::ActionKind::Queue, 1)).accepted);
    EXPECT(authority.Apply(p2, ActionFor(authority, p2, room::ActionKind::Queue, 1)).accepted);
    EXPECT(authority.Apply(p1, ActionFor(authority, p1, room::ActionKind::Ready, 2)).accepted);
    EXPECT(authority.Apply(p2, ActionFor(authority, p2, room::ActionKind::Ready, 2)).accepted);
    EXPECT(authority.BeginMatch(0, p1, p2).accepted);

    netplay::RandomRoomId roomId{};
    roomId[0] = 0x42;
    const auto generation = authority.SnapshotView().tables[0].matchGeneration;
    netplay::MatchResultOutbox outbox;
    netplay::MatchResultCapture capture;
    capture.roomId = roomId;
    capture.roomEpoch = authority.SnapshotView().roomEpoch;
    capture.generation = generation;
    capture.table = 0;
    capture.slot = 0;
    native_result::Timeline nativeTimeline;
    native_result::Result nativeOutcome = native_result::Result::None;
    for (int frame = 100; frame < 160; ++frame) {
        nativeTimeline.Rewind(frame - 3);
        for (int replay = frame - 2; replay <= frame; ++replay)
            nativeTimeline.Capture(replay, replay >= 100 ? native_result::Flow::MatchResult : native_result::Flow::Other, 0);
        nativeOutcome = nativeTimeline.Confirmed(frame - 4);
    }
    EXPECT(nativeOutcome == native_result::Result::P1Win);
    capture.result = nativeOutcome == native_result::Result::P1Win ? room::MatchResult::P1Win : room::MatchResult::Abort;
    EXPECT(outbox.Capture(capture));
    EXPECT(!outbox.Capture(capture));
    EXPECT(outbox.Pending());
    EXPECT(outbox.Captured() && outbox.Captured()->ProfileKey() == netplay::ProfileResultKeyV2(roomId, 0, generation));

    netplay::ProfileRecord profile;
    netplay::MatchResultRoomView view;
	view.roomId = roomId;
    view.roomEpoch = authority.SnapshotView().roomEpoch;
    view.revision = authority.SnapshotView().revision;
    view.tableRevision = authority.SnapshotView().tables[0].revision;
    view.matchGeneration = generation;
    view.liveGeneration = generation;
    view.healthy = false;
    netplay::MatchResultSubmission submission;
	// Recovery may temporarily expose no usable room projection. It is not an
	// authoritative generation change until the controller is healthy again.
	view.roomEpoch = 0;
	view.matchGeneration = 0;
	view.liveGeneration = 0;
    EXPECT(outbox.Poll(view, 1000, submission) == netplay::MatchResultOutbox::PollResult::None);
	EXPECT(outbox.Pending());
    EXPECT(authority.SnapshotView().tables[0].score[0] == 0 && profile.wins == 0 && profile.losses == 0);
	const auto otherRoom = [&]() { auto value = roomId; value[1] = 1; return value; }();
	EXPECT(outbox.ObserveTerminal(otherRoom, capture.roomEpoch, 0, generation, capture.result) ==
		netplay::MatchResultOutbox::TerminalResult::Unrelated);
	EXPECT(outbox.ObserveTerminal(roomId, capture.roomEpoch + 1, 0, generation, capture.result) ==
		netplay::MatchResultOutbox::TerminalResult::Unrelated);
	EXPECT(outbox.ObserveTerminal(roomId, capture.roomEpoch, 1, generation, capture.result) ==
		netplay::MatchResultOutbox::TerminalResult::Unrelated);
	EXPECT(outbox.Pending());

	view.roomEpoch = authority.SnapshotView().roomEpoch;
    view.matchGeneration = generation;
	view.liveGeneration = generation;
    view.healthy = true;
    EXPECT(outbox.Poll(view, 1000, submission) == netplay::MatchResultOutbox::PollResult::Ready);
	outbox.MarkUnsent(1000);
	EXPECT(outbox.Poll(view, 1099, submission) == netplay::MatchResultOutbox::PollResult::None);
	EXPECT(outbox.Poll(view, 1100, submission) == netplay::MatchResultOutbox::PollResult::Ready);
    EXPECT(submission.roomEpoch == capture.roomEpoch && submission.generation == generation &&
        submission.table == 0 && submission.result == room::MatchResult::P1Win);
	// Native finish and the game result are separate terminal facts. Their
	// stable retries may interleave after a newer action advances the member
	// watermark, but each must still pass the exact generation validation path.
	auto finished = ActionFor(authority, p1, room::ActionKind::MatchFinished, 3);
	finished.matchGeneration = generation;
	EXPECT(authority.Apply(p1, finished).accepted);
	auto first = ActionFor(authority, p1, room::ActionKind::RecordResult, 4);
    first.roomEpoch = submission.roomEpoch;
    first.revision = submission.revision;
    first.tableRevision = submission.tableRevision;
    first.matchGeneration = submission.generation;
    first.result = submission.result;
    const auto firstResult = authority.Apply(p1, first);
    EXPECT(firstResult.accepted);
	const auto queuedRevision = submission.revision;
	const auto queuedTableRevision = submission.tableRevision;
    outbox.MarkQueued(first.actionId, 1100);
    // Drop the accepted reply. One report cannot award a score or update the profile.
    EXPECT(outbox.Pending() && authority.SnapshotView().tables[0].resultPending);
	EXPECT(!outbox.ObserveReply(first.actionId, false, false) && outbox.Pending());
	EXPECT(!outbox.ObserveReply(first.actionId + 100, true, false) && outbox.Pending());
    EXPECT(authority.SnapshotView().tables[0].score[0] == 0 && profile.wins == 0 && profile.losses == 0);
	auto chat = ActionFor(authority, p1, room::ActionKind::Chat, 5);
	chat.text = "result queued";
	EXPECT(authority.Apply(p1, chat).accepted);
	EXPECT(authority.Apply(p1, finished).accepted);
	auto wrongFinishedGeneration = finished; ++wrongFinishedGeneration.matchGeneration;
	const auto wrongFinished = authority.Apply(p1, wrongFinishedGeneration);
	EXPECT(!wrongFinished.accepted && wrongFinished.reason == room::RejectReason::WrongGeneration);
	auto changedOutcome = first; changedOutcome.result = room::MatchResult::P2Win;
	const auto changedOutcomeResult = authority.Apply(p1, changedOutcome);
	EXPECT(!changedOutcomeResult.accepted && changedOutcomeResult.reason == room::RejectReason::WrongPhase);
	auto changedGeneration = first; ++changedGeneration.matchGeneration;
	const auto changedGenerationResult = authority.Apply(p1, changedGeneration);
	EXPECT(!changedGenerationResult.accepted && changedGenerationResult.reason == room::RejectReason::WrongGeneration);
    EXPECT(outbox.Poll(view, 1599, submission) == netplay::MatchResultOutbox::PollResult::None);

    view.revision = authority.SnapshotView().revision;
    view.tableRevision = authority.SnapshotView().tables[0].revision;
    EXPECT(outbox.Poll(view, 1600, submission) == netplay::MatchResultOutbox::PollResult::Ready);
	EXPECT(submission.actionId == first.actionId && submission.revision == queuedRevision &&
		submission.tableRevision == queuedTableRevision);
    auto retry = ActionFor(authority, p1, room::ActionKind::RecordResult, submission.actionId);
    retry.roomEpoch = submission.roomEpoch;
    retry.revision = submission.revision;
    retry.tableRevision = submission.tableRevision;
    retry.matchGeneration = submission.generation;
    retry.result = submission.result;
    const auto duplicate = authority.Apply(p1, retry);
    EXPECT(!duplicate.accepted && duplicate.reason == room::RejectReason::DuplicateResult);
	outbox.MarkQueued(retry.actionId, 1600);
	EXPECT(outbox.Poll(view, 2099, submission) == netplay::MatchResultOutbox::PollResult::None);
	EXPECT(outbox.Poll(view, 2100, submission) == netplay::MatchResultOutbox::PollResult::Ready);
	EXPECT(submission.actionId == first.actionId);
	// The original accepted reply may arrive after later lifecycle retries. Its
	// stable ID remains the authenticated completion proof.
    EXPECT(outbox.ObserveReply(first.actionId, true, false));
    EXPECT(!outbox.Pending());
    EXPECT(authority.SnapshotView().tables[0].score[0] == 0 && profile.wins == 0 && profile.losses == 0);

    auto second = ActionFor(authority, p2, room::ActionKind::RecordResult, 3);
    second.matchGeneration = generation;
    second.result = room::MatchResult::P1Win;
    const auto committed = authority.Apply(p2, second);
    EXPECT(committed.accepted && authority.SnapshotView().tables[0].score[0] == 1);
	const auto lateFinished = authority.Apply(p1, finished);
	EXPECT(!lateFinished.accepted && lateFinished.reason == room::RejectReason::WrongGeneration);
	EXPECT(authority.SnapshotView().tables[0].score[0] == 1);
    const auto ended = std::find_if(committed.events.begin(), committed.events.end(), [](const room::Event& event) {
        return event.kind == room::Event::Kind::MatchEnded;
    });
    EXPECT(ended != committed.events.end());
    EXPECT(outbox.ObserveTerminal(roomId, capture.roomEpoch, 0, generation + 1, room::MatchResult::P1Win) ==
        netplay::MatchResultOutbox::TerminalResult::Unrelated);
    EXPECT(profile.wins == 0 && profile.losses == 0);
	EXPECT(ended != committed.events.end() && outbox.ObserveTerminal(roomId, capture.roomEpoch,
        ended->table, ended->matchGeneration, ended->result) == netplay::MatchResultOutbox::TerminalResult::Confirmed);
	EXPECT(outbox.PrepareProfileConsumption(profile) ==
		netplay::MatchResultOutbox::ProfileConsumption::PersistenceRequired);
    EXPECT(profile.wins == 1 && profile.losses == 0);
	// A failed settings write leaves the in-memory key present. Replaying the
	// same receipt still requires persistence and can retry that boundary.
	EXPECT(outbox.PrepareProfileConsumption(profile) ==
		netplay::MatchResultOutbox::ProfileConsumption::PersistenceRequired);

	netplay::ProfileRecord unavailable;
	unavailable.available = false;
	EXPECT(outbox.PrepareProfileConsumption(unavailable) ==
		netplay::MatchResultOutbox::ProfileConsumption::NoPersistenceRequired);
	EXPECT(unavailable.wins == 0 && unavailable.losses == 0 && unavailable.recent.empty());
	netplay::ProfileRecord atCapacity;
	atCapacity.wins = netplay::ProfileRecord::MaximumGames;
	EXPECT(outbox.PrepareProfileConsumption(atCapacity) ==
		netplay::MatchResultOutbox::ProfileConsumption::NoPersistenceRequired);
	EXPECT(atCapacity.wins == netplay::ProfileRecord::MaximumGames && atCapacity.losses == 0 &&
		atCapacity.recent.empty());
	netplay::ProfileRecord reachingCapacity;
	reachingCapacity.wins = netplay::ProfileRecord::MaximumGames - 1;
	EXPECT(outbox.PrepareProfileConsumption(reachingCapacity) ==
		netplay::MatchResultOutbox::ProfileConsumption::PersistenceRequired);
	EXPECT(reachingCapacity.wins == netplay::ProfileRecord::MaximumGames && reachingCapacity.recent.size() == 1);
	// The first settings write may fail after the in-memory update. Reaching
	// capacity must not convert that exact unpersisted key into a no-save ACK.
	EXPECT(outbox.PrepareProfileConsumption(reachingCapacity) ==
		netplay::MatchResultOutbox::ProfileConsumption::PersistenceRequired);

	netplay::MatchResultOutbox invalidated;
	EXPECT(invalidated.Capture(capture));
	auto wrongRoomView = view;
	wrongRoomView.roomId = otherRoom;
	EXPECT(invalidated.Poll(wrongRoomView, 2000, submission) == netplay::MatchResultOutbox::PollResult::Invalidated);
	EXPECT(!invalidated.Pending());

    // The confirmed native result must release the table for the next game
    // after both fighters consume their terminal receipts and ready again.
    for (const auto member : {p1, p2}) {
        auto ack = ActionFor(authority, member, room::ActionKind::AcknowledgeTerminal, 6);
        ack.matchGeneration = generation;
        EXPECT(authority.Apply(member, ack).accepted);
    }
    EXPECT(authority.Apply(p1, ActionFor(authority, p1, room::ActionKind::Ready, 7)).accepted);
    EXPECT(authority.Apply(p2, ActionFor(authority, p2, room::ActionKind::Ready, 7)).accepted);
    EXPECT(authority.BeginMatch(0, p1, p2).accepted);
    EXPECT(authority.SnapshotView().tables[0].matchGeneration > generation);
}

// Ledger A-005: only a write that is still in flight may hold the terminal
// receipt. A refused queue, an invalid record and a writer error all release.
static void TestProfilePersistenceNeverHoldsForever() {
    using Persistence = netplay::MatchResultOutbox::ProfilePersistence;
    netplay::MatchResultCapture capture;
    capture.roomId[0] = 0x51;
    capture.roomEpoch = 3;
    capture.generation = 7;
    capture.table = 0;
    capture.slot = 0;
    capture.result = room::MatchResult::P1Win;

    int queued = 0;
    std::uint64_t onDisk = 0;
    bool failed = false;
    std::uint64_t nextRevision = 1;
    std::uint64_t now = 1000;
    netplay::ProfileStore store;
    store.queue = [&] { ++queued; return nextRevision; };
    store.saved = [&](std::uint64_t revision) { return onDisk >= revision; };
    store.failed = [&] { return failed; };

    {
        // Queued once, waits for its revision, then is saved.
        netplay::MatchResultOutbox outbox;
        netplay::ProfileRecord profile;
        EXPECT(outbox.Capture(capture));
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Waiting);
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Waiting);
        EXPECT(queued == 1);
        EXPECT(profile.wins == 1);
        onDisk = 1;
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Saved);
        // The room replays the same receipt two or three times per match;
        // a saved record is not written again (R1).
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::NotRequired);
        EXPECT(queued == 1);
        // A new capture is a new match; its revision is already on disk here.
        capture.generation = 8;
        EXPECT(outbox.Capture(capture));
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Saved);
        EXPECT(queued == 2);
        capture.generation = 7;
    }
    {
        // The writer refuses the snapshot: release at once rather than retry.
        netplay::MatchResultOutbox outbox;
        netplay::ProfileRecord profile;
        EXPECT(outbox.Capture(capture));
        nextRevision = 0;
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Released);
        nextRevision = 5;
    }
    {
        // A write error after queueing releases too.
        netplay::MatchResultOutbox outbox;
        netplay::ProfileRecord profile;
        EXPECT(outbox.Capture(capture));
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Waiting);
        failed = true;
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Released);
        failed = false;
    }
    {
        // A write the writer accepted but never finishes releases after the
        // timeout, not never.
        netplay::MatchResultOutbox outbox;
        netplay::ProfileRecord profile;
        EXPECT(outbox.Capture(capture));
        onDisk = 0; nextRevision = 9;
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Waiting);
        now += netplay::MatchResultOutbox::ProfileWriteTimeoutMs - 1;
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Waiting);
        now += 1;
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Released);
    }
    {
        // No usable profile: nothing to write.
        netplay::MatchResultOutbox outbox;
        netplay::ProfileRecord profile;
        profile.available = false;
        EXPECT(outbox.Capture(capture));
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::NotRequired);
    }
    {
        // A draw is not a counted result, so the record is invalid: release.
        netplay::MatchResultOutbox outbox;
        netplay::ProfileRecord profile;
        auto draw = capture;
        draw.result = room::MatchResult::Draw;
        EXPECT(outbox.Capture(draw));
        EXPECT(outbox.PersistProfile(profile, store, now) == Persistence::Released);
    }
}

int main() {
    TestDegradedCaptureAndLostReplyRetry();
    TestProfilePersistenceNeverHoldsForever();
    if (TEST_FAILURES()) return 1;
    std::printf("Match result outbox tests passed.\n");
    return 0;
}
