#include "ReplayFiles.hxx"

#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "Utf8.hxx"
#include "ReplayPath.hxx"
#include "../common/ReplayFileSafety.hxx"
#include "../common/ReplayInputDetails.hxx"
#include "../common/ReplayProvenance.hxx"

namespace fs = std::filesystem;
namespace slots = sf4e::replayslots;

namespace sf4e { namespace platform { namespace replays {
namespace {

// The largest files read: an exported replay, and the game's two indexes
// (76 KB and 44 KB as the game writes them).
constexpr std::size_t kMostReplayBytes = slots::kLargestReplay + slots::kExportHeaderBytes, kMostIndexBytes = 1 << 20;

// All ingestion uses replayfiles::ReadFile and its bounded read-error policy.
slots::Bytes LoadFile(const fs::path& path, std::size_t most = kMostReplayBytes) {
 auto bytes = replayfiles::ReadFile(path, most);
 return bytes ? std::move(*bytes) : slots::Bytes{};
}

// Written under another name first, so a cut-off write is never taken for a
// finished file. The name is this process's and this write's own: the
// launcher and the game may both be archiving the same replay.
bool SaveFile(const fs::path& path, const slots::Bytes& contents, bool replaceDamaged = false) {
	static std::atomic<unsigned> serial{0};
	const fs::path partial = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial) + L".tmp";
 HANDLE handle = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
 if (handle == INVALID_HANDLE_VALUE) return false;
 DWORD written = 0;
 const bool ok = contents.size() <= MAXDWORD && WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
  written == contents.size() && FlushFileBuffers(handle);
 const bool closed = CloseHandle(handle) != 0;
 if (ok && closed && MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | (replaceDamaged ? MOVEFILE_REPLACE_EXISTING : 0))) return true;
 DeleteFileW(partial.c_str());
 return false;
}

fs::path NamesPath(const fs::path& archive, const slots::Bytes& body) {
 wchar_t key[64] = {};
 StringCchPrintfW(key, 64, L"%08x-%u.names", slots::Crc32(body.data(), body.size()), static_cast<unsigned>(body.size()));
 return archive / L".names" / key;
}
void ReadNames(const fs::path& archive, const slots::Bytes& body, ArchivedReplay& replay) {
 replay.names[0].clear(); replay.names[1].clear(); replay.spectated = false;
 auto note = replayfiles::ReadFile(NamesPath(archive, body), slots::kLargestReplay + 21 + 2048);
 replayfiles::ReplayNames names;
 if (note && replayfiles::NamesForBody(*note, body, names)) {
  replay.names[0] = names.players[0]; replay.names[1] = names.players[1]; replay.spectated = names.spectated;
 }
}
struct PendingNames {
 fs::path saves, archive;
 std::vector<replayfiles::RecordedSlot> before;
 replayfiles::ReplayNames names;
 std::uint64_t started = 0, ended = 0;
 int fighters[2] = {-1, -1};
};
std::mutex namesMutex;
PendingNames pendingNames;
std::vector<replayfiles::RecordedSlot> RecordedSlots(const fs::path& saves) {
 std::vector<replayfiles::RecordedSlot> slotsNow;
 for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; ++slot) {
  const auto path = saves / std::to_wstring(slot);
  replayfiles::Change body, checksum;
  if (!replayfiles::Snapshot(path, slots::kLargestReplay, body) || !replayfiles::Snapshot(path.wstring() + L".0", 4, checksum))
   throw std::runtime_error("recording slots could not be read");
  slotsNow.push_back({std::move(body.before), std::move(checksum.before)});
 }
 return slotsNow;
}
// Caller holds namesMutex. Retry only this closed recording before another
// recording begins, never attach old notes to arbitrary archive files.
void BindPendingNames() {
 if (pendingNames.saves.empty() || !pendingNames.ended) return;
 const auto after = RecordedSlots(pendingNames.saves);
 const auto* body = replayfiles::RecordedBody(pendingNames.before, after, pendingNames.started, pendingNames.ended, pendingNames.fighters);
 if (!body) return;
 const auto path = NamesPath(pendingNames.archive, *body);
 std::error_code error;
 fs::create_directories(path.parent_path(), error);
 const auto bytes = replayfiles::BindNames(*body, pendingNames.names);
 auto existing = replayfiles::ReadFile(path, slots::kLargestReplay + 21 + 2048);
 replayfiles::ReplayNames bound;
 if (existing && replayfiles::NamesForBody(*existing, *body, bound)) { pendingNames = {}; return; }
 if (!bytes.empty() && SaveFile(path, bytes)) pendingNames = {};
}

// "20261005-213503-69991186.emberreplay": the save time (UTC) and the CRC,
// both from the export's record. ParseArchiveName reads it back.
bool ArchiveName(const slots::Bytes& exported, wchar_t (&name)[64]) {
	const __time64_t saved = slots::ReadU32(exported.data() + 8 + 13);
	tm utc = {};
	return !_gmtime64_s(&utc, &saved) && SUCCEEDED(StringCchPrintfW(name, 64, L"%04d%02d%02d-%02d%02d%02d-%08x.emberreplay",
		utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, slots::ReadU32(exported.data() + 8 + 5)));
}

// What a replay's own file says of it: whose it is, when it was saved, and
// the CRC of the replay in it. Ember's files sit in the archive root, named
// by their save time and CRC, and count only when they are the export that
// name says (WholeArchived); usf4-replay-saver's (.usf4replay, the game's
// file as it is) may be dropped in any folder under it and tell their time
// and fighters themselves. False for any other file.
bool ReadArchiveRow(const fs::path& path, bool inRoot, ArchivedReplay& replay, std::uint32_t& crc, slots::Bytes& body, slots::Bytes& identity) {
 replayfiles::ArchiveFile read;
 if (!replayfiles::ReadArchive(path, inRoot, read)) return false;
 replay = ArchivedReplay{path};
 replay.fighters[0] = read.fighters[0]; replay.fighters[1] = read.fighters[1];
 const std::uint64_t time = read.time;
 crc = read.crc;
 body = std::move(read.body);
	const __time64_t at = static_cast<__time64_t>(time);
	tm local = {};
	char label[32] = { 0 };
	if (_localtime64_s(&local, &at) || !std::strftime(label, sizeof(label), "%Y-%m-%d %H:%M", &local)) return false;
	replay.label = label; replay.time = time;
 identity = std::move(read.contents);
	return true;
}

// The archive as this process last read it: every replay file by its path,
// with its complete bytes, so summaries are reused only for identical files.
// The listing, the archiver and the import's check all read this one
// index. Refresh reads the folder on the caller's thread; Candidates locates
// possible backups and Verify freshly reads only those files.
class ArchiveIndex {
public:
	struct Entry { bool replay = false; std::uint32_t crc = 0; ArchivedReplay read; bool summarized = false; slots::Bytes body, identity; };

