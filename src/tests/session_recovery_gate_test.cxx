#include <cassert>
#include <iostream>

#include "../session/SessionRecovery.hxx"

using sf4e::room::ConnectionRef;
using sf4e::session::EffectEnvelope;
using sf4e::session::SessionProposal;
using sf4e::session::SessionRecoveryGate;

int main() {
	EffectEnvelope effect;
	effect.sequence = 1;
	effect.roomEpoch = 77;
	effect.term = 9;
	effect.revision = 5;
	effect.generation = 12;
	effect.recipient = 3;
	effect.endpoint = ConnectionRef{"relay", "peer-3"};
	effect.type = "room_result";
	effect.publicPayload = nlohmann::json{{"type", "room_result"}, {"action_id", 41}};
	effect.payloadDigest = sf4e::session::PayloadDigest(effect.publicPayload);
	nlohmann::json encoded = effect;
	EffectEnvelope decoded;
	encoded.get_to(decoded);
	assert(decoded == effect);
	const auto token = sf4e::session::EffectCommitToken(effect);
	token.get_to(decoded);
	assert(decoded == effect && decoded.publicPayload.is_null());

	// Replay the live stalled-room mix: historical verification frames must
	// not grow every result checkpoint or evict the lifecycle receipt.
	std::vector<EffectEnvelope> diagnosticHistory{effect};
	for (unsigned i = 0; i < 122; ++i) {
		auto diagnostic = effect;
		diagnostic.sequence = i + 2;
		diagnostic.type = i % 2 ? "battle_hash" : "battle_snapshot";
		diagnostic.publicPayload = nullptr;
		diagnosticHistory.push_back(diagnostic);
	}
	sf4e::session::CompactEffectJournal(diagnosticHistory);
	assert(diagnosticHistory.size() == 33);
	assert(diagnosticHistory.front() == effect);
	assert(diagnosticHistory.back().sequence == 123);

	EffectEnvelope privateEffect = effect;
	privateEffect.privatePayload = true;
	privateEffect.publicPayload = nlohmann::json{{"capability", "must-not-replicate"}};
	bool rejectedPrivate = false;
	// to_json intentionally omits a private publicPayload. Construct the
	// malformed wire shape explicitly to verify the import-side rejection.
	auto malformedPrivate = encoded;
	malformedPrivate["private_payload"] = true;
	malformedPrivate["public_payload"] = privateEffect.publicPayload;
	try { malformedPrivate.get_to(decoded); }
	catch (...) { rejectedPrivate = true; }
	assert(rejectedPrivate);

	// Compaction may drop only the optional replay copy to stay within the
	// journal byte budget. That must not change the stable effect identity used
	// to authenticate a live token against the committed journal.
	EffectEnvelope oversized = effect;
	oversized.type = "room_snapshot";
	oversized.publicPayload = nlohmann::json{{"type", "room_snapshot"}, {"blob", std::string(300 * 1024, 'x')}};
	oversized.payloadDigest = sf4e::session::PayloadDigest(oversized.publicPayload);
	std::vector<EffectEnvelope> compacted{oversized};
	sf4e::session::CompactEffectJournal(compacted);
	assert(compacted.size() == 1);
	assert(compacted.front().publicPayload.is_null());
	assert(compacted.front() == oversized);

	// A current private grant may precede one unique projection for every room
	// member. Byte compaction must shed those optional replay payloads before it
	// removes the only digest capable of authenticating the separately ordered
	// game_prepare. The old front-only rule erased all four grants in this shape.
	std::vector<EffectEnvelope> grantFirst;
	for (std::uint64_t recipient = 1; recipient <= 4; ++recipient) {
		EffectEnvelope grant = effect;
		grant.sequence = recipient;
		grant.generation = 10;
		grant.recipient = recipient;
		grant.endpoint.user = "grant-" + std::to_string(recipient);
		grant.type = "game_prepare";
		grant.privatePayload = true;
		grant.publicPayload = nullptr;
		grant.payloadDigest = sf4e::session::PayloadDigest(
			nlohmann::json{{"type", "game_prepare"}, {"generation", 10}, {"recipient", recipient}});
		grantFirst.push_back(std::move(grant));
	}
	for (std::uint64_t recipient = 1; recipient <= 16; ++recipient) {
		EffectEnvelope projection = effect;
		projection.sequence = 4 + recipient;
		projection.recipient = recipient;
		projection.endpoint.user = "projection-" + std::to_string(recipient);
		projection.type = "room_snapshot";
		projection.publicPayload = nlohmann::json{{"type", "room_snapshot"},
			{"recipient", recipient}, {"blob", std::string(20 * 1024, static_cast<char>('a' + recipient))}};
		projection.payloadDigest = sf4e::session::PayloadDigest(projection.publicPayload);
		grantFirst.push_back(std::move(projection));
	}
	const auto grantFirstIdentities = grantFirst;
	sf4e::session::CompactEffectJournal(grantFirst);
	assert(grantFirst.size() == 20);
	assert(nlohmann::json(grantFirst).dump().size() <= sf4e::session::MaxEffectJournalBytes);
	assert(std::equal(grantFirst.begin(), grantFirst.end(), grantFirstIdentities.begin(),
		[](const EffectEnvelope& left, const EffectEnvelope& right) { return left == right; }));
	for (std::uint64_t recipient = 1; recipient <= 4; ++recipient) {
		const auto grant = std::find_if(grantFirst.begin(), grantFirst.end(), [&](const EffectEnvelope& item) {
			return item.type == "game_prepare" && item.generation == 10 && item.recipient == recipient;
		});
		assert(grant != grantFirst.end() && grant->privatePayload && grant->publicPayload.is_null());
	}

	std::vector<EffectEnvelope> entryBound;
	for (std::uint64_t sequence = 1; sequence <= sf4e::session::MaxEffectJournalEntries + 1; ++sequence) {
		EffectEnvelope event = effect;
		event.sequence = sequence;
		event.type = "room_event";
		event.publicPayload = nullptr;
		entryBound.push_back(std::move(event));
	}
	sf4e::session::CompactEffectJournal(entryBound);
	assert(entryBound.size() == sf4e::session::MaxEffectJournalEntries);
	assert(entryBound.front().sequence == 2 &&
		entryBound.back().sequence == sf4e::session::MaxEffectJournalEntries + 1);
	assert(sf4e::session::EffectJournalBytes(entryBound) == nlohmann::json(entryBound).dump().size());

	// A maximum valid public room projection must fit the 64 KiB control frame
	// when its commit identity is encoded once. Ordinary player text containing
	// the word "capability" is public data; only an object field with that exact
	// name is sensitive.
	sf4e::room::Snapshot maxChat;
	maxChat.roomEpoch = 77;
	maxChat.revision = 101;
	maxChat.name = "Bounded room";
	maxChat.host = maxChat.localMember = 1;
	for (std::size_t i = 0; i < sf4e::room::TableCount; ++i)
		maxChat.tables[i].id = static_cast<std::uint8_t>(i);
	for (std::uint64_t i = 1; i <= sf4e::room::MaximumMembers; ++i) {
		sf4e::room::Member member;
		member.id = i; member.name = "Member " + std::to_string(i);
		member.connection = {"iroh:" + std::string(32, '1'), std::string(64, 'a') + std::to_string(i)};
		member.host = i == 1; member.joinOrder = i;
		maxChat.members.push_back(std::move(member));
	}
	for (std::uint64_t i = 1; i <= sf4e::room::MaximumChatMessages; ++i) {
		std::string text(sf4e::room::MaximumChatBytes, 'x');
		if (i == 1) text.replace(text.size() - 10, 10, "capability");
		maxChat.chat.push_back({i, 1, std::move(text)});
	}
	const auto validatedChat = nlohmann::json(maxChat).get<sf4e::room::Snapshot>();
	nlohmann::json maxPayload{{"type", "room_snapshot"}, {"snapshot", validatedChat}};
	assert(!sf4e::session::ContainsCapabilityField(maxPayload));
	assert(sf4e::session::ContainsCapabilityField(
		nlohmann::json{{"type", "game_prepare"}, {"links", {{{"capability", "secret"}}}}}));
	EffectEnvelope maxProjection = effect;
	maxProjection.type = "room_snapshot";
	maxProjection.publicPayload = maxPayload;
	maxProjection.payloadDigest = sf4e::session::PayloadDigest(maxPayload);
	auto duplicatedWire = maxPayload;
	duplicatedWire["_commit"] = maxProjection;
	assert(duplicatedWire.dump().size() > 64 * 1024);
	auto compactWire = maxPayload;
	compactWire["_commit"] = sf4e::session::EffectCommitToken(maxProjection);
	assert(compactWire.dump().size() <= 64 * 1024);
	auto compactToken = compactWire.at("_commit").get<EffectEnvelope>();
	assert(compactToken == maxProjection && compactToken.publicPayload.is_null());

	// Existing optional projection copies may consume the candidate byte budget
	// before a later private grant. Shed copies across the whole candidate while
	// retaining every stable identity and local-delivery digest.
	std::vector<EffectEnvelope> maxBatch;
	for (std::uint64_t recipient = 1; recipient <= sf4e::room::MaximumMembers; ++recipient) {
		auto projection = maxProjection;
		projection.sequence = 100 + recipient;
		projection.recipient = recipient;
		projection.endpoint.user = "max-chat-" + std::to_string(recipient);
		maxBatch.push_back(std::move(projection));
	}
	auto privateGrant = privateEffect;
	privateGrant.sequence = 200;
	privateGrant.recipient = sf4e::room::MaximumMembers;
	privateGrant.endpoint.user = "max-chat-grant";
	privateGrant.type = "game_prepare";
	privateGrant.publicPayload = nullptr;
	privateGrant.payloadDigest = sf4e::session::PayloadDigest(
		nlohmann::json{{"type", "game_prepare"}, {"generation", 12}});
	maxBatch.push_back(privateGrant);
	const auto maxBatchIdentities = maxBatch;
	const auto shedBytes = sf4e::session::ShedOptionalEffectPayloads(
		maxBatch, sf4e::session::MaxEffectJournalBytes);
	assert(shedBytes == sf4e::session::EffectJournalBytes(maxBatch));
	assert(shedBytes <= sf4e::session::MaxEffectJournalBytes);
	assert(maxBatch.size() == maxBatchIdentities.size());
	assert(std::equal(maxBatch.begin(), maxBatch.end(), maxBatchIdentities.begin(),
		[](const EffectEnvelope& left, const EffectEnvelope& right) { return left == right; }));
	assert(maxBatch.back().privatePayload && maxBatch.back().publicPayload.is_null());

	SessionProposal proposal;
	proposal.request = 100;
	proposal.term = effect.term;
	proposal.baseRevision = 4;
	proposal.checkpoint = nlohmann::json{{"schema", "session-recovery-v2"}};
	proposal.effects.push_back(effect);
	proposal.effectsDigest = sf4e::session::EffectsDigest(proposal.effects);
	assert(proposal.Valid());

	SessionRecoveryGate gate;
	assert(!gate.Prepare(proposal));
	gate.SetAuthority(9, 4, true);
	assert(gate.Prepare(proposal));
	assert(gate.Pending());
	gate.MarkPrecommittedStarted(12);
	gate.SetAuthority(9, 4, false);
	assert(gate.Pending());
	assert(gate.WasPrecommittedStarted(12));
	assert(!gate.Commit(proposal.request, 9, 5, proposal.effectsDigest));
	gate.SetAuthority(9, 4, true);
	assert(!gate.Commit(proposal.request, 8, 5, proposal.effectsDigest));
	assert(gate.Commit(proposal.request, 9, 5, proposal.effectsDigest));
	assert(!gate.Pending());
	assert(!gate.Commit(proposal.request, 9, 6, proposal.effectsDigest));

	gate.SetAuthority(10, 0, false);
	assert(!gate.Writable());
	assert(!gate.WasPrecommittedStarted(12));

	// The spliced proposal encoding is byte-identical to the JSON library's, and
	// the effects digest over the pre-encoded array matches EffectsDigest.
	{
		auto encodedProposal = proposal;
		encodedProposal.checkpoint = nlohmann::json{{"schema", "session-recovery-v2"}, {"room", {{"name", "a \"quoted\" room"}}},
			{"effect_journal", nlohmann::json::array()}, {"members", nlohmann::json::array({1, 2, 3})}};
		const auto effects = nlohmann::json(encodedProposal.effects).dump();
		assert(sf4e::session::recovery_detail::Sha256(effects) == sf4e::session::EffectsDigest(encodedProposal.effects));
		assert(sf4e::session::EncodeSessionProposal(encodedProposal, effects) == nlohmann::json(encodedProposal).dump());
	}

	// Compacting with tracked sizes gives the same journal as re-encoding it,
	// and leaves the sizes in step, through diagnostic trimming, projection
	// supersession, replay-copy shedding and the byte bound.
	{
		std::vector<EffectEnvelope> journal;
		for (std::uint64_t i = 0; i < 300; ++i) {
			auto entry = effect;
			entry.sequence = i + 1;
			entry.recipient = 1 + i % 5;
			entry.type = i % 7 == 0 ? "battle_hash" : i % 3 == 0 ? "room_result" : "room_snapshot";
			entry.publicPayload = nlohmann::json{{"type", entry.type}, {"blob", std::string(1500 + i, 'x')}};
			entry.payloadDigest = sf4e::session::PayloadDigest(entry.publicPayload);
			journal.push_back(entry);
		}
		auto reference = journal;
		sf4e::session::CompactEffectJournal(reference);
		std::vector<std::size_t> sizes;
		for (const auto& entry : journal) sizes.push_back(nlohmann::json(entry).dump().size());
		sf4e::session::CompactEffectJournal(journal, sizes);
		assert(journal == reference && journal.size() == sizes.size());
		for (std::size_t i = 0; i < journal.size(); ++i) {
			assert(journal[i].publicPayload == reference[i].publicPayload);
			assert(sizes[i] == nlohmann::json(journal[i]).dump().size());
		}
	}

	// The CNG digest is the standard SHA-256, as is the portable reference: the
	// Rust helper checks the same digests. Includes the empty input, block
	// boundaries and a large input.
	assert(sf4e::session::recovery_detail::Sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	assert(sf4e::session::recovery_detail::Sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	for (const std::size_t size : {std::size_t(55), std::size_t(56), std::size_t(64), std::size_t(1000), std::size_t(600000)}) {
		std::string input(size, '\0');
		for (std::size_t i = 0; i < size; ++i) input[i] = static_cast<char>((i * 131 + 7) & 0xff);
		assert(sf4e::session::recovery_detail::Sha256(input) == sf4e::session::recovery_detail::Sha256Portable(input));
	}

	// The journal sizes an envelope without encoding its replay copy again, and
	// the live send splices the token into the already encoded payload. Both
	// must match what nlohmann would produce.
	{
		const nlohmann::json snapshot{{"type", "room_snapshot"}, {"snapshot", {{"revision", 7}, {"name", "Room \"one\""}}}};
		const auto encoded = snapshot.dump();
		auto replayable = effect;
		replayable.type = "room_snapshot"; replayable.privatePayload = false; replayable.publicPayload = snapshot;
		replayable.payloadDigest = sf4e::session::recovery_detail::Sha256(encoded);
		assert(sf4e::session::EncodedEnvelopeBytes(replayable, encoded.size()) == nlohmann::json(replayable).dump().size());
		assert(replayable.publicPayload == snapshot); // restored after sizing
		auto digestOnly = replayable; digestOnly.publicPayload = nullptr;
		assert(sf4e::session::EncodedEnvelopeBytes(digestOnly, encoded.size()) == nlohmann::json(digestOnly).dump().size());
		auto expected = snapshot; expected["_commit"] = sf4e::session::EffectCommitToken(replayable);
		assert(nlohmann::json::parse(sf4e::session::CommittedEffectWire(replayable, encoded)) == expected);
		auto empty = nlohmann::json::object(); empty["_commit"] = sf4e::session::EffectCommitToken(replayable);
		assert(nlohmann::json::parse(sf4e::session::CommittedEffectWire(replayable, "{}")) == empty);
	}
	std::cout << "Session recovery gate test passed\n";
	return 0;
}
