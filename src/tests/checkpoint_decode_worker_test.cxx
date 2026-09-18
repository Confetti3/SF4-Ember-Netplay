#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "../session/CheckpointDecodeWorker.hxx"

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)

using sf4e::room::ConnectionRef;
using sf4e::session::CheckpointDecodeWorker;
using sf4e::session::EffectEnvelope;
using sf4e::session::SessionProposal;

static EffectEnvelope Effect(std::uint64_t sequence, std::uint64_t revision) {
	EffectEnvelope effect;
	effect.sequence = sequence; effect.roomEpoch = 77; effect.term = 9; effect.revision = revision;
	effect.recipient = 3; effect.endpoint = ConnectionRef{"relay", "peer-3"}; effect.type = "room_snapshot";
	effect.publicPayload = nlohmann::json{{"type", "room_snapshot"}, {"sequence", sequence}};
	effect.payloadDigest = sf4e::session::PayloadDigest(effect.publicPayload);
	return effect;
}

static std::string Proposal(std::uint64_t request) {
	SessionProposal proposal;
	proposal.request = request; proposal.term = 9; proposal.baseRevision = 5;
	proposal.checkpoint = {{"schema", "session-recovery-v2"}, {"effect_journal", std::vector<EffectEnvelope>{Effect(1, 4), Effect(2, 5)}}};
	proposal.effects = {Effect(3, 6)};
	proposal.effectsDigest = sf4e::session::EffectsDigest(proposal.effects);
	return nlohmann::json(proposal).dump();
}

static CheckpointDecodeWorker::Result Take(CheckpointDecodeWorker& worker) {
	CheckpointDecodeWorker::Result result;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!worker.TryTake(result)) {
		CHECK(std::chrono::steady_clock::now() < deadline);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return result;
}

int main() {
	// The journal handed to the import is the checkpoint's journal followed by
	// the proposal's own effects.
	const auto inlineResult = CheckpointDecodeWorker::Decode(1, Proposal(11));
	CHECK(inlineResult.ok && inlineResult.ticket == 1 && inlineResult.proposal.request == 11);
	CHECK(inlineResult.journal.size() == 3 && inlineResult.journal[2].sequence == 3);
	CHECK(inlineResult.proposal.checkpoint.at("schema") == "session-recovery-v2");

	// A proposal whose effects do not match their digest is refused, not thrown.
	auto tampered = nlohmann::json::parse(Proposal(12));
	tampered["effects"][0]["sequence"] = 99;
	CHECK(!CheckpointDecodeWorker::Decode(2, tampered.dump()).ok);
	CHECK(!CheckpointDecodeWorker::Decode(3, "not json").ok);

	{
		// Off-thread results arrive in submission order and equal the inline decode.
		CheckpointDecodeWorker worker;
		CheckpointDecodeWorker::Result none;
		CHECK(!worker.TryTake(none));
		CHECK(worker.Submit(1, Proposal(11)));
		CHECK(worker.Submit(2, "not json"));
		CHECK(worker.Submit(3, Proposal(13)));
		const auto first = Take(worker), second = Take(worker), third = Take(worker);
		CHECK(first.ticket == 1 && first.ok && first.proposal.request == 11);
		CHECK(first.journal == inlineResult.journal && first.proposal.checkpoint == inlineResult.proposal.checkpoint);
		CHECK(second.ticket == 2 && !second.ok);
		CHECK(third.ticket == 3 && third.ok && third.proposal.request == 13);
		CHECK(!worker.TryTake(none));
	}
	{
		// Destroying the owner with work still queued must not hang.
		CheckpointDecodeWorker worker;
		for (std::uint64_t ticket = 1; ticket <= 8; ++ticket) CHECK(worker.Submit(ticket, Proposal(ticket)));
	}
	std::cout << "Checkpoint decode worker passed\n";
	return 0;
}
