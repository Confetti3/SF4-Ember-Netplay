// SessionServer: the effect journal, quorum proposals and member rebinding.

#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <exception>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "sf4e__SessionProtocol.hxx"
#include "sf4e__SessionServer.hxx"
#include "RoomMessageQueue.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
namespace session = sf4e::session;
namespace room = sf4e::room;
namespace diag = sf4e::diag;
using Dimps::Math::FixedPoint;
using sf4e::SessionServer;

namespace {
constexpr std::size_t kRecoveryProposalBytes = 1024 * 1024;
}

void SessionServer::BeginRecoveryCandidate() {
	if (!_recovery.Enabled() || _recoveryCandidateReady || _recovery.PendingProposal()) return;
	try {
		_recoveryBaseline = RecoveryCheckpoint();
		_recoveryBaselinePermits = _roomAuthority->PermitAges();
		_recoveryBaselineBindings.clear();
		for (const auto& row : clients) {
			const auto member = roomMembers.find(row.conn);
			const auto cid = cidMap.find(row.conn);
			if (member != roomMembers.end() && cid != cidMap.end())
				_recoveryBaselineBindings.emplace_back(member->second, row.conn, cid->second,
					row.data.incarnation ? row.data.incarnation : 1);
		}
		_recoveryProjection = _roomAuthority->SnapshotCopy();
		_recoveryServerOwned = {_roomAuthority->KickedAccounts(), _roomAuthority->WonGames()};
		_hasRecoveryProjection = true;
		_candidate = {};
		_recoveryCandidateReady = true;
	} catch (const std::exception&) {
		_recoveryCandidateReady = false;
	}
}

void SessionServer::FinishRecoveryCandidate() {
	if (!_recovery.Enabled() || !_recoveryCandidateReady) return;
	// A no-op poll must not hold the transport behind a proposal that can never
	// produce an effect.  A timer tick with an outstanding relative result age
	// is still a logical mutation and therefore remains a candidate; the only
	// discarded case here is an exact checkpoint match.
	try {
		if (_candidate.effects.empty() && RecoveryCheckpoint() == _recoveryBaseline) {
			DropRecoveryCandidate();
			return;
		}
	} catch (...) {
		// Keep the candidate private and fail closed if comparison itself fails.
	}
	// A candidate remains private until the helper commits its exact effect
	// digest. Step() will not consume another poll while this flag is set.
}

void SessionServer::DropRecoveryCandidate() {
	// Used only when the private poll/timer produced no logical mutation. The
	// live authority and native capabilities are already unchanged, so
	// round-tripping the portable checkpoint here would destroy local grants
	// and rebase an otherwise healthy monotonic clock.
	_recovery.Discard();
	_candidate = {};
	_recoveryCandidateReady = false;
	_recoveryBaseline.clear();
	_recoveryBaselineBindings.clear();
	_hasRecoveryProjection = false;
	_cancellationCandidate = false;
}

bool SessionServer::RestoreRecoveryBaseline() {
	if (!_recoveryCandidateReady || !_recoveryBaseline.is_object()) return true;
	const auto bindings = _recoveryBaselineBindings;
	const bool restored = RestoreRecoveryCheckpoint(_recoveryBaseline);
	const bool rebound = restored && RebindMembers(bindings);
	_candidate = {}; _recoveryCandidateReady = false; _recoveryBaseline.clear();
	_recoveryBaselineBindings.clear();
	_cancellationCandidate = false;
	return restored && rebound;
}

void SessionServer::SetAuthority(std::uint64_t term, std::uint64_t revision, bool writable,
	bool coordinationHealthy) {
	const auto before = _recovery.Authority();
	const bool becameNonWritable = before.term && before.writable && !writable;
	// A passive follower is routinely reported as non-writable while it is
	// still healthy.  Treating that flag alone as a leadership loss would
	// erase a prepared native authority before the committed checkpoint has
	// arrived.  A term transition is the durable loss signal; the helper's
	// higher-term owner then drives any required cancellation through a new
	// candidate.
	const bool lost = before.term != 0 && term != before.term;
	if (lost) {
		_preparationCancellationRequested = true;
		if (!_recoveryCandidateReady && !_recovery.PendingProposal()) RememberInterruptedPreparations();
	}
	if (lost && _recoveryCandidateReady && !RestoreRecoveryBaseline()) _transportFailed = true;
	_recovery.SetAuthority(term, revision, writable);
	_coordinationHealthy = writable || coordinationHealthy;
	if (!_coordinationHealthy) _passiveTimerClock = 0;
	// Same-term follower transitions do not enter the term-loss branch above,
	// but they still suspend result/chat monotonic clocks.  Pause immediately
	// at the authority boundary so a 30-second dispute cannot mature between
	// the state watch and the next passive tick.
	if (becameNonWritable && _roomAuthority && !_roomAuthority->RecoveryPaused())
		_roomAuthority->PauseForRecovery();
	if (lost) CancelPrecommittedGenerations();
}

