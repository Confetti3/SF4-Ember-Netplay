#pragma once

// Portable authority and effect journal types shared by SessionServer and the
// helper/room owner.  This header deliberately contains no transport or
// Windows dependency: a follower can validate a proposal before it has a
// local socket or a native Connection handle.

#include <array>
#include <algorithm>
#include <cstddef>
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
#include "../common/JsonDump.hxx"
#include "../common/Sha256.hxx"

namespace sf4e { namespace session {

using EffectDigest = std::string;

// SHA-256 of the canonical JSON dump, the same digest the Rust helper computes.
// Sha256 uses Windows CNG, which uses the CPU's SHA instructions: a large room
// hashes several hundred KB per committed mutation on the game thread. The
// dependency-free Sha256Portable is the fallback and the reference for tests.
namespace recovery_detail {

using sf4e::Sha256Portable;

// Defined in RoomDigest.cxx as coordination::Sha256 (CoordinationBytes.hxx),
// so this header stays free of Windows headers.
std::string Sha256(const std::string& input);

inline bool IsSha256(const std::string& value) {
	if (value.size() != 64) return false;
	for (const auto c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	return true;
}

} // namespace recovery_detail

inline EffectDigest PayloadDigest(const nlohmann::json& payload) {
	return recovery_detail::Sha256(DumpJson(payload));
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
	// A room_snapshot that carries the room chat itself (CarriesRoomChat), which
	// later chat_unchanged snapshots leave out. Kept when the copy is shed, so
	// the journal still knows which snapshot holds the chat. Not identity.
	bool chat = false;

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
	if (effect.chat) value["chat"] = true;
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

// Encoded size of DumpJson(json(effect)) when its replay copy, if any, encodes to
// payloadBytes bytes, without encoding that copy again. Object keys are sorted
// and "public_payload" is never the first, so the copy adds exactly
// `,"public_payload":` plus its own bytes.
inline std::size_t EncodedEnvelopeBytes(EffectEnvelope& effect, std::size_t payloadBytes) {
	if (effect.privatePayload || effect.publicPayload.is_null()) return DumpJson(nlohmann::json(effect)).size();
	auto copy = std::move(effect.publicPayload);
	effect.publicPayload = nullptr;
	const auto bytes = DumpJson(nlohmann::json(effect)).size();
	effect.publicPayload = std::move(copy);
	return bytes + sizeof(",\"public_payload\":") - 1 + payloadBytes;
}

// The live wire form of a committed effect: its already encoded payload object
// with the commit token added, so the payload is not copied or encoded again.
inline std::string CommittedEffectWire(const EffectEnvelope& effect, const std::string& encodedPayload) {
	std::string wire = "{\"_commit\":" + DumpJson(EffectCommitToken(effect));
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
	effect.chat = value.value("chat", false);
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

// A superseded projection without a replay copy is kept this many revisions
// behind the newest revision in the journal being compacted. Its envelope is
// what authenticates a live token parked behind staged commits, and staging
// parks at four commits, so a parked token is at most three revisions behind;
// four leaves one commit of margin.
static constexpr std::uint64_t SupersededProjectionRevisions = 4;

// A room_snapshot payload that carries the room chat itself rather than saying
// chat_unchanged. Never throws on an odd payload.
inline bool CarriesRoomChat(const nlohmann::json& payload) {
	if (!payload.is_object()) return false;
	const auto snapshot = payload.find("snapshot");
	if (snapshot == payload.end() || !snapshot->is_object()) return false;
	const auto unchanged = snapshot->find("chat_unchanged");
	return unchanged == snapshot->end() || !unchanged->is_boolean() || !unchanged->get<bool>();
}

inline bool ChatBearingSnapshot(const EffectEnvelope& effect) {
	return effect.type == "room_snapshot" && effect.chat;
}

// Whether `later`, committed after `effect`, replaces it: room state of its
// kind for its recipient, and for a snapshot with the chat one with the chat
// too, as the later chat_unchanged ones leave it out.
inline bool ReplacesState(const EffectEnvelope& later, const EffectEnvelope& effect) {
	return SupersedableProjection(effect) && later.recipient == effect.recipient &&
		(ChatBearingSnapshot(effect) ? ChatBearingSnapshot(later) : later.type == effect.type);
}

// Room state a later entry replaces.
inline bool ReplacedState(const std::vector<EffectEnvelope>& history, std::size_t index) {
	return std::any_of(history.begin() + static_cast<std::ptrdiff_t>(index) + 1, history.end(),
		[&](const EffectEnvelope& later) { return ReplacesState(later, history[index]); });
}

// Whether a member may go without an effect it has not had once `journal` no
// longer holds it whole (its entry gone, or its replay copy): room state the
// journal holds a newer one of, and best-effort verification. Anything else
// would be lost without a word, so its room fails by name instead, at the
// effect gate (IrohRoom::HeadGate) or at activation.
inline bool Replaceable(const EffectEnvelope& effect, const std::vector<EffectEnvelope>& journal) {
	if (effect.type == "battle_hash" || effect.type == "battle_snapshot") return true;
	return std::any_of(journal.begin(), journal.end(), [&](const EffectEnvelope& later) {
		const bool after = later.revision > effect.revision || (later.revision == effect.revision && later.sequence > effect.sequence);
		return after && ReplacesState(later, effect);
	});
}

// What the journal promises a member whose checkpoints apply late. Room state
// is replaceable: under the budget the journal keeps, for each recipient, only
// its newest snapshot, its newest data_update and its newest snapshot with the
// chat. Every other effect (an action's reply, a room event, a match grant or
// end) keeps at least its envelope, which is all the effect gate needs to
// authenticate a member's parked live copy, until GuaranteedEffectEnvelopes
// newer entries have been committed: replay copies are shed before any such
// envelope is erased, and a journal of nothing but envelopes (under 450 bytes
// each, under 115 KB for 256) is inside the byte budget, so the entry bound
// is the one that binds. The bound is the room's, not each member's, since the
// members share one journal, and it counts three state entries for each of
// 16 members (one who has just left keeps its last three until they are the
// oldest entries, which takes a few from the bound while the room turns over). A 16-member room at its busiest (issue 231 and
// the public room soak) makes about 6 commits a second, each with one action's
// reply, and about 2 lifecycle effects a second across its four tables: under
// 1.4 such effects a commit. BusiestEffectsPerCommit allows 2, so the 208 envelopes
// cover at least 104 commits, about 17 s at that rate. A member that is
// further behind fails (ClientEffectQueue), and an effect that is gone all the
// same fails its room by name at the effect gate rather than being lost.
constexpr std::size_t GuaranteedEffectEnvelopes = MaxEffectJournalEntries - 3 * room::MaximumMembers;
constexpr std::size_t BusiestEffectsPerCommit = 2;

// Supersession, before any budget pressure. A projection followed by a later
// one for the same (recipient, type) keeps its envelope but drops its replay
// copy: a successor replays only the newest projection, and the envelope alone
// still authenticates a live token for it (EffectEnvelope::operator== ignores
// the payload). The newest chat-bearing snapshot of a recipient is not
// superseded while every later one says chat_unchanged: it keeps its copy, so
// a replay still carries the chat. A superseded entry without a copy is erased
// once it is more than SupersededProjectionRevisions behind the newest revision among the entries
// compacted, never the owner's authority revision, so the leader (history plus
// candidate) and a follower (journal plus effects) compute the same journal.
inline void DropSupersededProjectionPayloads(std::vector<EffectEnvelope>& history, std::vector<std::size_t>& encoded) {
	std::uint64_t newest = 0;
	for (const auto& effect : history) newest = (std::max)(newest, effect.revision);
	std::set<room::MemberId> laterSnapshot, laterData, laterChat;
	std::vector<char> superseded(history.size(), 0);
	for (std::size_t index = history.size(); index-- > 0;) {
		auto& effect = history[index];
		if (!SupersedableProjection(effect)) continue;
		const bool snapshot = effect.type == "room_snapshot";
		const bool laterOfKind = !(snapshot ? laterSnapshot : laterData).insert(effect.recipient).second;
		const bool newestChat = snapshot && ChatBearingSnapshot(effect) && laterChat.insert(effect.recipient).second;
		superseded[index] = laterOfKind && !newestChat;
		if (!superseded[index] || effect.publicPayload.is_null()) continue;
		effect.publicPayload = nullptr;
		encoded[index] = DumpJson(nlohmann::json(effect)).size();
	}
	std::size_t kept = 0;
	for (std::size_t index = 0; index < history.size(); ++index) {
		if (superseded[index] && history[index].publicPayload.is_null() &&
			newest - history[index].revision > SupersededProjectionRevisions) continue;
		if (kept != index) { history[kept] = std::move(history[index]); encoded[kept] = encoded[index]; }
		++kept;
	}
	history.erase(history.begin() + static_cast<std::ptrdiff_t>(kept), history.end());
	encoded.erase(encoded.begin() + static_cast<std::ptrdiff_t>(kept), encoded.end());
}

inline std::size_t EffectJournalBytes(const std::vector<EffectEnvelope>& history) {
	std::size_t bytes = 2; // JSON array brackets.
	for (std::size_t i = 0; i < history.size(); ++i) {
		if (i) ++bytes; // comma
		bytes += DumpJson(nlohmann::json(history[i])).size();
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
		const auto size = DumpJson(nlohmann::json(effect)).size();
		encoded.push_back(size); bytes += size;
	}
	for (std::size_t i = 0; i < history.size() && bytes > targetBytes; ++i) {
		if (!SupersedableProjection(history[i]) || history[i].publicPayload.is_null()) continue;
		history[i].publicPayload = nullptr;
		const auto size = DumpJson(nlohmann::json(history[i])).size();
		bytes -= encoded[i] - size;
		encoded[i] = size;
	}
	return bytes;
}

// encoded[i] is DumpJson(json(history[i])).size() and is kept in step with
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
	DropSupersededProjectionPayloads(history, encoded);
	std::size_t bytes = history.empty() ? 2 : history.size() + 1;
	for (const auto size : encoded) bytes += size;
	const auto erase = [&](std::size_t index) {
		bytes -= encoded[index];
		if (history.size() > 1) --bytes; // one array comma disappears
		history.erase(history.begin() + index);
		encoded.erase(encoded.begin() + index);
	};
	// The budget takes replaced room state first, at any age; then replay
	// copies, room state's before the rest and the oldest first; only then the
	// oldest entry (GuaranteedEffectEnvelopes).
	while (history.size() > MaxEffectJournalEntries || bytes > MaxEffectJournalBytes) {
		std::size_t replaced = 0;
		while (replaced < history.size() && !ReplacedState(history, replaced)) ++replaced;
		if (replaced < history.size()) {
			erase(replaced);
			continue;
		}
		// Replay copies are optional: an envelope's stable digest is enough to
		// authenticate a separately delivered effect. Shed every copy before
		// discarding an ordered lifecycle identity. Otherwise an active private
		// game_prepare at the front can be erased merely because a later
		// recipient's large, unique snapshot or an action's reply, which carries
		// the whole room, crosses the byte budget.
		if (bytes > MaxEffectJournalBytes) {
			const auto copy = [](const EffectEnvelope& effect) { return !effect.publicPayload.is_null(); };
			auto payload = std::find_if(history.begin(), history.end(), [&](const EffectEnvelope& effect) {
				return SupersedableProjection(effect) && copy(effect);
			});
			if (payload == history.end()) payload = std::find_if(history.begin(), history.end(), copy);
			if (payload != history.end()) {
				const auto index = static_cast<std::size_t>(std::distance(history.begin(), payload));
				payload->publicPayload = nullptr;
				const auto size = DumpJson(nlohmann::json(*payload)).size();
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
	for (const auto& effect : history) encoded.push_back(DumpJson(nlohmann::json(effect)).size());
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
	// Local only, never serialized: DumpJson(json(*this)) as produced when the
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

// Exactly DumpJson(json(proposal)) (keys in the same sorted order), given the
// effects already encoded, without building a JSON copy of the checkpoint.
inline std::string EncodeSessionProposal(const SessionProposal& proposal, const std::string& encodedEffects) {
	return "{\"base_revision\":" + std::to_string(proposal.baseRevision) +
		",\"checkpoint\":" + DumpJson(proposal.checkpoint) +
		",\"effects\":" + encodedEffects +
		",\"effects_digest\":" + DumpJson(nlohmann::json(proposal.effectsDigest)) +
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
