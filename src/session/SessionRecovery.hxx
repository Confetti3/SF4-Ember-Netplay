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

namespace sf4e { namespace session {

using EffectDigest = std::string;

// SHA-256 of the canonical JSON dump, the same digest the Rust helper computes.
// Sha256 uses Windows CNG, which uses the CPU's SHA instructions: a large room
// hashes several hundred KB per committed mutation on the game thread. The
// dependency-free Sha256Portable is the fallback and the reference for tests.
namespace recovery_detail {

inline std::uint32_t ShaRotate(std::uint32_t value, std::uint32_t amount) {
	return (value >> amount) | (value << (32U - amount));
}

inline std::string Sha256Portable(const std::string& input) {
	static constexpr std::uint32_t k[64] = {
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
		0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
		0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
		0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
		0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
		0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
		0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
		0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
		0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
	};
	std::vector<std::uint8_t> bytes(input.begin(), input.end());
	const auto bitLength = static_cast<std::uint64_t>(bytes.size()) * 8U;
	bytes.push_back(0x80U);
	while ((bytes.size() % 64U) != 56U) bytes.push_back(0U);
	for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(bitLength >> shift));

	std::uint32_t h[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
	for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
		std::uint32_t w[64] = {};
		for (std::size_t i = 0; i < 16; ++i)
			w[i] = (static_cast<std::uint32_t>(bytes[offset + i * 4]) << 24) |
				(static_cast<std::uint32_t>(bytes[offset + i * 4 + 1]) << 16) |
				(static_cast<std::uint32_t>(bytes[offset + i * 4 + 2]) << 8) |
				static_cast<std::uint32_t>(bytes[offset + i * 4 + 3]);
		for (std::size_t i = 16; i < 64; ++i) {
			const auto s0 = ShaRotate(w[i - 15], 7) ^ ShaRotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const auto s1 = ShaRotate(w[i - 2], 17) ^ ShaRotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (std::size_t i = 0; i < 64; ++i) {
			const auto S1 = ShaRotate(e, 6) ^ ShaRotate(e, 11) ^ ShaRotate(e, 25);
			const auto ch = (e & f) ^ ((~e) & g);
			const auto temp1 = hh + S1 + ch + k[i] + w[i];
			const auto S0 = ShaRotate(a, 2) ^ ShaRotate(a, 13) ^ ShaRotate(a, 22);
			const auto maj = (a & b) ^ (a & c) ^ (b & c);
			const auto temp2 = S0 + maj;
			hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d;
		h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string result; result.reserve(64);
	for (const auto value : h) for (int shift = 28; shift >= 0; shift -= 4) result.push_back(hex[(value >> shift) & 0xfU]);
	return result;
}

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

} }