	// Brings the index up to the folder. The files are read without the lock,
	// so a thread that only asks is never kept waiting for a disk.
	std::vector<ArchivedReplay> Refresh(const fs::path& archive) {
		std::map<std::wstring, Entry> kept, seen;
		{ std::lock_guard<std::mutex> lock(mutex_); kept = files_; }
		std::vector<ArchivedReplay> replays;
		std::error_code ignored;
		for (fs::recursive_directory_iterator at(archive, fs::directory_options::skip_permission_denied, ignored), end; !ignored && at != end; at.increment(ignored)) {
			const fs::path& path = at->path();
			const std::wstring extension = path.extension().wstring();
			if (extension != L".emberreplay" && extension != L".usf4replay") continue;
			std::error_code failed;
			// A directory by a replay's name is not one.
			if (!at->is_regular_file(failed) || failed) continue;
			const auto before = kept.find(path.wstring());
			Entry entry;
   entry.replay = ReadArchiveRow(path, path.parent_path() == archive, entry.read, entry.crc, entry.body, entry.identity);
   if (entry.replay) {
    if (before != kept.end() && before->second.summarized && before->second.identity == entry.identity) entry.read.summary = before->second.read.summary;
    else {
     // Worker/launcher only. Add leaves parsing for this refresh.
     replayinputs::Match played;
     if (replayinputs::Parse(entry.identity.data(), entry.identity.size(), played)) entry.read.summary = replayinputs::Summarize(played);
    }
    entry.summarized = true;
    ReadNames(archive, entry.body, entry.read); replays.push_back(entry.read);
   }
			seen.emplace(path.wstring(), std::move(entry));
		}
		std::lock_guard<std::mutex> lock(mutex_);
		// A file archived while the folder was being read stays known.
		for (auto& added : added_) seen.emplace(added.first, std::move(added.second));
		added_.clear();
		files_ = std::move(seen);
		return replays;
	}
 std::vector<fs::path> Candidates(std::uint32_t crc) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<fs::path> paths;
  const auto append = [&](const std::map<std::wstring, Entry>& files) {
   for (const auto& file : files) if (file.second.replay && file.second.crc == crc) paths.emplace_back(file.first);
  };
  append(files_); append(added_);
  return paths;
 }
 bool Verify(const fs::path& archive, const slots::Bytes& body, replayfiles::BackupEvidence& evidence) const {
  return replayfiles::VerifyBackup(Candidates(slots::Crc32(body.data(), body.size())), archive, body, evidence);
 }
	// A file this process just wrote into the archive.
	bool Add(const fs::path& path) {
		Entry entry;
		entry.replay = ReadArchiveRow(path, true, entry.read, entry.crc, entry.body, entry.identity);
		if (!entry.replay) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		added_[path.wstring()] = std::move(entry);
		return true;
	}
private:
	mutable std::mutex mutex_;
	std::map<std::wstring, Entry> files_, added_;
};
ArchiveIndex& Index() { static ArchiveIndex index; return index; }