bool SessionServer::IsInterruptedPreparation(std::uint8_t table, std::uint64_t generation) const {
	return std::any_of(_pendingInterruptedPreparations.begin(), _pendingInterruptedPreparations.end(),
		[&](const InterruptedPreparation& pending) { return pending.table == table && pending.generation == generation; });
}

void SessionServer::RememberInterruptedPreparations() {
	if (!_roomAuthority) return;
	const auto& snapshot = _roomAuthority->SnapshotView();
	for (std::uint8_t table = 0; table < room::TableCount; ++table) {
		const auto* authority = _roomMatchAuthorities[table].get();
		if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle ||
			authority->GetPhase() == session::MatchAuthority::Phase::Started || !authority->Generation()) continue;
		if (snapshot.tables[table].phase != room::TablePhase::Playing ||
			snapshot.tables[table].matchGeneration != authority->Generation()) continue;
		if (!IsInterruptedPreparation(table, authority->Generation()))
			_pendingInterruptedPreparations.push_back({table, authority->Generation()});
	}
}

void SessionServer::CancelPrecommittedGenerations() {
	for (std::uint8_t table = 0; table < room::TableCount; ++table) {
		auto& authority = _roomMatchAuthorities[table];
		// Keep a committed preparation alive until the new owner can journal its
		// game_end.  The stable table/generation record survives the term change;
		// uncommitted candidates have no pending record and can be discarded.
		if (authority && !IsInterruptedPreparation(table, authority->Generation())) authority->CancelPreparation();
	}
	if (_matchAuthority) _matchAuthority->CancelPreparation();
}

