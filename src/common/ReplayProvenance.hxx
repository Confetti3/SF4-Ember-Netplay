#pragma once
#include "ReplaySlots.hxx"
#include "ReplayFileSafety.hxx"
#include "Sha256.hxx"
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
struct ReplayNames { std::string players[2]; bool spectated = false; SetTag set; };
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
// EMBRNAM2: magic, body size, the two name sizes, spectated (byte 20), the
// match and bridge IDs' sizes and the revision (low, high), then body, names,
// match ID and bridge ID. EMBRNAM1 is the same without the tag (a 21-byte head).
constexpr std::size_t kNamesHead = 37, kMostId = 64, kMostNamesBytes = replayslots::kLargestReplay + kNamesHead + 2048 + 2 * kMostId;
inline replayslots::Bytes BindNames(const replayslots::Bytes& body, const ReplayNames& names) {
 if (body.size() > replayslots::kLargestReplay || names.players[0].size() > 1024 || names.players[1].size() > 1024 ||
  names.set.matchId.size() > kMostId || names.set.bridgeId.size() > kMostId) return {};
 replayslots::Bytes out{'E','M','B','R','N','A','M','2'};
 out.resize(kNamesHead);
 replayslots::WriteU32(out.data()+8, static_cast<std::uint32_t>(body.size()));
 for(int side=0;side<2;++side) replayslots::WriteU32(out.data()+12+side*4, static_cast<std::uint32_t>(names.players[side].size()));
 out[20] = names.spectated ? 1 : 0;
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
 names.players[0].assign(text,p1);names.players[1].assign(text+p1,p2);names.spectated=note[20]!=0;
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
inline ReplayNames ReadBodyNames(const std::filesystem::path& archive, const replayslots::Bytes& body) {
 ReplayNames names;
 if (archive.empty()) return names;
 auto note = ReadFile(NamesPath(archive, body), kMostNamesBytes);
 if (note) NamesForBody(*note, body, names);
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
inline std::string ReplayDateLabel(std::uint64_t time) {
 const std::time_t at = static_cast<std::time_t>(time);
 std::tm local = {};
#ifdef _WIN32
 if (localtime_s(&local, &at)) return {};
#else
 if (!localtime_r(&at, &local)) return {};
#endif
 char label[32] = {};
 return std::strftime(label, sizeof label, "%Y-%m-%d %H:%M", &local) ? label : std::string();
}
} }
