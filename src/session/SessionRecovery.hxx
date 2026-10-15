#pragma once

// Portable authority and effect journal types shared by SessionServer and the
// helper/room owner.  This header deliberately contains no transport or
// Windows dependency: a follower can validate a proposal before it has a
// local socket or a native Connection handle.

#include <array>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "RoomModel.hxx"
#include "../common/Sha256.hxx"

namespace sf4e { namespace session {

using EffectDigest = std::string;

// SHA-256 of the canonical JSON dump, the same digest the Rust helper computes.
// Sha256 uses Windows CNG, which uses the CPU's SHA instructions: a large room
// hashes several hundred KB per committed mutation on the game thread. The
// dependency-free Sha256Portable is the fallback and the reference for tests.
namespace recovery_detail {

using sf4e::Sha256Portable;

// Defined in RoomDigest.cxx, so this header stays free of Windows headers.
std::string Sha256(const std::string& input);

inline bool IsSha256(const std::string& value) {
	if (value.size() != 64) return false;
	for (const auto c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	return true;
}

} // namespace recovery_detail

inline EffectDigest PayloadDigest(const nlohmann::json& payload) {
	return recovery_detail::Sha256(payload.dump());
}

struct AuthorityStamp {
	std::uint64_t term = 0;
	std::uint64_t revision = 0;
	bool writable = false;
};

// Numeric transport handles are intentionally absent.  A recipient is a
// stable room member plus its authenticated endpoint.  The endpoint is used
// to validate a rebind and the local numeric handle is resolved only at send
// time.
struct EffectEnvelope {
	std::uint64_t sequence = 0;
	// roomEpoch is the stable room instance identifier.  It is intentionally
	// separate from the helper's technical term/revision and from a transient
	// transport connection handle.
	std::uint64_t roomEpoch = 0;
	std::uint64_t term = 0;
	std::uint64_t revision = 0;
	std::uint64_t generation = 0;
	room::MemberId recipient = 0;
	room::ConnectionRef endpoint;
	std::string type;
	EffectDigest payloadDigest;
	bool privatePayload = false;
	// Public, idempotent effects may be replayed by a successor after a
	// committed handoff. Private game_prepare grants deliberately leave this
	// null; their pair capabilities are represented only by payloadDigest.
	nlohmann::json publicPayload = nullptr;