void SessionServer::JournalEffect(session::Connection client, const json& payload, bool retainPublicReplay) {
	if (!_recovery.Enabled() || _recoveryFlushing) {
		if (!_transport || !_transport->Send(client, payload.dump())) _transportFailed = true;
		return;
	}
	if (!_recoveryCandidateReady) BeginRecoveryCandidate();
	if (!_recoveryCandidateReady) return;
	diag::ScopedTimer timer(diag::OP_ROOM_JOURNAL);
	room::MemberId member = 0;
	const auto iter = roomMembers.find(client);
	if (iter != roomMembers.end()) member = iter->second;
	if (!member || !_roomAuthority) return; // no recipient path for an unauthenticated handle
	const auto findMember = [&](room::MemberId id) -> const room::Member* {
		for (const auto& value : _roomAuthority->SnapshotView().members) if (value.id == id) return &value;
		return nullptr;
	};
	const auto* roomMember = findMember(member);
	room::ConnectionRef endpoint;
	if (roomMember) {
		endpoint = roomMember->connection;
	} else {
		// Leave/Kick mutates RoomAuthority before its neutral response is
		// journaled. Resolve the recipient from the candidate's stable baseline,
		// never from the now-shortened public roster.
		if (!_recoveryBaseline.is_object() || !_recoveryBaseline.contains("members") ||
			!_recoveryBaseline["members"].is_array()) return;
		const auto baseline = std::find_if(_recoveryBaseline["members"].begin(), _recoveryBaseline["members"].end(),
			[&](const json& row) { return row.value("member", room::MemberId(0)) == member; });
		if (baseline == _recoveryBaseline["members"].end()) return;
		endpoint = baseline->value("endpoint", room::ConnectionRef{});
		if (endpoint.host.empty() || endpoint.user.empty()) return;
	}
	session::EffectEnvelope envelope;
	envelope.sequence = _nextEffectSequence++;
	envelope.roomEpoch = _roomAuthority->SnapshotView().roomEpoch;
	envelope.term = _recovery.Authority().term;
	envelope.revision = _recovery.Authority().revision + 1;
	envelope.generation = payload.value("generation", std::uint64_t(0));
	envelope.recipient = member;
	envelope.endpoint = endpoint;
	envelope.type = payload.value("type", std::string());
	// Encoded once: for the digest here, the journal size below, and the live
	// send in ApplyCommit.
	std::string encoded = payload.dump();
	envelope.payloadDigest = session::recovery_detail::Sha256(encoded);
	const auto type = envelope.type;
	const bool publicReplay = type == "room_snapshot" || type == "room_result" || type == "room_event" ||
		type == "data_update" || type == "game_start" || type == "game_end" || type == "game_peer_end";
	// game_connect remains a private native transition. Pair capabilities and
	// connection setup are never replayed from a replicated payload.
	envelope.privatePayload = !publicReplay || session::ContainsCapabilityField(payload);
	if (!envelope.privatePayload && retainPublicReplay) envelope.publicPayload = payload;
	auto& effects = _candidate.effects;
	if (type == "room_snapshot" || type == "data_update") {
		for (std::size_t i = 0; i < effects.size();) {
			if (effects[i].envelope.recipient == envelope.recipient && effects[i].envelope.type == type) {
				_candidate.bytes -= effects[i].envelopeBytes;
				if (effects.size() > 1) --_candidate.bytes;
				effects.erase(effects.begin() + i);
			} else ++i;
		}
	}
	for (const auto& effect : effects) {
		const auto& prior = effect.envelope;
		if (prior.recipient == envelope.recipient && prior.revision == envelope.revision && prior.type == envelope.type && prior.payloadDigest == envelope.payloadDigest) return;
	}
	auto envelopeBytes = session::EncodedEnvelopeBytes(envelope, encoded.size());
	const auto appended = [&](std::size_t bytes) { return _candidate.bytes + (effects.empty() ? 0 : 1) + bytes; };
	const auto fits = [&](std::size_t bytes) {
		return effects.size() + 1 <= session::MaxEffectJournalEntries && appended(bytes) <= session::MaxEffectJournalBytes;
	};
	// A projection's replay copy is optional; its digest still authenticates
	// the live delivery. When the candidate is full, drop this copy rather than
	// run the whole-candidate pass below, which re-encodes every entry and in a
	// large room would run again for each remaining recipient. That pass is for
	// making room for a record that must be kept whole.
	if (!fits(envelopeBytes) && session::SupersedableProjection(envelope) && !envelope.publicPayload.is_null()) {
		envelope.publicPayload = nullptr;
		envelopeBytes = session::EncodedEnvelopeBytes(envelope, encoded.size());
	}
	if (fits(envelopeBytes)) {
		_candidate.bytes = appended(envelopeBytes);
		effects.push_back({std::move(envelope), client, std::move(encoded), envelopeBytes});
		return;
	}
	auto prospective = _candidate.Envelopes();
	prospective.push_back(envelope);
	const auto prospectiveBytes = session::ShedOptionalEffectPayloads(prospective, session::MaxEffectJournalBytes);
	if (prospective.size() > session::MaxEffectJournalEntries || prospectiveBytes > session::MaxEffectJournalBytes) {
		_candidate.overflow = true;
		return;
	}
	// Existing local payloads remain intact; only an envelope's optional replay
	// copy may have been shed by the whole-candidate pass.
	effects.push_back({session::EffectEnvelope{}, client, std::move(encoded), 0});
	for (std::size_t i = 0; i < effects.size(); ++i) {
		effects[i].envelope = std::move(prospective[i]);
		effects[i].envelopeBytes = nlohmann::json(effects[i].envelope).dump().size();
	}
	_candidate.bytes = prospectiveBytes;
}

