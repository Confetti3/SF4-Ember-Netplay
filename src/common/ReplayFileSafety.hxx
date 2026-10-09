#pragma once

#include "ReplaySlots.hxx"
#include "BoundedRead.hxx"
#include <algorithm>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <optional>

namespace sf4e { namespace replayfiles {

struct Change {
	std::string name;
	replayslots::Bytes before, after;
	bool existed = false;
	// Exact local destination, captured by import preparation; Steam's name
	// alone does not identify the account's file for commit-time validation.
	std::filesystem::path path;
};

constexpr std::size_t kMostArchiveBytes = replayslots::kLargestReplay + replayslots::kExportHeaderBytes;

// The single bounded reader for indexes, archives, snapshots and detail.
// Missing, unreadable, oversized and interrupted reads are failures; an
// existing empty file is a successful read of zero bytes.
inline std::optional<replayslots::Bytes> ReadFile(const std::filesystem::path& path, std::size_t most = kMostArchiveBytes) {
	auto read = durable::ReadBounded(path, most);
	if (read.status != durable::ReadStatus::Read) return std::nullopt;
	return std::move(read.bytes);
}

// Missing files are distinct from empty files; an unreadable file cannot be replaced.
inline bool Snapshot(const std::filesystem::path& path, std::size_t most, Change& change) {
	auto read = durable::ReadBounded(path, most);
	if (read.status == durable::ReadStatus::Missing) { change.existed = false; change.before.clear(); return true; }
	if (read.status != durable::ReadStatus::Read) return false;
	change.before = std::move(read.bytes); change.existed = true;
	return true;
}

// The archive reader owns validation for both listing and backup proof.
struct ArchiveFile {
 replayslots::Bytes contents;
 std::size_t bodyOffset = 0;
 std::uint64_t time = 0;
 std::uint32_t crc = 0;
 int fighters[2] = {-1, -1};
 bool SameBody(const replayslots::Bytes& body) const {
  return contents.size() >= bodyOffset && contents.size() - bodyOffset == body.size() &&
   std::equal(body.begin(), body.end(), contents.begin() + bodyOffset);
 }
 replayslots::Bytes Body() const { return {contents.begin() + bodyOffset, contents.end()}; }
};
// Missing and unreadable are different publication decisions. Invalid means
// the complete bounded file was read successfully but failed validation.
enum class ArchiveState { Missing, Unreadable, Invalid, Valid };
inline ArchiveState ReadArchive(const std::filesystem::path& path, bool inRoot, ArchiveFile& out) {
 auto file = durable::ReadBounded(path, kMostArchiveBytes);
 if (file.status == durable::ReadStatus::Missing) return ArchiveState::Missing;
 if (file.status != durable::ReadStatus::Read) return ArchiveState::Unreadable;
 auto& bytes = file.bytes;
 ArchiveFile read;
 if (path.extension() == L".usf4replay") {
  replayslots::ReplayHeaderInfo header;
  if (bytes.size() > replayslots::kLargestReplay || !replayslots::ReadReplayHeader(bytes, header)) return ArchiveState::Invalid;
  read.time = header.time;
  read.fighters[0] = header.fighters[0]; read.fighters[1] = header.fighters[1];
  read.crc = replayslots::Crc32(bytes.data(), bytes.size());
 } else {
  if (!inRoot || !replayslots::ParseArchiveName(path.filename().wstring(), read.time, read.crc) || !replayslots::WholeArchived(bytes, read.crc)) return ArchiveState::Invalid;
  const auto info = replayslots::ReadRecordInfo(bytes.data() + 8);
  read.fighters[0] = info.fighters[0]; read.fighters[1] = info.fighters[1];
  read.bodyOffset = replayslots::kExportHeaderBytes;
 }
 read.contents = std::move(bytes);
 out = std::move(read);
 return ArchiveState::Valid;
}
// One publication policy, used with the actual create-only writer. Even a
// confirmed damaged target is preserved for manual repair: a prior read
// cannot authorize replacing a file another process may have just published.
template<class Reader, class CreateOnly>
bool PublishArchive(const replayslots::Bytes& body, const replayslots::Bytes& exported,
 const Reader& read, const CreateOnly& createOnly) {
 ArchiveFile existing;
 switch (read(existing)) {
 case ArchiveState::Valid: return existing.SameBody(body);
 case ArchiveState::Missing: return createOnly(exported);
 default: return false;
 }
}
struct BackupEvidence { std::filesystem::path path; ArchiveFile replay; };
// The cached index only locates candidates. Read each candidate now, and
// compare the exact body, so CRC collisions and stale entries cannot waive
// the backup requirement. Keep this evidence with the prepared transaction.
inline bool VerifyBackup(const std::vector<std::filesystem::path>& candidates, const std::filesystem::path& archive,
 const replayslots::Bytes& body, BackupEvidence& out) {
 for (const auto& path : candidates) {
  ArchiveFile read;
  if (ReadArchive(path, path.parent_path() == archive, read) == ArchiveState::Valid && read.SameBody(body)) {
   out = {path, std::move(read)}; return true;
  }
 }
 return false;
}

// Called inside Apply's pre-write gate, on the game owner. Notifications can
// lag cached writes, so only current exact bytes and existence authorize the
// prepared overwrite/rollback snapshots. Recheck the whole verified archive
// artifact too: retained memory is not proof of a surviving durable backup.
inline bool ImportFilesUnchanged(const std::vector<Change>& changes, const BackupEvidence& backup) {
 for (const auto& change : changes) {
  Change current;
  if (change.path.empty() || !Snapshot(change.path, change.before.size(), current) ||
   current.existed != change.existed || current.before != change.before) return false;
 }
 if (!backup.path.empty()) {
  const auto current = ReadFile(backup.path, backup.replay.contents.size());
  if (!current || *current != backup.replay.contents) return false;
 }
 return true;
}

enum class ApplyOutcome { RejectedBeforeWrite, Done, FailedRestored, RecoveryIncomplete };

// Keep all snapshots until the live table accepts the files. A failed write
// may have changed its file too, so every attempted write is undone.
template<class Fresh, class Writer, class Remover, class Publish>
ApplyOutcome Apply(const std::vector<Change>& changes, const Fresh& fresh, const Writer& write, const Remover& remove, const Publish& publish) noexcept {
	try { if (!fresh()) return ApplyOutcome::RejectedBeforeWrite; } catch (...) { return ApplyOutcome::RejectedBeforeWrite; }
	std::size_t attempted = 0;
	bool restored = true;
	try {
		bool written = true;
		for (const auto& change : changes) {
			++attempted;
			if (!write(change.name, change.after)) { written = false; break; }
		}
		if (written && publish()) return ApplyOutcome::Done;
	} catch (...) {}
	while (attempted) {
		const auto& change = changes[--attempted];
		try {
			const bool ok = change.existed ? write(change.name, change.before) : remove(change.name);
			if (!ok) restored = false;
		} catch (...) { restored = false; }
	}
	return restored ? ApplyOutcome::FailedRestored : ApplyOutcome::RecoveryIncomplete;
}

} }
