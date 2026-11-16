#pragma once
#include "ReplaySlots.hxx"
#include "ReplayFileSafety.hxx"
#include "Sha256.hxx"
#include "LocalTime.hxx"
#include "SkillBadge.hxx"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <utility>

namespace sf4e { namespace replayfiles {
// The tournament set a game whose result was confirmed belongs to: its bridge,
// the set's match ID and the room binding's revision; empty for any other game.
struct SetTag {
 std::string bridgeId, matchId;
 std::uint64_t revision = 0;
 bool Tagged() const { return !bridgeId.empty() && !matchId.empty(); }
};
// How this PC took part in a replay's match, as the note bound to its body
// says. Unknown is every replay without one: none written yet, unreadable, or
// a game put back and saved again. Only a Played game is ever shared.
enum class Provenance { Unknown, Played, Spectated };
// classes: each side's public room class as the HUD showed it when the match
// started (none outside a public room, with class emblems hidden, or for a
// mark that is not a class). Kept in a note of its own beside the names note
// (BindClasses), which builds before it never read.
struct ReplayNames { std::string players[2]; Provenance provenance = Provenance::Unknown; SetTag set; SkillBadge classes[2]; };
struct RecordedSlot { replayslots::Bytes body, checksum; };

// Native metadata has no verified Ember session/player identity. At the
// recording boundary require exactly one newly saved, checksum-complete body
// in the active account, with these fighters and a native save time inside
// this match's lifetime. This one-time attribution remains heuristic. Once
// bound, the side record requires byte-for-byte body identity, never a time
// join against the mixed-account/external archive.
inline const replayslots::Bytes* RecordedBody(const std::vector<RecordedSlot>& before, const std::vector<RecordedSlot>& after,
 std::uint64_t started, std::uint64_t ended, const int fighters[2]) {
 if (started > ended || fighters[0] < 0 || fighters[1] < 0 || before.size() != after.size()) return nullptr;
 const replayslots::Bytes* found = nullptr;
 for (std::size_t i = 0; i < after.size(); ++i) {
  const auto& slot = after[i];
  replayslots::ReplayHeaderInfo header;
  if (slot.body == before[i].body || slot.body.size() > replayslots::kLargestReplay ||
   !replayslots::ReadReplayHeader(slot.body, header) || slot.checksum != replayslots::Sidecar(slot.body) ||
   header.time < started || header.time > ended || header.fighters[0] != fighters[0] || header.fighters[1] != fighters[1]) continue;
  if (found) return nullptr; // ambiguous saves never borrow names
  found = &slot.body;
 }
 return found;
}
// EMBRNAM2: magic, body size, the two name sizes, spectated (byte 20; 0 is
// played), the match and bridge IDs' sizes and the revision (low, high), then
// body, names, match ID and bridge ID. EMBRNAM1 is the same without the tag (a
// 21-byte head). A note always says played or spectated, never Unknown.
constexpr std::size_t kNamesHead = 37, kMostId = 64, kMostNamesBytes = replayslots::kLargestReplay + kNamesHead + 2048 + 2 * kMostId;
inline replayslots::Bytes BindNames(const replayslots::Bytes& body, const ReplayNames& names) {
 if (names.provenance == Provenance::Unknown || body.size() > replayslots::kLargestReplay || names.players[0].size() > 1024 || names.players[1].size() > 1024 ||
  names.set.matchId.size() > kMostId || names.set.bridgeId.size() > kMostId) return {};
 replayslots::Bytes out{'E','M','B','R','N','A','M','2'};
 out.resize(kNamesHead);
 replayslots::WriteU32(out.data()+8, static_cast<std::uint32_t>(body.size()));
 for(int side=0;side<2;++side) replayslots::WriteU32(out.data()+12+side*4, static_cast<std::uint32_t>(names.players[side].size()));
 out[20] = names.provenance == Provenance::Spectated ? 1 : 0;
 replayslots::WriteU32(out.data()+21, static_cast<std::uint32_t>(names.set.matchId.size()));
 replayslots::WriteU32(out.data()+25, static_cast<std::uint32_t>(names.set.bridgeId.size()));
 replayslots::WriteU32(out.data()+29, static_cast<std::uint32_t>(names.set.revision));
 replayslots::WriteU32(out.data()+33, static_cast<std::uint32_t>(names.set.revision >> 32));
 out.insert(out.end(),body.begin(),body.end());
 for(const auto* text:{&names.players[0],&names.players[1],&names.set.matchId,&names.set.bridgeId}) out.insert(out.end(),text->begin(),text->end());
 return out;
}
inline bool NamesForBody(const replayslots::Bytes& note, const replayslots::Bytes& body, ReplayNames& out) {
 if(note.size()<21 || std::memcmp(note.data(),"EMBRNAM",7) || (note[7]!='1' && note[7]!='2') ||
  replayslots::ReadU32(note.data()+8)!=body.size() || note[20]>1) return false;
 const bool tagged=note[7]=='2';
 const std::size_t head=tagged?kNamesHead:21;
 if(note.size()<head) return false;
 const auto p1=replayslots::ReadU32(note.data()+12),p2=replayslots::ReadU32(note.data()+16);
 const auto match=tagged?replayslots::ReadU32(note.data()+21):0,bridge=tagged?replayslots::ReadU32(note.data()+25):0;
 if(p1>1024 || p2>1024 || match>kMostId || bridge>kMostId || body.size()>replayslots::kLargestReplay ||
  note.size()!=head+body.size()+p1+p2+match+bridge || std::memcmp(note.data()+head,body.data(),body.size())) return false;
 ReplayNames names;
 const auto* text=reinterpret_cast<const char*>(note.data()+head+body.size());
 names.players[0].assign(text,p1);names.players[1].assign(text+p1,p2);names.provenance=note[20]?Provenance::Spectated:Provenance::Played;
 if(tagged){
  names.set.matchId.assign(text+p1+p2,match);names.set.bridgeId.assign(text+p1+p2+match,bridge);
  names.set.revision=replayslots::ReadU32(note.data()+29)|std::uint64_t{replayslots::ReadU32(note.data()+33)}<<32;
 }
 out=std::move(names);return true;
}
inline std::filesystem::path NamesPath(const std::filesystem::path& archive, const replayslots::Bytes& body) {
 char key[64] = {};
 std::snprintf(key, sizeof key, "%08x-%u.names", replayslots::Crc32(body.data(), body.size()), static_cast<unsigned>(body.size()));
 return archive / ".names" / key;
}
// EMBRCLS1: magic, body size, the body's SHA-256 (64 lowercase hex
// characters), then each side's band (0 for none, 1 to SkillBadge::Classes)
// and settled (0 or 1; 0 for no band): 80 bytes. Bound to one body as a names
// note is, by its digest, so a replay's classes cost a few bytes. A file of
// its own beside the names note, which keeps the form every older build
// reads. A note with no class is never written.
constexpr std::size_t kClassesBytes = 80;
inline bool AnyClass(const SkillBadge (&classes)[2]) { return classes[0].HasClass() || classes[1].HasClass(); }
inline std::string BodyDigest(const replayslots::Bytes& body) {
 return Sha256Portable(std::string(reinterpret_cast<const char*>(body.data()), body.size()));
}
inline replayslots::Bytes BindClasses(const replayslots::Bytes& body, const SkillBadge (&classes)[2]) {
 if (body.size() > replayslots::kLargestReplay || !AnyClass(classes)) return {};
 for (const auto& badge : classes) if (badge.Present() && (!badge.HasClass() || !badge.Valid())) return {};
 const auto digest = BodyDigest(body);
 if (digest.size() != 64) return {};
 replayslots::Bytes out{'E','M','B','R','C','L','S','1'};
 out.resize(kClassesBytes);
 replayslots::WriteU32(out.data()+8, static_cast<std::uint32_t>(body.size()));
 std::memcpy(out.data()+12, digest.data(), 64);
 for (int side = 0; side < 2; ++side) { out[76+side*2] = classes[side].band; out[77+side*2] = classes[side].settled ? 1 : 0; }
 return out;
}
// The classes a note binds to `body`; false, with `out` left alone, for any
// other note, another body, a band that is not a class or no class at all.
inline bool ClassesForBody(const replayslots::Bytes& note, const replayslots::Bytes& body, SkillBadge (&out)[2]) {
 if (note.size() != kClassesBytes || std::memcmp(note.data(), "EMBRCLS1", 8) || replayslots::ReadU32(note.data()+8) != body.size() ||
  body.size() > replayslots::kLargestReplay) return false;
 SkillBadge read[2];
 for (int side = 0; side < 2; ++side) {
  const std::uint8_t band = note[76+side*2], settled = note[77+side*2];
  if (band > SkillBadge::Classes || settled > 1 || (!band && settled)) return false;
  if (band) read[side] = SkillBadge::OfClass(band, settled != 0);
 }
 if (!AnyClass(read) || BodyDigest(body) != std::string(reinterpret_cast<const char*>(note.data()+12), 64)) return false;
 out[0] = read[0]; out[1] = read[1];
 return true;
}
inline std::filesystem::path ClassesPath(const std::filesystem::path& archive, const replayslots::Bytes& body) {
 auto path = NamesPath(archive, body);
 path.replace_extension(L".classes");
 return path;
}
// Fills `names.classes` from the body's classes note, when it has a good one.
inline void ReadBodyClasses(const std::filesystem::path& archive, const replayslots::Bytes& body, ReplayNames& names) {
 if (archive.empty()) return;
 auto note = ReadFile(ClassesPath(archive, body), kClassesBytes);
 if (note) ClassesForBody(*note, body, names.classes);
}
inline ReplayNames ReadBodyNames(const std::filesystem::path& archive, const replayslots::Bytes& body) {
 ReplayNames names;
 if (archive.empty()) return names;
 auto note = ReadFile(NamesPath(archive, body), kMostNamesBytes);
 if (note) NamesForBody(*note, body, names);
 ReadBodyClasses(archive, body, names);
 return names;
}
// A replay's name among Ember's files that stand for it (the outbox's): the
// SHA-256 of its bytes.
inline std::string ReplayKey(const std::uint8_t* body, std::size_t size) {
 return Sha256Portable(std::string(reinterpret_cast<const char*>(body), size));
}
// Existing unreadable/invalid/different-body targets are terminal refusals.
// Only a missing destination authorizes a create-only publication attempt.
enum class NamePublication { Published, Retryable, Refused };
template<class CreateOnly>
NamePublication PublishBodyNames(const std::filesystem::path& path, const replayslots::Bytes& body,
 const ReplayNames& names, const CreateOnly& createOnly) {
 Change existing;
 if (!Snapshot(path, kMostNamesBytes, existing)) return NamePublication::Refused;
 if (existing.existed) {
  ReplayNames bound;
  return NamesForBody(existing.before, body, bound) ? NamePublication::Published : NamePublication::Refused;
 }
 auto bytes = BindNames(body, names);
 if (bytes.empty()) return NamePublication::Refused;
 if (createOnly(bytes)) return NamePublication::Published;
 // A competing create is permanent for this job too; no further temp writes.
 std::error_code error;
 const auto status = std::filesystem::status(path, error);
 if (status.type() != std::filesystem::file_type::not_found || (error && error != std::errc::no_such_file_or_directory)) return NamePublication::Refused;
 return NamePublication::Retryable;
}
inline std::string ReplayDateLabel(std::uint64_t time) { return LocalTime(time); }
} }