bool SessionServer::ValidateEffectRecipient(const session::EffectEnvelope& effect, session::Connection candidate, session::Connection& local) const {
	if (!_roomAuthority || !effect.recipient || effect.endpoint.host.empty() || effect.endpoint.user.empty()) return false;
	const auto currentEpoch = _roomAuthority->SnapshotView().roomEpoch;
	if (effect.roomEpoch && effect.roomEpoch != currentEpoch) return false;
	const auto member = std::find_if(_roomAuthority->SnapshotView().members.begin(), _roomAuthority->SnapshotView().members.end(),
		[&](const room::Member& value) { return value.id == effect.recipient && value.connection.host == effect.endpoint.host && value.connection.user == effect.endpoint.user; });
	if (member != _roomAuthority->SnapshotView().members.end()) {
		for (const auto& mapping : roomMembers) {
			if (mapping.second != effect.recipient) continue;
			const auto cid = cidMap.find(mapping.first);
			if (cid != cidMap.end() && cid->second.host == effect.endpoint.host && cid->second.user == effect.endpoint.user) { local = mapping.first; return true; }
		}
	}
	// A Leave or Kick response is journaled before the member is removed from
	// the live roster.  Preserve that one response using the stable baseline
	// row and the original local handle; never resolve a departed member from
	// the current room snapshot or by a newly rebound numeric handle.
	if (!_recoveryBaseline.is_object() || !_recoveryBaseline.contains("members") ||
		!_recoveryBaseline["members"].is_array()) return false;
	bool baselineMatch = false;
	for (const auto& row : _recoveryBaseline["members"]) {
		if (row.value("member", room::MemberId(0)) != effect.recipient) continue;
		const auto endpoint = row.value("endpoint", room::ConnectionRef{});
		baselineMatch = endpoint == effect.endpoint;
		break;
	}
	if (!baselineMatch || !candidate) return false;
	const auto currentCid = cidMap.find(candidate);
	if (currentCid != cidMap.end()) {
		if (currentCid->second == SessionProtocol::ConnectionID{effect.endpoint.host, effect.endpoint.user}) {
			local = candidate; return true;
		}
		return false; // the numeric handle has already been rebound to another peer
	}
	const auto currentClient = std::find_if(clients.begin(), clients.end(),
		[&](const SessionMember& row) { return row.conn == candidate; });
	if (currentClient != clients.end()) return false;
	local = candidate;
	return true;
}

bool SessionServer::ProposeCheckpoint(std::uint64_t request, std::uint64_t term, std::uint64_t baseRevision, const json& checkpoint) {
	if (!_recoveryCandidateReady || !_recovery.Writable() || term != _recovery.Authority().term || baseRevision != _recovery.Authority().revision || request == 0) return false;
	if (_candidate.overflow) return false;
	json local;
	try { local = RecoveryCheckpoint(); } catch (...) { return false; }
	// Root may attach its own metadata, but state must be byte-for-byte the
	// candidate produced by this owner before it can be committed.
	if (!checkpoint.is_null() && checkpoint != local) return false;
	session::SessionProposal proposal;
	proposal.request = request; proposal.term = term; proposal.baseRevision = baseRevision;
	proposal.checkpoint = std::move(local); proposal.effects = _candidate.Envelopes();
	// The effects are encoded once, for their digest and inside the proposal.
	const auto encodedEffects = json(proposal.effects).dump();
	proposal.effectsDigest = session::recovery_detail::Sha256(encodedEffects);
	proposal.encoded = session::EncodeSessionProposal(proposal, encodedEffects);
	if (proposal.encoded.size() > kRecoveryProposalBytes) return false;
	return _recovery.Prepare(std::move(proposal));
}