// A slot of the game's as its files are now, kept by the replay file's size
// and time so its 50 KB are read and summed once.
struct SlotFile { slots::Bytes replay, sidecar; };
SlotFile ReadSlotFile(const fs::path& file) {
	struct Kept { std::uintmax_t size; fs::file_time_type written; slots::Bytes replay; };
	static std::mutex mutex;
	static std::map<std::wstring, Kept> cache;
	std::error_code failed;
	const std::uintmax_t size = fs::file_size(file, failed);
	const fs::file_time_type written = failed ? fs::file_time_type{} : fs::last_write_time(file, failed);
	SlotFile slot;
	bool kept = false;
	if (!failed) {
		// The lock is held only to look and to store, never while a file is read.
		std::lock_guard<std::mutex> lock(mutex);
		const auto at = cache.find(file.wstring());
		kept = at != cache.end() && at->second.size == size && at->second.written == written;
		if (kept) slot.replay = at->second.replay;
	}
	if (!failed && !kept) {
		slot.replay = LoadFile(file);
		std::lock_guard<std::mutex> lock(mutex);
		cache[file.wstring()] = Kept{size, written, slot.replay};
	}
	// Four bytes, and the game writes them after the replay: read every time.
	slot.sidecar = LoadFile(file.wstring() + L".0", 4);
	return slot;
}

