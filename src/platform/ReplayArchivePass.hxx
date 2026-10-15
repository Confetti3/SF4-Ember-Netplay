#pragma once

// Copying the game's match replays into Ember's archive (ReplaySlots.hxx has
// the files). It is a slot's file that is copied, not what the game's index
// says the slot holds: the game writes a replay's file and its ".0" as the
// match ends, and its indexes at a save that can come later or never.
//
// A pass reads the slots first, since they are what the next match takes. A
// whole replay the archive is not known to hold, byte for byte, goes into the
// outbox, a folder of its own outside the archive, as its export with a
// record made from its own header (ExportFromFile). The pass then settles
// outbox entries, a bounded number of them, so a backlog never holds up the
// next reading of the slots: settling archives an entry's replay unless the
// archive holds it, and an entry goes only once settled. Sidecar runs passes
// as the game saves; the launcher before and after a game run, settling the
// whole outbox, so one left by a game that closed is drained too, whether or
// not any game's saves are found (ReplayFiles.hxx). Portable, so
// ReplayArchivePassTest runs it on folders it builds.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "ReplayArchiveIndex.hxx"
#include "../common/ReplayFileSafety.hxx"
#include "../common/ReplayProvenance.hxx"

namespace sf4e { namespace replayfiles {

// Where a pass copies to: the archive folder and the outbox; how many outbox
// entries a pass settles at most, and where the next pass starts on them, so
// an entry that cannot be settled does not keep others waiting; the pass's
// view of the index of the replays the archive holds, one generation of it
// with the copies the pass makes (ArchiveView); the cache of the game's slot
// files; and the writer that creates a file only where there is none.
struct ArchiveTarget {
	std::filesystem::path folder, outbox;
	std::size_t settleBudget;
	std::size_t& settleFrom;
	ArchiveView& held;
	SlotFileCache& slots;
	std::function<bool(const std::filesystem::path&, const replayslots::Bytes&)> createOnly;
};

// Whether the archive holds this replay. Unknown: a file that may hold it
// could not be read, or the index was not listed whole, so the replay is
// neither copied again nor taken as held. evidence is the file that holds it.
enum class Holding { Held, Absent, Unknown };
inline Holding Holds(const ArchiveTarget& target, const replayslots::Bytes& body, BackupEvidence& evidence) {
	Holding holding = Holding::Absent;
	for (const auto& path : target.held.Candidates(replayslots::Crc32(body.data(), body.size()))) {
		ArchiveFile read;
		const ArchiveState state = ReadArchive(path, path.parent_path() == target.folder, read);
		if (state == ArchiveState::Valid && read.SameBody(body)) { evidence = {path, std::move(read)}; return Holding::Held; }
		if (state == ArchiveState::Unreadable) holding = Holding::Unknown;
	}
	return holding == Holding::Absent && !target.held.Complete() ? Holding::Unknown : holding;
}

// A slot's replay and ".0" as they are now, each empty when it could not be
// read. The replay is read through the cache; the four bytes of the ".0",
// which the game writes beside the replay, every time.
struct SlotFile { replayslots::Bytes replay, sidecar; };
inline SlotFile ReadSlotFile(SlotFileCache& cache, const std::filesystem::path& file) {
	SlotFile slot;
	if (auto replay = cache.Read(file, [](const std::filesystem::path& path) { return ReadFile(path); })) slot.replay = std::move(*replay);
	if (auto sidecar = ReadFile(file.wstring() + L".0", 4)) slot.sidecar = std::move(*sidecar);
	return slot;
}

// A slot's file as an export, with a record made from its own header, when
// its ".0" says it is whole. Never with the slot's record in the game's
// index: a record there that names the file's CRC, size and fighters may be
// an older match's that shares them, which nothing read from the save folder
// can rule out, and a wrong record would name a wrong winner.
inline bool ExportFromFile(const SlotFile& slot, replayslots::Bytes& out) {
	return slot.sidecar.size() == 4 && slot.sidecar == replayslots::Sidecar(slot.replay) && replayslots::ExportFromReplayAlone(slot.replay, out);
}

// Writes a replay's archive file and adds it to the index. False when it
// could not be written or read back.
inline bool Publish(const ArchiveTarget& target, const replayslots::Bytes& body, const replayslots::Bytes& exported) {
	const std::wstring name = replayslots::ArchiveName(exported);
	if (name.empty()) return false;
	std::error_code ignored;
	std::filesystem::create_directories(target.folder, ignored);
	const std::filesystem::path path = target.folder / name;
	ArchiveFile copied;
	if (!PublishArchive(body, exported,
			[&](ArchiveFile& existing) { return ReadArchive(path, true, existing); },
			[&](const replayslots::Bytes& bytes) { return target.createOnly(path, bytes); }) ||
		ReadArchive(path, true, copied) != ArchiveState::Valid) return false;
	target.held.Add(copied.crc, path);
	return true;
}

// An outbox entry: a replay's export, <ReplayKey>.export after its replay. It
// stands for that replay only when its body is the replay byte for byte, so
// writing it twice keeps one.
inline std::filesystem::path OutboxPath(const ArchiveTarget& target, const replayslots::Bytes& body) {
	return target.outbox / (ReplayKey(body.data(), body.size()) + ".export");
}
inline bool Exports(const replayslots::Bytes& entry, const std::uint8_t* body, std::size_t size) {
	using namespace replayslots;
	return entry.size() == kExportHeaderBytes + size && !std::memcmp(entry.data(), kExportMagic, 8) &&
		std::equal(body, body + size, entry.begin() + kExportHeaderBytes) && Describes(entry.data() + 8, body, size);
}

// Puts a replay's export into the outbox. The entry's path, or none when there
// is no outbox or the entry could not be written.
inline std::optional<std::filesystem::path> Enqueue(const ArchiveTarget& target, const replayslots::Bytes& body, const replayslots::Bytes& exported) {
	if (target.outbox.empty()) return std::nullopt;
	const std::filesystem::path path = OutboxPath(target, body);
	Change existing;
	if (!Snapshot(path, kMostArchiveBytes, existing)) return std::nullopt;
	if (existing.existed) return Exports(existing.before, body.data(), body.size()) ? std::optional<std::filesystem::path>(path) : std::nullopt;
	std::error_code ignored;
	std::filesystem::create_directories(target.outbox, ignored);
	return target.createOnly(path, exported) ? std::optional<std::filesystem::path>(path) : std::nullopt;
}

// The outbox's entries in name order. False when it could not be listed to
// the end; an outbox not made yet is an empty one.
inline bool OutboxEntries(const ArchiveTarget& target, std::vector<std::filesystem::path>& entries) {
	entries.clear();
	std::error_code error;
	std::filesystem::directory_iterator at(target.outbox, error);
	if (error) {
		std::error_code probe;
		return !std::filesystem::exists(target.outbox, probe) && !probe;
	}
	for (const std::filesystem::directory_iterator end; at != end;) {
		if (at->path().extension() == L".export") entries.push_back(at->path());
		at.increment(error);
		if (error) return false;
	}
	std::sort(entries.begin(), entries.end());
	return true;
}

// What a pass came to. copied counts the new archive files, failed the
// replays that could not be written. waiting counts the outbox entries still
// to be settled, and is not zero either when the outbox could not be listed;
// more says some of them were left for the next pass. retry says a slot is
// worth another look though the save folder may not change again.
struct ArchivePass {
	int copied = 0, failed = 0, waiting = 0;
	bool retry = false, more = false;
};

// Settles the outbox entry at path: archives its replay unless the archive
// holds it, then removes the entry. False while it waits: the replay could
// not be archived, or it is not known whether the archive holds it. A file
// there that is not an entry is left alone.
inline bool Settle(const ArchiveTarget& target, const std::filesystem::path& path, ArchivePass& pass) {
	const auto entry = ReadFile(path);
	if (!entry) return false;
	if (entry->size() < replayslots::kExportHeaderBytes ||
		!Exports(*entry, entry->data() + replayslots::kExportHeaderBytes, entry->size() - replayslots::kExportHeaderBytes)) return true;
	const replayslots::Bytes body(entry->begin() + replayslots::kExportHeaderBytes, entry->end());
	BackupEvidence evidence;
	switch (Holds(target, body, evidence)) {
	case Holding::Unknown: return false;
	case Holding::Absent:
		if (!Publish(target, body, *entry) || Holds(target, body, evidence) != Holding::Held) { pass.failed++; return false; }
		pass.copied++;
		break;
	case Holding::Held: break;
	}
	std::error_code kept;
	return std::filesystem::remove(path, kept);
}

// Settles up to the budget of outbox entries, starting where the last pass
// stopped and going round. A budget of kSettleAll settles every entry.
constexpr std::size_t kSettleAll = static_cast<std::size_t>(-1);
inline void DrainOutbox(const ArchiveTarget& target, ArchivePass& pass) {
	if (target.outbox.empty()) return;
	std::vector<std::filesystem::path> entries;
	if (!OutboxEntries(target, entries)) pass.waiting++;
	const std::size_t count = entries.size(), visits = (std::min)(count, target.settleBudget);
	for (std::size_t visit = 0; visit < visits; visit++)
		if (!Settle(target, entries[(target.settleFrom + visit) % count], pass)) pass.waiting++;
	pass.waiting += static_cast<int>(count - visits);
	pass.more = visits < count;
	target.settleFrom += visits;
}

// What one slot came to. Nothing: no replay in it, or a whole file that is
// not one (no slot of a match, a placeholder). Retry: a replay that could not
// be read or is half written, which a later pass may find whole. Held: in the
// archive. Queued: in the outbox, whatever is known of the archive. Copied:
// kept from the outbox (it cannot be written) and archived straight away, the
// archive known not to hold it. Failed: neither.
enum class SlotCopy { Nothing, Retry, Held, Queued, Copied, Failed };

inline SlotCopy CopySlot(const ArchiveTarget& target, const std::filesystem::path& saves, int slot) {
	const std::filesystem::path file = saves / std::to_wstring(slot);
	const SlotFile current = ReadSlotFile(target.slots, file);
	std::error_code missing;
	if (current.replay.empty()) return std::filesystem::exists(file, missing) ? SlotCopy::Retry : SlotCopy::Nothing;
	replayslots::ReplayHeaderInfo header;
	if (!replayslots::ReadReplayHeader(current.replay, header)) return SlotCopy::Nothing;
	replayslots::Bytes exported;
	if (!ExportFromFile(current, exported))
		return current.sidecar == replayslots::Sidecar(current.replay) ? SlotCopy::Nothing : SlotCopy::Retry;
	if (replayslots::ArchiveName(exported).empty()) return SlotCopy::Nothing;
	BackupEvidence evidence;
	const Holding holding = Holds(target, current.replay, evidence);
	if (holding == Holding::Held) return SlotCopy::Held;
	if (Enqueue(target, current.replay, exported)) return SlotCopy::Queued;
	return holding == Holding::Absent && Publish(target, current.replay, exported) ? SlotCopy::Copied : SlotCopy::Failed;
}

// One pass: the slots of each of the accounts' save folders (none at all is
// fine), then the outbox.
inline ArchivePass RunPass(const ArchiveTarget& target, const std::vector<std::filesystem::path>& accounts, int first, int last) {
	ArchivePass pass;
	for (const auto& saves : accounts) {
		for (int slot = first; slot <= last; slot++) {
			switch (CopySlot(target, saves, slot)) {
			case SlotCopy::Copied: pass.copied++; break;
			case SlotCopy::Failed: pass.failed++; pass.retry = true; break;
			case SlotCopy::Retry: pass.retry = true; break;
			default: break;
			}
		}
	}
	DrainOutbox(target, pass);
	return pass;
}

// The replay an import is about to replace has a verified copy in the archive,
// whoever made it: held already, or copied now from its slot through the
// outbox. evidence is that copy. The caller holds the one copy lock
// (ReplayFiles.cxx), so no pass decides on the slot meanwhile.
inline bool EnsureBackup(const ArchiveTarget& target, const std::filesystem::path& saves, int slot, const replayslots::Bytes& body, BackupEvidence& evidence) {
	const Holding holding = Holds(target, body, evidence);
	if (holding == Holding::Held) return true;
	const SlotFile current = ReadSlotFile(target.slots, saves / std::to_wstring(slot));
	replayslots::Bytes exported;
	if (current.replay == body && ExportFromFile(current, exported)) {
		ArchivePass pass;
		if (const auto queued = Enqueue(target, body, exported)) Settle(target, *queued, pass);
		else if (holding == Holding::Absent) Publish(target, body, exported);
	}
	return Holds(target, body, evidence) == Holding::Held;
}

} }
