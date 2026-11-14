#pragma once
#include "session_server_transport_support.hxx"

// A member's own game behind the scripted helper, with the committed leader
// as a peer, for the client queue's effect gate: live effects arrive from the
// leader, commits arrive as transfers, and RoomRecoveryRuntime activates them
// against a replica server.
struct ParkedEffectRoom {
	ParticipantRoom fixture{SetupStage::FightersPrepared};
	static constexpr session::Connection self = 2;
	json checkpoint;
	session::RosterRow local;
	FakeHelperPipe pipe;
	platform::HelperClient helper;
	std::shared_ptr<session::IrohRoom> room;
	std::unique_ptr<session::ClientTransport> client;
	const std::array<std::uint8_t, 16> id{{9}};
	const std::string leader = std::string(64, 'a');
	std::int64_t nextMessage = 2;

	// `leaderLocal`: the helper reports this game's room as the committed
	// leader, so it may propose checkpoints of its own.
	explicit ParkedEffectRoom(bool leaderLocal = false) {
		checkpoint = fixture.server.RecoveryCheckpoint();
		CHECK(session::ActiveRosterRow(checkpoint.at("members"), ParticipantRoom::IdentityOf(self), local) && local.member);
		pipe.Connect(helper);
		room = std::make_shared<session::IrohRoom>(helper);
		CHECK(room->Join("invitation", "build"));
		client = room->Client();
		CHECK(client != nullptr);
		std::map<std::string, std::uint64_t> incarnations{{leader, 1}};
		for (const auto& row : checkpoint.at("members"))
			if (!row.value("frozen", false))
				incarnations[row.at("data").at("authenticatedEndpoint").get<std::string>()] = row.at("incarnation").get<std::uint64_t>();
		pipe.Send({{"type", "status"}, {"request_id", 1}, {"endpoint", ParticipantRoom::IdentityOf(self)}, {"fixed_port", 0}});
		pipe.Send({{"type", "coordination_state"}, {"epoch", 1}, {"room", id}, {"term", 1}, {"revision", 2},
			{"incarnation", ParticipantRoom::IncarnationOf(self)}, {"leader", leader}, {"writable", true}, {"leader_local", leaderLocal}});
		pipe.Send({{"type", "control_rebound"}, {"epoch", 1}, {"room", id}, {"leader", leader},
			{"member_incarnations", incarnations}, {"members", json::array({leader})}});
	}
	// An effect with `payload` for `recipient` (this member when 0), committed at
	// `revision` in term 1, keeping its payload as the public replay copy and
	// marked as the leader's JournalEffect marks it.
	session::EffectEnvelope Committed(std::uint64_t revision, std::uint64_t sequence, json payload,
		room::MemberId recipient = 0) const {
		session::EffectEnvelope effect;
		effect.sequence = sequence; effect.roomEpoch = 9; effect.term = 1; effect.revision = revision;
		effect.recipient = recipient ? recipient : local.member;
		effect.endpoint = recipient ? room::ConnectionRef{"relay", "member-" + std::to_string(recipient)} : local.endpoint;
		effect.type = payload.value("type", std::string());
		effect.chat = effect.type == "room_snapshot" && session::CarriesRoomChat(payload);
		effect.publicPayload = std::move(payload);
		effect.payloadDigest = session::PayloadDigest(effect.publicPayload);
		return effect;
	}
	// This member's data_update committed at `revision` in term 1.
	session::EffectEnvelope Projection(std::uint64_t revision) const {
		return Committed(revision, revision, json{{"type", "data_update"}, {"marker", revision}});
	}
	std::string Proposal(std::uint64_t revision, const std::vector<session::EffectEnvelope>& journal,
		const std::vector<session::EffectEnvelope>& effects) const {
		session::SessionProposal proposal;
		proposal.request = revision; proposal.term = 1; proposal.baseRevision = revision - 1;
		proposal.checkpoint = checkpoint;
		proposal.checkpoint["effect_journal"] = journal;
		proposal.effects = effects;
		proposal.effectsDigest = session::EffectsDigest(proposal.effects);
		return json(proposal).dump();
	}
	// The leader's live send of one committed effect, or another member's
	// message on its own control when `from` names it.
	void SendLive(const std::string& wire, const std::string& from = {}) {
		pipe.Send({{"type", "message"}, {"epoch", 1}, {"peer", from.empty() ? leader : from},
			{"message_id", nextMessage++}, {"payload", wire}});
	}
	void Transfer(std::uint64_t revision, const std::string& bytes) {
		const json transfer{{"epoch", 1}, {"room", id}, {"transfer", revision}, {"term", 1},
			{"base_revision", revision - 1}, {"revision", revision}};
		auto begin = transfer; begin["type"] = "checkpoint_begin";
		begin["length"] = bytes.size(); begin["digest"] = coordination::Sha256(bytes);
		pipe.Send(begin);
		for (std::size_t offset = 0; offset < bytes.size(); offset += coordination::CheckpointChunk) {
			auto chunk = transfer; chunk["type"] = "checkpoint_chunk";
			chunk["offset"] = offset; chunk["data"] = coordination::Encode(bytes.substr(offset, coordination::CheckpointChunk));
			pipe.Send(chunk);
		}
		auto end = begin; end["type"] = "checkpoint_end"; pipe.Send(end);
		auto committed = begin; committed["type"] = "checkpoint_committed"; pipe.Send(committed);
	}
	// Polls the client long enough for every helper event sent so far to be
	// handled (the room handles at most 8 per poll).
	void Settle(std::vector<session::Message>& received, int polls = 50) {
		for (int tick = 0; tick < polls; ++tick) {
			CHECK(client->Poll(received, 8));
			pipe.Drain();
			Sleep(1);
		}
	}
};