// Copies one slot's replay into the archive unless it is held there already.
// Nothing: no replay there, held already, or the game's index has not caught
// up with a file written in the last two minutes.
enum class Archived { Copied, Nothing, Failed };
Archived ArchiveSlot(const fs::path& archive, const fs::path& saves, int slot, const slots::Bytes& list, const slots::Bytes& swan) {
	// The file is what is archived, not what the index says the slot holds:
	// the index can be behind it (ReplaySlots.hxx: ExportSlotFile).
	const fs::path file = saves / std::to_wstring(slot);
	const SlotFile now = ReadSlotFile(file);
	slots::ReplayHeaderInfo header;
	replayfiles::BackupEvidence evidence;
	if (!slots::ReadReplayHeader(now.replay, header) || Index().Verify(archive, now.replay, evidence)) return Archived::Nothing;
	wchar_t name[64] = { 0 };
	slots::Bytes exported;
	bool fromRecord = false;
	if (!slots::ExportSlotFile(list, swan, slot, now.replay, now.sidecar, exported, fromRecord) || !ArchiveName(exported, name)) {
		spdlog::debug("Replays: slot {} is not a whole replay, not archived", slot);
		return Archived::Nothing;
	}
	std::error_code ignored;
	// Two minutes for the game to write the slot's record, which says more
	// than the file's header does.
	if (!fromRecord && fs::file_time_type::clock::now() - fs::last_write_time(file, ignored) < std::chrono::minutes(2)) return Archived::Nothing;
	if (!fromRecord) spdlog::info("Replays: slot {} holds a replay the game's index does not list; archived from the file alone", slot);
	fs::create_directories(archive, ignored);
 replayfiles::ArchiveFile existing;
 if (replayfiles::ReadArchive(archive / name, true, existing)) {
  if (existing.body != now.replay) return Archived::Failed; // valid naming collision: preserve both originals
 } else if (!SaveFile(archive / name, exported, true)) {
  spdlog::warn("Replays: could not write the copy of slot {}", slot); return Archived::Failed;
 }
	if (!Index().Add(archive / name)) { spdlog::warn("Replays: the copy of slot {} could not be verified", slot); return Archived::Failed; }
	return Archived::Copied;
}

// Recovery lives beyond ImportFile. A failed restoration blocks subsequent
// imports, preserving both the exact originals and the verified backup body.
struct PreparedTransaction {
 std::vector<replayfiles::Change> changes;
 replayfiles::BackupEvidence backup;
 fs::path recoveryFile;
 slots::Bytes recoveryBytes;
};
std::mutex importMutex;
std::optional<PreparedTransaction> failedRecovery;

// Pre-encode while no save has been changed. If recovery fails, persist this
// exact material beside the archive as well as retaining it in this process.
slots::Bytes RecoveryBytes(const fs::path& saves, const std::vector<replayfiles::Change>& changes) {
 slots::Bytes bytes{'E','M','B','R','R','E','C','1'};
 const auto number = [&](std::uint32_t n) { const auto at = bytes.size(); bytes.resize(at + 4); slots::WriteU32(bytes.data()+at,n); };
 const auto account = WideToUtf8(saves.wstring());
 number(static_cast<std::uint32_t>(account.size()));
 bytes.insert(bytes.end(),account.begin(),account.end());
 number(static_cast<std::uint32_t>(changes.size()));
 for (const auto& change : changes) {
  number(static_cast<std::uint32_t>(change.name.size()));
  number(static_cast<std::uint32_t>(change.before.size()));
  bytes.push_back(change.existed ? 1 : 0);
  bytes.insert(bytes.end(),change.name.begin(),change.name.end());
  bytes.insert(bytes.end(),change.before.begin(),change.before.end());
 }
 return bytes;
}

const std::string kSavePrefix = "capcom/superstreetfighteriv/ssf4_savedata/";

}

Folders FindFolders() {
	Folders folders;
	wchar_t steamPath[1024] = { 0 };
	DWORD steamPathBytes = sizeof(steamPath);
	PWSTR appData = NULL;
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steamPath, &steamPathBytes) != ERROR_SUCCESS ||
		SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appData) != S_OK) return folders;
	folders.archive = fs::path(appData) / L"sf4e" / L"replays";
	CoTaskMemFree(appData);
	// The account signed in to the running Steam client, which is the one a
	// running game reads and writes as.
	DWORD active = 0, activeBytes = sizeof(active);
	RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD, NULL, &active, &activeBytes);
	std::error_code ignored;
	for (fs::directory_iterator at(fs::path(steamPath) / L"userdata", ignored), end; !ignored && at != end; at.increment(ignored)) {
		const fs::path saves = at->path() / L"45760" / L"remote" / L"capcom" / L"superstreetfighteriv" / L"ssf4_savedata";
		if (!fs::exists(saves / L"replays-swan.dat", ignored)) continue;
		folders.saves.push_back(saves);
		if (active && at->path().filename().wstring() == std::to_wstring(active)) folders.active = saves;
	}
	return folders;
}