bool SessionServer::ApplyCommit(std::uint64_t request, std::uint64_t term, std::uint64_t revision,
	const json& committedCheckpoint, const session::EffectDigest& effectsDigest) {
	const auto pending = _recovery.PendingProposal();
	if (!pending || !_recovery.Writable() || term != _recovery.Authority().term || request != pending->request ||
		effectsDigest != pending->effectsDigest || revision <= _recovery.Authority().revision) return false;
	if (!committedCheckpoint.is_null() && committedCheckpoint != pending->checkpoint) return false;
	if (!_recovery.Commit(request, term, revision, effectsDigest)) return false;
	_recoveryFlushing = true;
	{
		diag::ScopedTimer sendTimer(diag::OP_ROOM_COMMIT_SEND);
		for (const auto& effect : _candidate.effects) {
			session::Connection local = effect.local;
			if (!ValidateEffectRecipient(effect.envelope, effect.local, local)) continue; // departed member; helper owns the removal decision
			// The client still consumes the original protocol object.  The commit
			// envelope is additive and gives the recipient enough immutable material
			// to reject a stale/mutated replay before dispatching the payload. The
			// token has the same shape as a checkpoint journal entry, so the
			// recipient parses one form for both.
			if (!_transport || !_transport->Send(local, session::CommittedEffectWire(effect.envelope, effect.encoded))) _transportFailed = true;
		}
	}
	_recoveryFlushing = false;
	for (auto& client : clients) {
		const auto sent = _candidate.chatSent.find(client.conn);
		if (sent != _candidate.chatSent.end()) { client.chatSent = true; client.chatVersion = sent->second; }
	}
	diag::ScopedTimer compactTimer(diag::OP_ROOM_COMPACT);
	// Sizes are unknown once after a restore; measure the inherited journal once.
	if (_committedEffectSizes.size() != _committedEffectHistory.size()) {
		_committedEffectSizes.clear();
		for (const auto& effect : _committedEffectHistory) _committedEffectSizes.push_back(json(effect).dump().size());
	}
	for (auto& effect : _candidate.effects) {
		_committedEffectHistory.push_back(std::move(effect.envelope));
		_committedEffectSizes.push_back(effect.envelopeBytes);
	}
	// Keep lifecycle/final-result effects in order while compacting only
	// supersedable projections. The same helper is used by the root recovery
	// bridge when it merges a checkpoint journal with a committed proposal.
	session::CompactEffectJournal(_committedEffectHistory, _committedEffectSizes);
	_candidate = {}; _recoveryCandidateReady = false; _recoveryBaseline.clear(); _recoveryBaselineBindings.clear();
	_hasRecoveryProjection = false;
	if (_cancellationCandidate) {
		// ApplyCommit has already advanced the authoritative revision. The
		// teardown records are now durable and must not be repeated on the next
		// handoff.
		_pendingInterruptedPreparations.clear();
		_preparationCancellationRequested = false;
		_cancellationCandidate = false;
	}
	return !_transportFailed;
}

void SessionServer::DiscardProposal() {
	_recovery.Discard();
	if (!RestoreRecoveryBaseline()) _transportFailed = true;
}

bool SessionServer::CancelInterruptedPreparations() {
	if (!_roomAuthority || !_recovery.Enabled() || !_recovery.Writable() ||
		_recoveryCandidateReady || _recovery.PendingProposal()) return false;
	if (!_preparationCancellationRequested) return true;
	if (_pendingInterruptedPreparations.empty()) {
		_preparationCancellationRequested = false;
		return true;
	}

	struct PendingTable {
		std::uint8_t table = 0;
		std::uint64_t generation = 0;
		session::MatchAuthority* authority = nullptr;
		std::vector<room::MemberId> recipients;
	};
	std::vector<PendingTable> active;
	const auto snapshot = _roomAuthority->SnapshotCopy();
	std::vector<InterruptedPreparation> retained;
	for (const auto& pending : _pendingInterruptedPreparations) {
		if (pending.table >= room::TableCount) continue;
		auto* authority = _roomMatchAuthorities[pending.table].get();
		const auto& table = snapshot.tables[pending.table];
		if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle ||
			table.phase != room::TablePhase::Playing || table.matchGeneration != pending.generation) {
			// The imported checkpoint may already contain the committed teardown,
			// or a later generation may have replaced this one.
			continue;
		}
		if (authority->GetPhase() == session::MatchAuthority::Phase::Started) {
			// Started is a durable gameplay state. Never tear it down as part of
			// preparation recovery.
			continue;
		}
		PendingTable item;
		item.table = pending.table; item.generation = pending.generation; item.authority = authority;
		for (const auto member : {table.p1, table.p2}) if (member &&
			std::find(item.recipients.begin(), item.recipients.end(), member) == item.recipients.end()) item.recipients.push_back(member);
		for (const auto member : table.spectators) if (member &&
			std::find(item.recipients.begin(), item.recipients.end(), member) == item.recipients.end()) item.recipients.push_back(member);
		active.push_back(std::move(item));
		retained.push_back(pending);
	}
	_pendingInterruptedPreparations = std::move(retained);
	if (active.empty()) return true;
	BeginRecoveryCandidate();
	if (!_recoveryCandidateReady) return false;
	std::vector<room::Event> events;
	for (const auto& item : active) {
		// MatchAuthority::End sends the private native teardown through the
		// candidate journal. It cannot touch a socket while the candidate is
		// uncommitted.
		if (!item.authority->End(MatchSender())) return false;
		const auto ended = _roomAuthority->EndMatch(item.table, item.generation, room::MatchResult::Abort);
		if (!ended.accepted) return false;
		_roomBattleLoaded[item.table].clear();
		_roomPunchReady[item.table].clear();
		_roomMatchData[item.table].Clear();
		events.insert(events.end(), ended.events.begin(), ended.events.end());
		// Pair the teardown with an acknowledged room result before the regular
		// snapshot/event burst. actionId=0 is the legacy synthetic recovery form.
		for (const auto member : item.recipients) {
			for (const auto& mapping : roomMembers) {
				if (mapping.second != member) continue;
				SessionProtocol::RoomResultMessage response;
				response.result.accepted = true;
				response.result.snapshot = _roomAuthority->SnapshotFor(member);
				Respond(mapping.first, json(response));
				break;
			}
		}
	}
	BroadcastRoomState(events);
	_cancellationCandidate = true;
	return true;
}