	bool operator==(const EffectEnvelope& rhs) const {
		return sequence == rhs.sequence && roomEpoch == rhs.roomEpoch && term == rhs.term && revision == rhs.revision &&
			generation == rhs.generation && recipient == rhs.recipient && endpoint == rhs.endpoint &&
			type == rhs.type && payloadDigest == rhs.payloadDigest && privatePayload == rhs.privatePayload;
	}
};

inline void to_json(nlohmann::json& value, const EffectEnvelope& effect) {
	value = { {"sequence", effect.sequence}, {"room_epoch", effect.roomEpoch}, {"term", effect.term}, {"revision", effect.revision},
		{"generation", effect.generation}, {"recipient", effect.recipient}, {"endpoint", effect.endpoint},
		{"type", effect.type}, {"payload_digest", effect.payloadDigest}, {"private_payload", effect.privatePayload} };
	if (!effect.privatePayload && !effect.publicPayload.is_null()) value["public_payload"] = effect.publicPayload;
}

// Live delivery already carries the public protocol payload beside `_commit`.
// The commit token therefore carries only immutable identity/digest fields.
// Checkpoint journals keep using EffectEnvelope::to_json so a successor still
// retains the optional public replay copy.
inline nlohmann::json EffectCommitToken(const EffectEnvelope& effect) {
	return { {"sequence", effect.sequence}, {"room_epoch", effect.roomEpoch}, {"term", effect.term},
		{"revision", effect.revision}, {"generation", effect.generation}, {"recipient", effect.recipient},
		{"endpoint", effect.endpoint}, {"type", effect.type}, {"payload_digest", effect.payloadDigest},
		{"private_payload", effect.privatePayload} };
}

// Encoded size of json(effect).dump() when its replay copy, if any, encodes to
// payloadBytes bytes, without encoding that copy again. Object keys are sorted
// and "public_payload" is never the first, so the copy adds exactly
// `,"public_payload":` plus its own bytes.
inline std::size_t EncodedEnvelopeBytes(EffectEnvelope& effect, std::size_t payloadBytes) {
	if (effect.privatePayload || effect.publicPayload.is_null()) return nlohmann::json(effect).dump().size();
	auto copy = std::move(effect.publicPayload);
	effect.publicPayload = nullptr;
	const auto bytes = nlohmann::json(effect).dump().size();
	effect.publicPayload = std::move(copy);
	return bytes + sizeof(",\"public_payload\":") - 1 + payloadBytes;
}

// The live wire form of a committed effect: its already encoded payload object
// with the commit token added, so the payload is not copied or encoded again.
inline std::string CommittedEffectWire(const EffectEnvelope& effect, const std::string& encodedPayload) {
	std::string wire = "{\"_commit\":" + EffectCommitToken(effect).dump();
	if (encodedPayload.size() > 2) { wire += ','; wire.append(encodedPayload, 1, std::string::npos); }
	else wire += '}';
	return wire;
}

inline void from_json(const nlohmann::json& value, EffectEnvelope& effect) {
	effect.sequence = value.at("sequence").get<std::uint64_t>();
	effect.roomEpoch = value.value("room_epoch", std::uint64_t(0));
	effect.term = value.at("term").get<std::uint64_t>();
	effect.revision = value.at("revision").get<std::uint64_t>();
	effect.generation = value.at("generation").get<std::uint64_t>();
	effect.recipient = value.at("recipient").get<room::MemberId>();
	value.at("endpoint").get_to(effect.endpoint);
	effect.type = value.at("type").get<std::string>();
	effect.payloadDigest = value.at("payload_digest").get<std::string>();
	effect.privatePayload = value.at("private_payload").get<bool>();
	effect.publicPayload = value.value("public_payload", nlohmann::json(nullptr));
	if (effect.privatePayload && !effect.publicPayload.is_null()) throw std::invalid_argument("private effect payload replicated");
	if (!effect.sequence || !effect.term || !effect.recipient || effect.endpoint.host.empty() || effect.endpoint.user.empty() ||
		effect.type.empty() || !recovery_detail::IsSha256(effect.payloadDigest)) throw std::invalid_argument("invalid effect envelope");
}

// Deterministic bounded journal selection shared by the server checkpoint and
// the root recovery bridge. Only supersedable projections may be removed ahead
// of lifecycle/final-result records; digest-only projections remain eligible
// for replay validation without carrying another large public payload.
static constexpr std::size_t MaxEffectJournalEntries = 256;
static constexpr std::size_t MaxEffectJournalBytes = 256 * 1024;

inline bool SupersedableProjection(const EffectEnvelope& effect) {
	return effect.type == "room_snapshot" || effect.type == "data_update";
}

inline std::size_t EffectJournalBytes(const std::vector<EffectEnvelope>& history) {
	std::size_t bytes = 2; // JSON array brackets.
	for (std::size_t i = 0; i < history.size(); ++i) {
		if (i) ++bytes; // comma
		bytes += nlohmann::json(history[i]).dump().size();
	}
	return bytes;
}

// Pending local delivery keeps the original payload separately. Replication
// may shed any optional projection copy across the candidate, including an
// older recipient projection which otherwise strands a later private grant.
inline std::size_t ShedOptionalEffectPayloads(std::vector<EffectEnvelope>& history, std::size_t targetBytes) {
	std::vector<std::size_t> encoded;
	encoded.reserve(history.size());
	std::size_t bytes = history.empty() ? 2 : history.size() + 1;
	for (const auto& effect : history) {
		const auto size = nlohmann::json(effect).dump().size();
		encoded.push_back(size); bytes += size;
	}
	for (std::size_t i = 0; i < history.size() && bytes > targetBytes; ++i) {
		if (!SupersedableProjection(history[i]) || history[i].publicPayload.is_null()) continue;
		history[i].publicPayload = nullptr;
		const auto size = nlohmann::json(history[i]).dump().size();
		bytes -= encoded[i] - size;
		encoded[i] = size;
	}
	return bytes;
}

// encoded[i] is json(history[i]).dump().size() and is kept in step with
// history, so an owner that tracks it avoids re-encoding its whole journal on
// every commit. Both vectors are left compacted.
inline void CompactEffectJournal(std::vector<EffectEnvelope>& history, std::vector<std::size_t>& encoded) {
	// Verification history is best effort, not a room lifecycle receipt. Retain
	// only a recent window so a long fight cannot fill every later checkpoint
	// with obsolete hashes or evict the admission/result/teardown identities.
	const auto diagnostic = [](const EffectEnvelope& effect) {
		return effect.type == "battle_hash" || effect.type == "battle_snapshot";
	};
	auto diagnostics = std::count_if(history.begin(), history.end(), diagnostic);
	for (std::size_t item = 0; item < history.size() && diagnostics > 32;) {
		if (diagnostic(history[item])) {
			history.erase(history.begin() + item); encoded.erase(encoded.begin() + item); --diagnostics;
		} else ++item;
	}
	std::size_t bytes = history.empty() ? 2 : history.size() + 1;
	for (const auto size : encoded) bytes += size;
	const auto erase = [&](std::size_t index) {
		bytes -= encoded[index];
		if (history.size() > 1) --bytes; // one array comma disappears
		history.erase(history.begin() + index);
		encoded.erase(encoded.begin() + index);
	};
	while (history.size() > MaxEffectJournalEntries || bytes > MaxEffectJournalBytes) {
		auto removable = history.end();
		for (auto current = history.begin(); current != history.end(); ++current) {
			if (!SupersedableProjection(*current)) continue;
			const auto later = std::find_if(std::next(current), history.end(), [&](const EffectEnvelope& candidate) {
				return candidate.recipient == current->recipient && candidate.type == current->type;
			});
			if (later != history.end()) { removable = current; break; }
		}
		if (removable != history.end()) {
			erase(static_cast<std::size_t>(std::distance(history.begin(), removable)));
			continue;
		}
		// Public projections are optional replay copies: their stable digest is
		// enough to authenticate a separately delivered effect. Search the whole
		// journal before discarding an ordered lifecycle identity. Otherwise an
		// active private game_prepare at the front can be erased merely because a
		// later recipient's large, unique snapshot crosses the byte budget.
		if (bytes > MaxEffectJournalBytes) {
			auto payload = std::find_if(history.begin(), history.end(), [&](const EffectEnvelope& effect) {
				return SupersedableProjection(effect) && !effect.publicPayload.is_null();
			});
			if (payload != history.end()) {
				const auto index = static_cast<std::size_t>(std::distance(history.begin(), payload));
				payload->publicPayload = nullptr;
				const auto size = nlohmann::json(*payload).dump().size();
				bytes -= encoded[index] - size;
				encoded[index] = size;
				continue;
			}
		}
		if (history.empty()) break;
		erase(0);
	}
}

inline void CompactEffectJournal(std::vector<EffectEnvelope>& history) {
	std::vector<std::size_t> encoded;
	encoded.reserve(history.size());
	for (const auto& effect : history) encoded.push_back(nlohmann::json(effect).dump().size());
	CompactEffectJournal(history, encoded);
}

inline bool ContainsCapabilityField(const nlohmann::json& value, unsigned depth = 0) {
	if (depth > 16) return true;
	if (value.is_object()) {
		for (auto field = value.begin(); field != value.end(); ++field) {
			if (field.key() == "capability" || ContainsCapabilityField(field.value(), depth + 1)) return true;
		}
	} else if (value.is_array()) {
		for (const auto& item : value) if (ContainsCapabilityField(item, depth + 1)) return true;
	}
	return false;
}

struct SessionProposal {
	std::uint64_t request = 0;
	std::uint64_t term = 0;
	std::uint64_t baseRevision = 0;
	nlohmann::json checkpoint;
	std::vector<EffectEnvelope> effects;
	EffectDigest effectsDigest;
	// Local only, never serialized: json(*this).dump() as produced when the
	// owner bounded the proposal's size, so sending it does not encode again.
	std::string encoded;