int Archive() {
	try {
		const Folders folders = FindFolders();
		if (folders.archive.empty()) return -1;
		Index().Refresh(folders.archive);
		int copied = 0;
		for (const fs::path& saves : folders.saves) {
			const slots::Bytes list = LoadFile(saves / L"LIST", kMostIndexBytes), swan = LoadFile(saves / L"replays-swan.dat", kMostIndexBytes);
			if (!slots::ValidSwan(swan)) continue;
			for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; slot++) copied += ArchiveSlot(folders.archive, saves, slot, list, swan) == Archived::Copied;
		}
		if (copied) spdlog::info(L"Replays: archived {} to {}", copied, folders.archive.c_str());
		return copied;
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: archiving stopped: {}", e.what());
		return -1;
	}
}

ImportResult ImportFile(const fs::path& file, const Writer& write, const Remover& remove, const Publisher& publish, Imported& out) {
 std::lock_guard<std::mutex> transactionLock(importMutex);
 if (failedRecovery) return ImportResult::RecoveryIncomplete;
 ImportResult outcome = ImportResult::RejectedBeforeWrite;
	try {
  const auto resolved = ResolveReplayFile(WideToUtf8(file.wstring()));
  if (resolved.empty()) return ImportResult::NotAReplay;
  const fs::path source = fs::u8path(resolved);
		const Folders folders = FindFolders();
		// The files written through `write` are the running account's, so the
		// indexes are read from that account and no other.
		const fs::path& saves = folders.active;
		if (saves.empty() || folders.archive.empty()) { spdlog::warn("Replays: no save folder for the Steam account that is signed in"); return ImportResult::NoFolder; }
		slots::SlotFiles before;
		before.list = LoadFile(saves / L"LIST", kMostIndexBytes); before.listSidecar = LoadFile(saves / L"LIST.0", 4);
		before.swan = LoadFile(saves / L"replays-swan.dat", kMostIndexBytes); before.swanSidecar = LoadFile(saves / L"replays-swan.dat.0", 4);
		// An index that is not the one its ".0" names is half written, or
		// damaged: it is not built on and not written out again.
		if (!slots::ValidList(before.list) || !slots::ValidSwan(before.swan) ||
			before.listSidecar != slots::Sidecar(before.list) || before.swanSidecar != slots::Sidecar(before.swan)) {
			spdlog::warn("Replays: the game's replay index does not match its checksum file; nothing is imported over it");
			return ImportResult::IndexDamaged;
		}
		// The game writes a match's replay file as the match ends and its
		// index at a later save. Until then the index still has that slot as
		// its oldest, and choosing by it would put the import over the match
		// just played.
		for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; slot++) {
			const fs::path slotFile = saves / std::to_wstring(slot);
			const SlotFile now = ReadSlotFile(slotFile);
			if (!slots::FileAheadOfRecord(before.list, before.swan, slot, now.replay, now.sidecar)) continue;
			// An index saved since the file was written is not behind it: the
			// file is one the game no longer lists, and waits for nothing.
			std::error_code failed;
			const auto indexWritten = fs::last_write_time(saves / (slot < slots::kListSlots ? L"LIST" : L"replays-swan.dat"), failed);
			const auto fileWritten = failed ? fs::file_time_type{} : fs::last_write_time(slotFile, failed);
			if (!failed && fileWritten <= indexWritten) continue;
			spdlog::info("Replays: slot {} holds a replay the game has not put in its index yet; the import waits for that", slot);
			return ImportResult::IndexBehind;
		}
		const int slot = slots::SlotToReplace(before.list, before.swan, kFirstMatchSlot, kLastMatchSlot);
		if (slot < 0) { spdlog::warn("Replays: the game's index has no match slot"); return ImportResult::IndexDamaged; }
		slots::Bytes exported = LoadFile(source);
		// usf4-replay-saver keeps the game's replay as it is and the slot
		// record beside it, in .index/<crc>.entry.
		if (exported.size() >= 4 && !std::memcmp(exported.data(), "#BRP", 4)) {
			wchar_t crc[16] = { 0 };
			StringCchPrintfW(crc, 16, L"%08x", slots::Crc32(exported.data(), exported.size()));
			const slots::Bytes entry = LoadFile(source.parent_path() / L".index" / (std::wstring(crc) + L".entry"), slots::kRecordBytes);
			slots::Bytes built;
			// Without that entry, the record is made up from the replay's own header.
			if (!slots::ExportFromReplay(exported, entry, built) && !slots::ExportFromReplayAlone(exported, built)) {
				spdlog::warn(L"Replays: {} has no slot record beside it and no readable header", file.c_str()); return ImportResult::NotAReplay;
			}
			exported = std::move(built);
		}
		// The replay the slot holds now has to be in the archive before it
		// goes: copied there now if it is not, and nothing is written if that
		// cannot be done. The index is the one the listing keeps; no folder
		// is walked here.
		const SlotFile held = ReadSlotFile(saves / std::to_wstring(slot));
		before.replay = held.replay; before.replaySidecar = held.sidecar;
  PreparedTransaction transaction;
  slots::ReplayHeaderInfo header;
  if (slots::ReadReplayHeader(held.replay, header) && !Index().Verify(folders.archive, held.replay, transaction.backup)) {
   if (ArchiveSlot(folders.archive, saves, slot, before.list, before.swan) != Archived::Copied ||
    !Index().Verify(folders.archive, held.replay, transaction.backup)) {
    spdlog::warn("Replays: slot {} could not be backed up and verified; it is not replaced", slot);
    return ImportResult::ArchiveFailed;
   }
  }
		slots::WritePlan plan;
		if (!slots::PlanImport(exported, slot, static_cast<std::uint32_t>(_time64(nullptr)), before, plan)) {
			spdlog::warn(L"Replays: {} is not a replay Ember can import", file.c_str());
			return ImportResult::NotAReplay;
		}
		// Prepare the live-table record before writing, so an allocation failure
		// cannot leave the files ahead of the table.
		const slots::Bytes& swan = plan[2].now;
		const std::uint8_t* record = slots::Record(slot < slots::kListSlots ? plan[4].now : before.list, swan, slot);
		if (!record) return ImportResult::IndexDamaged;
		Imported imported;
		imported.slot = slot;
		imported.record.assign(record, record + slots::kRecordBytes);
		imported.slotBytes.assign(swan.begin() + slots::kSwanSlotBytesOffset + slot * 2, swan.begin() + slots::kSwanSlotBytesOffset + slot * 2 + 2);
		// Execute only this plan, with nightly's exact-file snapshots and
		// exception-safe rollback. Empty and missing files stay distinct.
		auto& changes = transaction.changes;
		for (const slots::PlannedWrite& step : plan) {
			replayfiles::Change change;
			change.name = kSavePrefix + step.name; change.after = step.now;
			if (!replayfiles::Snapshot(saves / step.name, kMostIndexBytes, change)) return ImportResult::RejectedBeforeWrite;
			// The plan must describe the files we are about to replace. A game
			// save during preparation requires a fresh plan, never stale indexes.
			if (change.before != step.before) return ImportResult::IndexBehind;
			changes.push_back(std::move(change));
		}
  transaction.recoveryBytes = RecoveryBytes(saves, changes);
  static std::atomic<unsigned> recoverySerial{0};
  transaction.recoveryFile = folders.archive / (L"recovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++recoverySerial) + L".ember-recovery");
  const auto applied = replayfiles::Apply(changes, write, remove, [&] { return publish(imported); });
  if (applied != replayfiles::ApplyOutcome::Done) {
   outcome = applied == replayfiles::ApplyOutcome::FailedRestored ? ImportResult::FailedRestored : ImportResult::RecoveryIncomplete;
   if (outcome == ImportResult::RecoveryIncomplete) {
    failedRecovery.emplace(std::move(transaction)); // move only; no allocation after a failed rollback
    const bool persisted = SaveFile(failedRecovery->recoveryFile, failedRecovery->recoveryBytes);
    spdlog::error(L"Replays: restoration incomplete; originals retained in this process; recovery file {} {}", failedRecovery->recoveryFile.c_str(), persisted ? L"written" : L"could not be written");
   } else spdlog::error("Replays: import failed; every original file was restored");
   return outcome;
  }
  outcome = ImportResult::Done;
		out = std::move(imported);
		spdlog::info(L"Replays: imported {} into slot {}", file.c_str(), slot);
		return ImportResult::Done;
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: import stopped: {}", e.what());
		return outcome;
	}
 catch (...) { return outcome; }
}

