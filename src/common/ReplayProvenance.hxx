#pragma once
#include "ReplaySlots.hxx"
#include "ReplayFileSafety.hxx"
#include "Sha256.hxx"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <utility>

namespace sf4e { namespace replayfiles {
// matchId and revision: the bridge set and room binding of a tournament game
// whose result was confirmed; empty and 0 for any other game.
struct ReplayNames { std::string players[2]; bool spectated = false; std::string matchId; std::uint64_t revision = 0; };
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
// match ID's size and the revision (low, high), then body, names and match ID.
// EMBRNAM1 is the same without the match ID and revision (a 21-byte head).
constexpr std::size_t kNamesHead = 33, kMostMatchId = 64, kMostNamesBytes = replayslots::kLargestReplay + kNamesHead + 2048 + kMostMatchId;
inline replayslots::Bytes BindNames(const replayslots::Bytes& body, const ReplayNames& names) {
 if (body.size() > replayslots::kLargestReplay || names.players[0].size() > 1024 || names.players[1].size() > 1024 ||
  names.matchId.size() > kMostMatchId) return {};
 replayslots::Bytes out{'E','M','B','R','N','A','M','2'};
 out.resize(kNamesHead);
 replayslots::WriteU32(out.data()+8, static_cast<std::uint32_t>(body.size()));
 for(int side=0;side<2;++side) replayslots::WriteU32(out.data()+12+side*4, static_cast<std::uint32_t>(names.players[side].size()));
 out[20] = names.spectated ? 1 : 0;
 replayslots::WriteU32(out.data()+21, static_cast<std::uint32_t>(names.matchId.size()));
 replayslots::WriteU32(out.data()+25, static_cast<std::uint32_t>(names.revision));
 replayslots::WriteU32(out.data()+29, static_cast<std::uint32_t>(names.revision >> 32));
 out.insert(out.end(),body.begin(),body.end());
 for(const auto& name:names.players) out.insert(out.end(),name.begin(),name.end());
 out.insert(out.end(),names.matchId.begin(),names.matchId.end());
 return out;
}
inline bool NamesForBody(const replayslots::Bytes& note, const replayslots::Bytes& body, ReplayNames& out) {
 if(note.size()<21 || std::memcmp(note.data(),"EMBRNAM",7) || (note[7]!='1' && note[7]!='2') ||
  replayslots::ReadU32(note.data()+8)!=body.size() || note[20]>1) return false;
 const bool tagged=note[7]=='2';
 const std::size_t head=tagged?kNamesHead:21;
 if(note.size()<head) return false;
 const auto p1=replayslots::ReadU32(note.data()+12),p2=replayslots::ReadU32(note.data()+16);
 const auto id=tagged?replayslots::ReadU32(note.data()+21):0;
 if(p1>1024 || p2>1024 || id>kMostMatchId || body.size()>replayslots::kLargestReplay || note.size()!=head+body.size()+p1+p2+id ||
  std::memcmp(note.data()+head,body.data(),body.size())) return false;
 ReplayNames names;
 const auto* text=reinterpret_cast<const char*>(note.data()+head+body.size());
 names.players[0].assign(text,p1);names.players[1].assign(text+p1,p2);names.spectated=note[20]!=0;
 if(tagged){
  names.matchId.assign(text+p1+p2,id);
  names.revision=replayslots::ReadU32(note.data()+25)|std::uint64_t{replayslots::ReadU32(note.data()+29)}<<32;
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