	bool Valid() const {
		if (!request || !term || !recovery_detail::IsSha256(effectsDigest) || !checkpoint.is_object()) return false;
		std::set<std::uint64_t> sequences;
		for (const auto& effect : effects) {
			if (effect.term != term || effect.revision != baseRevision + 1 || !sequences.insert(effect.sequence).second) return false;
		}
		return true;
	}
};

inline EffectDigest EffectsDigest(const std::vector<EffectEnvelope>& effects) {
	nlohmann::json encoded = nlohmann::json::array();
	for (const auto& effect : effects) encoded.push_back(effect);
	return PayloadDigest(encoded);
}

inline void to_json(nlohmann::json& value, const SessionProposal& proposal) {
	value = { {"version", 1}, {"request", proposal.request}, {"term", proposal.term},
		{"base_revision", proposal.baseRevision}, {"checkpoint", proposal.checkpoint},
		{"effects", proposal.effects}, {"effects_digest", proposal.effectsDigest} };
}

// Exactly json(proposal).dump() (keys in the same sorted order), given the
// effects already encoded, without building a JSON copy of the checkpoint.
inline std::string EncodeSessionProposal(const SessionProposal& proposal, const std::string& encodedEffects) {
	return "{\"base_revision\":" + std::to_string(proposal.baseRevision) +
		",\"checkpoint\":" + proposal.checkpoint.dump() +
		",\"effects\":" + encodedEffects +
		",\"effects_digest\":" + nlohmann::json(proposal.effectsDigest).dump() +
		",\"request\":" + std::to_string(proposal.request) +
		",\"term\":" + std::to_string(proposal.term) + ",\"version\":1}";
}

// Consumes the parsed document so the checkpoint, by far its largest member,
// is moved rather than deep copied. Validates exactly as from_json does.
inline SessionProposal DecodeSessionProposal(nlohmann::json&& value) {
	if (value.value("version", 0) != 1) throw std::invalid_argument("unsupported session proposal");
	SessionProposal proposal;
	proposal.request = value.at("request").get<std::uint64_t>();
	proposal.term = value.at("term").get<std::uint64_t>();
	proposal.baseRevision = value.at("base_revision").get<std::uint64_t>();
	proposal.checkpoint = std::move(value.at("checkpoint"));
	value.at("effects").get_to(proposal.effects);
	proposal.effectsDigest = value.at("effects_digest").get<std::string>();
	if (!proposal.Valid() || EffectsDigest(proposal.effects) != proposal.effectsDigest) throw std::invalid_argument("invalid session proposal");
	return proposal;
}

inline void from_json(const nlohmann::json& value, SessionProposal& proposal) {
	proposal = DecodeSessionProposal(nlohmann::json(value));
}

// The gate owns only immutable proposal metadata and local payloads.  State
// mutation is still performed by SessionServer's private candidate; this
// object makes stale terms, duplicate commits and leadership loss explicit.
class SessionRecoveryGate {
public:
	void SetAuthority(std::uint64_t term, std::uint64_t revision, bool writable) {
		if (!term || term < stamp_.term || (term == stamp_.term && revision < stamp_.revision)) return;
		// A same-term health pause does not revoke the proposal which this owner
		// already submitted. The helper may durably commit it while its writable
		// watch is temporarily false; retaining the exact base metadata lets the
		// recovery bridge flush its local private effects after rebound. A term or
		// revision transition still makes an unmatched candidate stale.
		if (term != stamp_.term || revision != stamp_.revision) {
			pending_.reset();
		}
		if (term != stamp_.term) precommittedStarted_.clear();
		stamp_ = {term, revision, writable};
	}

