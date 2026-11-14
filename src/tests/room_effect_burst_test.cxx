// A busy room's commits reaching a member whose checkpoint has not applied yet
// (issue 231): the burst no longer fails the room, the member ends where
// applying every commit would have left it, with its events and chat, each
// checkpoint shows that commit's room, and a stream that never applies still
// fails as it did.
#include "parked_effect_room.hxx"

#include <vector>

using session::EffectEnvelope;

namespace {

// The leader's side of a run of commits: each commit's effects for this member
// sent live as it commits, in BroadcastRoomState's order (the room snapshot,
// with the chat only when it changed; a table event when there is one; the
// table's data_update), and the checkpoint the helper exports for a commit.
struct CommitStream {
	ParkedEffectRoom& member;
	std::map<std::uint64_t, std::vector<EffectEnvelope>> commits;
	std::vector<json> sent;

	void Commit(std::uint64_t revision, bool chat, bool event) {
		json snapshot{{"marker", revision}};
		if (chat) snapshot["chat"] = json::array({revision});
		else snapshot["chat_unchanged"] = true;
		auto& effects = commits[revision];
		effects.push_back(member.Committed(revision, revision * 4, json{{"type", "room_snapshot"}, {"snapshot", snapshot}}));
		if (event) effects.push_back(member.Committed(revision, revision * 4 + 1, json{{"type", "room_event"}, {"marker", revision}}));
		effects.push_back(member.Committed(revision, revision * 4 + 2, json{{"type", "data_update"}, {"marker", revision}}));
		for (const auto& effect : effects) {
			member.SendLive(session::CommittedEffectWire(effect, effect.publicPayload.dump()));
			sent.push_back(effect.publicPayload);
		}
	}
	// The newest commit's checkpoint, as the helper sends it to a member that
	// is behind: the journal of every earlier commit and its own effects.
	void Checkpoint(std::uint64_t revision) {
		std::vector<EffectEnvelope> journal;
		for (const auto& commit : commits)
			if (commit.first < revision) journal.insert(journal.end(), commit.second.begin(), commit.second.end());
		member.Transfer(revision, member.Proposal(revision, journal, commits.at(revision)));
	}
};

// What SessionClient keeps of the room state it is given: the newest snapshot
// (an older one is ignored), the chat of the last one that carried it, the
// last data_update, and every event in order.
struct ClientRoom {
	std::uint64_t snapshot = 0, data = 0;
	json chat;
	std::vector<std::uint64_t> events;
	void Apply(const json& message) {
		const auto type = message.at("type").get<std::string>();
		if (type == "room_snapshot") {
			const auto& body = message.at("snapshot");
			if (body.at("marker").get<std::uint64_t>() < snapshot) return;
			snapshot = body.at("marker").get<std::uint64_t>();
			if (!body.value("chat_unchanged", false)) chat = body.at("chat");
		} else if (type == "data_update") data = message.at("marker").get<std::uint64_t>();
		else if (type == "room_event") events.push_back(message.at("marker").get<std::uint64_t>());
	}
};

// The room payloads delivered, without verification frames.
std::vector<json> RoomPayloads(const std::vector<session::Message>& received) {
	std::vector<json> payloads;
	for (const auto& message : received) {
		auto payload = json::parse(message.payload);
		CHECK(!payload.contains("_commit"));
		if (!session::IsVerificationType(payload.value("type", std::string()))) payloads.push_back(std::move(payload));
	}
	return payloads;
}

// Stages the checkpoint sent last, then applies it as the runtime does.
void Apply(ParkedEffectRoom& member, SessionServer& replica, session::RoomRecoveryRuntime& recovery, std::uint64_t revision) {
	session::IrohRoom::CommittedCheckpoint staged;
	for (int tick = 0; tick < 5000 && !member.room->TakeCommittedCheckpoint(staged); ++tick) {
		member.room->Poll();
		member.pipe.Drain();
		Sleep(1);
	}
	CHECK(member.room->TakeCommittedCheckpoint(staged) && staged.identity.revision == revision);
	CHECK(recovery.Tick(replica, *member.room));
	CHECK(recovery.AppliedRevision() == revision && recovery.Error().empty() && !member.room->CheckpointPending());
}

} // namespace