void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating, int fighter1, int fighter2) {
 std::lock_guard<std::mutex> lock(namesMutex);
 try {
  BindPendingNames();
  pendingNames = {}; // a failed/unattributed recording cannot leak to the next
  const auto folders = FindFolders();
  if (folders.active.empty() || folders.archive.empty()) return;
  PendingNames note;
  note.saves = folders.active; note.archive = folders.archive;
  note.before = RecordedSlots(note.saves);
  note.names = {{p1, p2}, spectating}; note.fighters[0] = fighter1; note.fighters[1] = fighter2;
  note.started = static_cast<std::uint64_t>(_time64(nullptr));
  pendingNames = std::move(note);
 } catch (...) { pendingNames = {}; spdlog::warn("Replays: recording provenance could not be captured"); }
}
void NoteMatchEnd() {
 std::lock_guard<std::mutex> lock(namesMutex);
 try {
  if (pendingNames.saves.empty() || pendingNames.ended) return;
  pendingNames.ended = static_cast<std::uint64_t>(_time64(nullptr));
  BindPendingNames();
 } catch (...) { spdlog::warn("Replays: recording provenance could not be bound"); }
}

void MarkWatched(const fs::path& file) {
	try {
		const Folders folders = FindFolders();
		if (folders.archive.empty()) return;
		std::ofstream out(folders.archive / L"watched.txt", std::ios::app);
		out << WideToUtf8(file.filename().wstring()) << '\n';
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: the watched list was not written: {}", e.what());
	}
}

