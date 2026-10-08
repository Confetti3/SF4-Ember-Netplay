#pragma once
#include "ReplaySlots.hxx"
#include "ReplayFileSafety.hxx"
#include <cstdio>
#include <ctime>
#include <utility>

namespace sf4e { namespace replayfiles {
struct ReplayNames { std::string players[2]; bool spectated = false; };
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
inline replayslots::Bytes BindNames(const replayslots::Bytes& body, const ReplayNames& names) {
 if (body.size() > replayslots::kLargestReplay || names.players[0].size() > 1024 || names.players[1].size() > 1024) return {};
 replayslots::Bytes out{'E','M','B','R','N','A','M','1'};
 out.resize(20);
 replayslots::WriteU32(out.data()+8, static_cast<std::uint32_t>(body.size()));
 for(int side=0;side<2;++side) replayslots::WriteU32(out.data()+12+side*4, static_cast<std::uint32_t>(names.players[side].size()));
 out.push_back(names.spectated ? 1 : 0);
 out.insert(out.end(),body.begin(),body.end());
 for(const auto& name:names.players) out.insert(out.end(),name.begin(),name.end());
 return out;
}
inline bool NamesForBody(const replayslots::Bytes& note, const replayslots::Bytes& body, ReplayNames& out) {
 if(note.size()<21 || std::memcmp(note.data(),"EMBRNAM1",8) || replayslots::ReadU32(note.data()+8)!=body.size() || note[20]>1) return false;
 const auto p1=replayslots::ReadU32(note.data()+12),p2=replayslots::ReadU32(note.data()+16);
 if(p1>1024 || p2>1024 || body.size()>replayslots::kLargestReplay || note.size()!=21+body.size()+p1+p2 ||
  std::memcmp(note.data()+21,body.data(),body.size())) return false;
 ReplayNames names;
 const auto* text=reinterpret_cast<const char*>(note.data()+21+body.size());
 names.players[0].assign(text,p1);names.players[1].assign(text+p1,p2);names.spectated=note[20]!=0;
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
 auto note = ReadFile(NamesPath(archive, body), replayslots::kLargestReplay + 21 + 2048);
 if (note) NamesForBody(*note, body, names);
 return names;
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