// 80 commits, a verification frame every other one as in a fight, while this
// member's checkpoint has not applied: 202 live messages behind a parked head,
// where 64 used to fail the room. It keeps ten room messages, and once the
// newest checkpoint applies it shows the same room, chat and events as
// applying every commit in turn would.
static void TestBurstBehindParkedCheckpoint() {
	ParkedEffectRoom member;
	CommitStream leader{member};
	std::vector<session::Message> received;
	for (std::uint64_t revision = 1; revision <= 80; ++revision) {
		leader.Commit(revision, revision == 30, revision == 40);
		if (revision % 2 == 0) member.SendLive(json{{"type", "battle_hash"}, {"frameIdx", revision}}.dump());
		if (revision % 8 == 0) member.Settle(received, 4);
	}
	member.Settle(received);
	CHECK(received.empty() && member.room->GetState() != session::IrohRoom::State::Failed);
	// The first snapshot and data_update (the head), the snapshot with the
	// chat, the run up to the event, the event, and the run after it.
	const auto parked = member.room->RecoveryState();
	CHECK(parked.clientQueueMessages == 10 + session::MaximumQueuedVerification);
	CHECK(parked.clientHeadCommitted && parked.clientHeadRevision == 1 && parked.clientHeadType == "room_snapshot");

	const auto replica = member.fixture.Replica();
	session::RoomRecoveryRuntime recovery;
	leader.Checkpoint(80);
	Apply(member, *replica, recovery, 80);
	member.Settle(received);
	const auto delivered = RoomPayloads(received);
	// What the gate lets through: the snapshot with the chat, the event, and
	// the newest state; the superseded state the journal no longer holds is
	// dropped there.
	CHECK(delivered.size() == 4);
	ClientRoom client, reference;
	for (const auto& payload : delivered) client.Apply(payload);
	for (const auto& payload : leader.sent) reference.Apply(payload);
	CHECK(client.snapshot == 80 && client.data == 80 && client.chat == json::array({30}));
	CHECK(client.snapshot == reference.snapshot && client.data == reference.data && client.chat == reference.chat);
	CHECK(client.events == reference.events && client.events == std::vector<std::uint64_t>{40});
	CHECK(member.room->RecoveryState().clientQueueMessages == 0);
}

// The helper sends a member that is behind the newest commit it has, so a
// checkpoint can apply while later commits are still arriving. Commit 20 then
// shows commit 20's room, from its replay, although its live state was
// replaced in the queue by commit 50's, which waits for its own checkpoint.
static void TestEachCheckpointShowsItsRoom() {
	ParkedEffectRoom member;
	CommitStream leader{member};
	std::vector<session::Message> received;
	for (std::uint64_t revision = 1; revision <= 50; ++revision) {
		leader.Commit(revision, false, false);
		if (revision % 8 == 0) member.Settle(received, 4);
	}
	member.Settle(received);
	CHECK(received.empty() && member.room->RecoveryState().clientQueueMessages == 4);

	const auto replica = member.fixture.Replica();
	session::RoomRecoveryRuntime recovery;
	leader.Checkpoint(20);
	Apply(member, *replica, recovery, 20);
	member.Settle(received);
	auto delivered = RoomPayloads(received);
	CHECK(delivered.size() == 2);
	if (delivered.size() == 2) {
		CHECK(delivered[0].at("type") == "room_snapshot" && delivered[0].at("snapshot").at("marker") == 20);
		CHECK(delivered[1].at("type") == "data_update" && delivered[1].at("marker") == 20);
	}
	const auto waiting = member.room->RecoveryState();
	CHECK(waiting.clientQueueMessages == 2 && waiting.clientHeadRevision == 50);

	received.clear();
	leader.Checkpoint(50);
	Apply(member, *replica, recovery, 50);
	member.Settle(received);
	delivered = RoomPayloads(received);
	CHECK(delivered.size() == 2);
	if (delivered.size() == 2) {
		CHECK(delivered[0].at("snapshot").at("marker") == 50 && delivered[1].at("marker") == 50);
	}
}