bool SessionServer::RebindMembers(const std::vector<StableRebind>& bindings) {
	if (!_roomAuthority) return false;
	const auto& snapshot = _roomAuthority->SnapshotView();
	if (bindings.empty()) return snapshot.members.empty();
	if (bindings.size() != snapshot.members.size()) return false;
	std::set<room::MemberId> members;
	std::set<session::Connection> handles;
	std::set<std::pair<std::string, std::string>> endpoints;
	const auto findRoomMember = [&](room::MemberId id) -> const room::Member* {
		for (const auto& value : snapshot.members) if (value.id == id) return &value;
		return nullptr;
	};
	for (const auto& binding : bindings) {
		const auto member = std::get<0>(binding);
		const auto local = std::get<1>(binding);
		const auto& cid = std::get<2>(binding);
		const auto incarnation = std::get<3>(binding);
		const auto* roomMember = findRoomMember(member);
		if (!roomMember || !local || !incarnation || roomMember->connection.host != cid.host ||
			roomMember->connection.user != cid.user || !members.insert(member).second ||
			!handles.insert(local).second || !endpoints.emplace(cid.host, cid.user).second) return false;
		const auto priorIncarnation = roomIncarnations.find(member);
		if (priorIncarnation != roomIncarnations.end() && priorIncarnation->second && priorIncarnation->second != incarnation) return false;
	}
	if (members.size() != snapshot.members.size()) return false;
	for (const auto& row : clients) {
		const auto member = roomMembers.find(row.conn);
		if (member == roomMembers.end() || !members.count(member->second)) return false;
	}

	// Capture the current native mapping so a failed authority update can be
	// rolled back without regenerating private capabilities or changing phase.
	std::vector<json> oldPortable;
	for (const auto& authority : _roomMatchAuthorities) oldPortable.push_back(authority ? authority->PortableCheckpoint() : json(nullptr));
	const json oldLegacy = _matchAuthority ? _matchAuthority->PortableCheckpoint() : json(nullptr);
	const auto oldHandleFor = [&](const SessionProtocol::ConnectionID& endpoint) -> session::Connection {
		for (const auto& pair : cidMap) if (pair.second == endpoint) return pair.first;
		for (const auto& member : snapshot.members) if (member.connection.host == endpoint.host && member.connection.user == endpoint.user) return static_cast<session::Connection>(member.id);
		return 0;
	};
	std::vector<session::MatchAuthority::RebindEntry> fullMapping;
	for (const auto& binding : bindings) fullMapping.push_back({std::get<2>(binding), std::get<1>(binding)});
	const auto rollbackAuthorities = [&]() {
		const auto oldResolver = [&](const SessionProtocol::ConnectionID& endpoint) { return oldHandleFor(endpoint); };
		for (std::size_t i = 0; i < _roomMatchAuthorities.size(); ++i) {
			if (_roomMatchAuthorities[i] && i < oldPortable.size()) _roomMatchAuthorities[i]->RestorePortableCheckpoint(oldPortable[i], oldResolver);
		}
		if (_matchAuthority && !oldLegacy.is_null()) _matchAuthority->RestorePortableCheckpoint(oldLegacy, oldResolver);
	};
	for (auto& authoritySlot : _roomMatchAuthorities) {
		auto* authority = authoritySlot.get();
		if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle) continue;
		if (!authority->RebindConnections(fullMapping)) { rollbackAuthorities(); return false; }
	}
	if (_matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle &&
		!_matchAuthority->RebindConnections(fullMapping)) {
		rollbackAuthorities(); return false;
	}

	std::map<room::MemberId, SessionMember> oldRows;
	std::map<room::MemberId, session::Connection> oldConnections;
	for (const auto& row : clients) {
		const auto member = roomMembers.find(row.conn);
		if (member != roomMembers.end()) {
			oldRows[member->second] = row;
			oldConnections[member->second] = row.conn;
		}
	}
	std::map<session::Connection, SessionProtocol::ConnectionID> newCids;
	std::map<session::Connection, room::MemberId> newMembers;
	std::map<session::Connection, std::uint8_t> newSelected;
	std::vector<SessionMember> newClients;
	std::array<std::set<session::Connection>, room::TableCount> newBattleLoaded{};
	std::array<std::set<session::Connection>, room::TableCount> newPunchReady{};
	for (const auto& binding : bindings) {
		const auto member = std::get<0>(binding);
		const auto local = std::get<1>(binding);
		const auto& cid = std::get<2>(binding);
		const auto incarnation = std::get<3>(binding);
		SessionMember row{};
		const auto prior = oldRows.find(member);
		if (prior != oldRows.end()) row = prior->second;
		const auto* roomMember = findRoomMember(member);
		row.conn = local; row.data.connId = cid; row.data.name = roomMember->name;
		row.data.roomMember = member; row.data.authenticatedEndpoint = roomPeerIdentities.count(member) ? roomPeerIdentities.at(member) : cid.user;
		row.data.incarnation = incarnation;
		// The route is the stable record's, whatever an older row held.
		const auto record = roomMemberRecords.find(member);
		row.data.latency = record != roomMemberRecords.end() ? record->second.latency : sf4e::LatencyRoute{};
		newClients.push_back(row); newCids[local] = cid; newMembers[local] = member;
		const auto priorSelected = prior == oldRows.end() ? roomSelectedTables.end() : roomSelectedTables.find(prior->second.conn);
		newSelected[local] = priorSelected != roomSelectedTables.end() ? priorSelected->second :
			(_recoveryPendingSelected.count(member) ? _recoveryPendingSelected.at(member) : 0);
		for (std::size_t table = 0; table < room::TableCount; ++table) {
			const auto oldConnection = oldConnections.find(member);
			if ((oldConnection != oldConnections.end() && _roomBattleLoaded[table].count(oldConnection->second)) || _recoveryPendingBattleLoaded[table].count(member)) newBattleLoaded[table].insert(local);
			if ((oldConnection != oldConnections.end() && _roomPunchReady[table].count(oldConnection->second)) || _recoveryPendingPunchReady[table].count(member)) newPunchReady[table].insert(local);
		}
		roomIncarnations[member] = incarnation;
		_roomAuthority->SetMemberIncarnation(member, incarnation);
	}
	clients = std::move(newClients); cidMap = std::move(newCids); roomMembers = std::move(newMembers);
	roomSelectedTables = std::move(newSelected); _roomBattleLoaded = std::move(newBattleLoaded); _roomPunchReady = std::move(newPunchReady);
	for (const auto& binding : bindings) {
		const auto member = std::get<0>(binding);
		_recoveryPendingSelected.erase(member);
		for (auto& set : _recoveryPendingBattleLoaded) set.erase(member);
		for (auto& set : _recoveryPendingPunchReady) set.erase(member);
	}
	return true;
}

std::size_t SessionServer::ReplayPendingTerminalEvents(session::Connection connection, room::MemberId member) {
	if (!_roomAuthority || !connection || !member) return 0;
	const auto pending = _roomAuthority->PendingTerminalEvents(member);
	for (const auto& replay : pending) {
		SessionProtocol::RoomEventMessage eventMessage;
		eventMessage.event = room::Event{room::Event::Kind::MatchEnded, replay.table, replay.generation, 0, replay.result, true};
		Respond(connection, json(eventMessage));
		// The native side may also still be waiting for its game_end.
		if (!MatchSender()(connection, json{{"type", "game_end"}, {"generation", replay.generation}})) _transportFailed = true;
		spdlog::info("Server: replayed terminal event member={} table={} generation={} result={}",
			member, replay.table, replay.generation, static_cast<int>(replay.result));
	}
	return pending.size();
}