	AuthorityStamp Authority() const { return stamp_; }
	bool Enabled() const { return stamp_.term != 0; }
	bool Writable() const { return stamp_.writable; }
	bool Pending() const { return static_cast<bool>(pending_); }
	std::shared_ptr<const SessionProposal> PendingProposal() const { return pending_; }

	bool Prepare(SessionProposal proposal) {
		if (!stamp_.writable || proposal.term != stamp_.term || proposal.baseRevision != stamp_.revision || !proposal.Valid() || pending_) return false;
		pending_ = std::make_shared<SessionProposal>(std::move(proposal));
		return true;
	}

	bool Commit(std::uint64_t request, std::uint64_t term, std::uint64_t revision, const EffectDigest& digest) {
		if (!pending_ || !stamp_.writable || term != stamp_.term || request != pending_->request || digest != pending_->effectsDigest ||
			revision <= stamp_.revision) return false;
		stamp_.revision = revision;
		for (const auto& effect : pending_->effects) {
			committed_.insert(effect.sequence);
			// This set is only the local replay/dedup index.  The complete
			// bounded journal lives in SessionServer and is checkpointed there;
			// retaining an unbounded sequence set would make a long-lived
			// follower grow without limit even when the public journal is
			// compacted.
			while (committed_.size() > 256) committed_.erase(committed_.begin());
		}
		pending_.reset();
		return true;
	}