// A stream whose checkpoints never apply still fails the room, under the same
// name, once its newest commit is MaximumParkedCommits past the one at the
// head: superseded state no longer counts towards 64 messages, so the wait is
// bounded by commits instead.
static void TestStuckStreamStillFails() {
	ParkedEffectRoom member;
	CommitStream leader{member};
	std::vector<session::Message> received;
	for (std::uint64_t revision = 1; revision <= 1 + session::MaximumParkedCommits; ++revision) {
		leader.Commit(revision, false, false);
		if (revision % 8 == 0) member.Settle(received, 4);
	}
	member.Settle(received);
	CHECK(received.empty() && member.room->GetState() != session::IrohRoom::State::Failed);
	CHECK(member.room->RecoveryState().clientQueueMessages == 4);
	leader.Commit(2 + session::MaximumParkedCommits, false, false);
	for (int tick = 0; tick < 50 && member.room->GetState() != session::IrohRoom::State::Failed; ++tick) {
		member.client->Poll(received, 8);
		member.pipe.Drain();
		Sleep(1);
	}
	CHECK(member.room->GetState() == session::IrohRoom::State::Failed);
	CHECK(member.room->Error() == "room_client_receive_queue");
	CHECK(received.empty());
}

// The tag a committed effect is queued with: its token's place in the journal
// and, for room state, which kind it is. A token that does not read leaves the
// message unmarked, for the gate to drop.
static void TestCommitTokenTags() {
	ParkedEffectRoom member;
	const auto tagOf = [&](const json& payload, std::uint64_t revision) {
		const auto effect = member.Committed(revision, revision * 4 + 3, payload);
		return session::ClassifyRoomMessage(session::CommittedEffectWire(effect, payload.dump()));
	};
	const auto withChat = tagOf(json{{"type", "room_snapshot"}, {"snapshot", {{"chat", json::array()}}}}, 5);
	CHECK(withChat.committed && withChat.projection == session::RoomProjection::SnapshotWithChat);
	CHECK(withChat.commitTerm == 1 && withChat.commitRevision == 5 && withChat.commitSequence == 23 &&
		withChat.commitRecipient == member.local.member);
	const auto unchanged = tagOf(json{{"type", "room_snapshot"}, {"snapshot", {{"chat_unchanged", true}}}}, 6);
	CHECK(unchanged.projection == session::RoomProjection::Snapshot && unchanged.commitRevision == 6);
	CHECK(tagOf(json{{"type", "data_update"}, {"marker", 1}}, 7).projection == session::RoomProjection::TableData);
	const auto event = tagOf(json{{"type", "room_event"}}, 8);
	CHECK(event.projection == session::RoomProjection::None && event.commitRevision == 8);
	auto malformed = json::parse(session::CommittedEffectWire(member.Projection(9), member.Projection(9).publicPayload.dump()));
	malformed["_commit"]["revision"] = "nine";
	const auto unread = session::ClassifyRoomMessage(malformed);
	CHECK(unread.committed && unread.projection == session::RoomProjection::None && unread.commitRevision == 0 &&
		unread.commitSequence == 0);
	CHECK(session::ClassifyRoomMessage(json{{"type", "battle_hash"}}).commitRevision == 0);
}

int main() {
	TestCommitTokenTags();
	TestBurstBehindParkedCheckpoint();
	TestEachCheckpointShowsItsRoom();
	TestStuckStreamStillFails();
	std::cout << "Room effect burst tests passed\n";
}