namespace {
std::vector<std::string> WatchedNames(const fs::path& file) {
	std::vector<std::string> names;
	auto bytes = replayfiles::ReadFile(file, 1 << 20);
 if (!bytes) return names;
 std::string line;
 for (auto byte : *bytes) {
  if (byte == '\n') { if (!line.empty() && line.size() <= 4096) names.push_back(line); line.clear(); }
  else if (line.size() <= 4096) line.push_back(static_cast<char>(byte));
 }
 if (!line.empty() && line.size() <= 4096) names.push_back(line);
	return names;
}

// Watched markers are read again when their bounded file changes.
struct NotesCache {
	fs::file_time_type watchedWritten{};
	std::vector<std::string> watched;
};

std::vector<ArchivedReplay> List(NotesCache& cache) {
	const Folders folders = FindFolders();
	if (folders.archive.empty()) return {};
	std::error_code ignored;
 { std::lock_guard<std::mutex> lock(namesMutex); BindPendingNames(); }
 const fs::path watchedFile = folders.archive / L"watched.txt";
	const auto watchedWritten = fs::last_write_time(watchedFile, ignored);
	if (ignored) cache.watched.clear(); else if (watchedWritten != cache.watchedWritten) cache.watched = WatchedNames(watchedFile);
	cache.watchedWritten = watchedWritten;

	std::vector<ArchivedReplay> archived = Index().Refresh(folders.archive);
	for (ArchivedReplay& replay : archived) {
		replay.watched = std::find(cache.watched.begin(), cache.watched.end(), WideToUtf8(replay.path.filename().wstring())) != cache.watched.end();
		fs::path video = replay.path;
		std::error_code failed;
		replay.video = fs::exists(video.replace_extension(L".mp4"), failed);
	}
	std::sort(archived.begin(), archived.end(), [](const ArchivedReplay& a, const ArchivedReplay& b) { return a.time > b.time; });
	return archived;
}

// The one lister of this process. Its thread is started by the first Want
// and ended by StopListing, which waits for it. Requests and completion
// ownership are kept together under its lock.
struct Lister {
	std::mutex mutex;
	std::condition_variable wake;
	bool wanted = false, stop = false;
	replayinputs::DetailRequests details;
	std::thread thread;
	std::shared_ptr<const std::vector<ArchivedReplay>> latest;
};
Lister& TheLister() { static Lister* const lister = new Lister; return *lister; }

void ListUntilStopped(Lister& lister) {
	NotesCache cache;
 replayinputs::DetailCache details;
	// Two seconds between listings, however often one is asked for. One
	// replay's detail does not wait for that.
	auto nextListing = std::chrono::steady_clock::now();
	std::unique_lock<std::mutex> lock(lister.mutex);
	while (!lister.stop) {
		if (lister.details.Pending()) {
   auto request = lister.details.Take();
			lock.unlock();
   const auto detail = details.Read(request.file, request.revision);
   lock.lock();
   lister.details.Complete(detail);
		}
		else if (lister.wanted && std::chrono::steady_clock::now() >= nextListing) {
			lister.wanted = false;
			lock.unlock();
			std::shared_ptr<const std::vector<ArchivedReplay>> listed;
			// Whatever a file or a folder throws ends this listing, not the game.
			try { listed = std::make_shared<const std::vector<ArchivedReplay>>(List(cache)); }
			catch (const std::exception& e) { spdlog::warn("Replays: the archive was not listed: {}", e.what()); }
			catch (...) { spdlog::warn("Replays: the archive was not listed"); }
			lock.lock();
			if (listed) lister.latest = std::move(listed);
			nextListing = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		}
		else if (lister.wanted) lister.wake.wait_until(lock, nextListing, [&] { return lister.stop || lister.details.Pending(); });
		else lister.wake.wait(lock, [&] { return lister.wanted || lister.stop || lister.details.Pending(); });
	}
}

// Under the lister's lock: wakes its thread, starting it the first time.
void Wake(Lister& lister) {
	lister.wake.notify_all();
	if (!lister.thread.joinable()) lister.thread = std::thread([&lister] { ListUntilStopped(lister); });
}
}

void WantListing() {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
	if (lister.stop) return;
	lister.wanted = true;
	Wake(lister);
}

void WantDetail(const std::string& file, std::uint64_t revision) {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
 if (lister.stop || !lister.details.Want(file, revision)) return;
 try { Wake(lister); }
 catch (...) { lister.details.Fail(revision); }
}

replayinputs::DetailCompletion LatestDetail() {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
	return lister.details.Latest();
}

std::shared_ptr<const std::vector<ArchivedReplay>> LatestListing() {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
	return lister.latest;
}

void StopListing() {
	Lister& lister = TheLister();
	std::thread thread;
	{
		std::lock_guard<std::mutex> lock(lister.mutex);
		lister.stop = true;
		lister.wake.notify_all();
		thread = std::move(lister.thread);
	}
	// A listing in progress is waited for: it holds the archive's files open.
	if (thread.joinable()) thread.join();
}

} } }