	void Discard() { pending_.reset(); }
	const std::set<std::uint64_t>& CommittedEffects() const { return committed_; }

	void MarkPrecommittedStarted(std::uint64_t generation) { if (generation) precommittedStarted_.insert(generation); }
	bool WasPrecommittedStarted(std::uint64_t generation) const { return precommittedStarted_.count(generation) != 0; }
	void MarkStarted(std::uint64_t generation) { precommittedStarted_.erase(generation); started_.insert(generation); }
	bool IsStarted(std::uint64_t generation) const { return started_.count(generation) != 0; }

	// A loss of leadership cancels only preparations which have not reached the
	// committed Started state.  Existing Started generations remain usable and
	// game_start is therefore idempotent at the effect boundary.
	void LeadershipLost() { pending_.reset(); precommittedStarted_.clear(); stamp_.writable = false; }

private:
	AuthorityStamp stamp_;
	std::shared_ptr<SessionProposal> pending_;
	std::set<std::uint64_t> committed_;
	std::set<std::uint64_t> precommittedStarted_;
	std::set<std::uint64_t> started_;
};

// A returning member's helper can replay a commit from before its own
// departure, whose active roster still lists this endpoint under a previous
// process incarnation. That member has left, and the commit removing it
// follows; such a commit can never be rebound to the current session. Frozen
// rows are history a started match retains for a departed spectator, not
// membership, so an old incarnation there says nothing about the commit.
// `currentIncarnation` is 0 until the helper has reported it, when nothing
// can be decided.
inline bool CheckpointPredatesSession(const nlohmann::json& checkpoint, const std::string& localIdentity,
	std::uint64_t currentIncarnation) {
	if (!currentIncarnation || localIdentity.empty() || !checkpoint.is_object() ||
		!checkpoint.contains("members") || !checkpoint.at("members").is_array()) return false;
	for (const auto& row : checkpoint.at("members")) {
		if (!row.is_object() || !row.contains("data") || row.value("frozen", false)) continue;
		const auto& data = row.at("data");
		if (data.value("authenticatedEndpoint", std::string()) != localIdentity) continue;
		const auto incarnation = row.value("incarnation", data.value("incarnation", std::uint64_t(0)));
		if (incarnation && incarnation != currentIncarnation) return true;
	}
	return false;
}

// Whether a commit's active roster has no row for this endpoint at all: it
// was committed before this session was admitted. Frozen rows are history and
// do not count as membership.
inline bool CheckpointOmitsSession(const nlohmann::json& checkpoint, const std::string& localIdentity) {
	if (localIdentity.empty() || !checkpoint.is_object() ||
		!checkpoint.contains("members") || !checkpoint.at("members").is_array()) return false;
	for (const auto& row : checkpoint.at("members")) {
		if (!row.is_object() || !row.contains("data") || row.value("frozen", false)) continue;
		if (row.at("data").value("authenticatedEndpoint", std::string()) == localIdentity) return false;
	}
	return true;
}

// This endpoint's row in a commit's active roster, which activation binds
// local terminal replays to. Frozen rows are history here too: a member that
// left a match and rejoined the room may have an old frozen row beside its
// new active one. False when a row is malformed or the endpoint is active
// twice; true with `local.member` 0 when it is not active at all.
struct RosterRow {
	room::MemberId member = 0;
	room::ConnectionRef endpoint;
	std::uint64_t incarnation = 0;
};
inline bool ActiveRosterRow(const nlohmann::json& members, const std::string& localIdentity, RosterRow& local) {
	local = RosterRow();
	if (!members.is_array()) return false;
	for (const auto& row : members) {
		if (!row.is_object() || !row.contains("member") || !row.contains("endpoint") || !row.contains("data")) return false;
		const auto& data = row.at("data");
		RosterRow value{row.at("member").get<room::MemberId>(), row.at("endpoint").get<room::ConnectionRef>(),
			row.value("incarnation", data.value("incarnation", std::uint64_t(0)))};
		if (!value.member || value.endpoint.host.empty() || value.endpoint.user.empty() || !value.incarnation) return false;
		if (row.value("frozen", false) || data.value("authenticatedEndpoint", std::string()) != localIdentity) continue;
		if (local.member) return false;
		local = std::move(value);
	}
	return true;
}

} }
