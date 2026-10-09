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
#include "ReplayPublication.hxx"
#include "ReplayRecordingChanges.hxx"
#include "ReplayImportFreshness.hxx"
#include "../common/ReplayFileSafety.hxx"
#include "../common/ReplayInputDetails.hxx"
#include "../common/ReplayRecordingWorker.hxx"
#include "../common/ReplayPreparationWorker.hxx"
#include "../common/ReplayArchiveIndex.hxx"

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

void ReadNames(const fs::path& archive, const slots::Bytes& body, ArchivedReplay& replay) {
 const auto names = replayfiles::ReadBodyNames(archive, body);
 replay.names[0] = names.players[0]; replay.names[1] = names.players[1]; replay.spectated = names.spectated;
}
std::atomic<replayfiles::RecordingWorker*> provenanceWorker{nullptr};
std::atomic<RecordingChanges*> recordingChanges{nullptr};
RecordingChanges& Changes() {
 static auto* const changes = [] {
  auto* started = new RecordingChanges([] { if (auto* worker = provenanceWorker.load()) worker->FilesChanged(); });
  recordingChanges.store(started); return started;
 }();
 return *changes;
}
replayfiles::RecordingSnapshot RecordingSlots() {
 const auto folders = FindFolders();
 replayfiles::RecordingSnapshot snapshot;
 snapshot.saves = folders.active; snapshot.archive = folders.archive;
 if (snapshot.saves.empty() || snapshot.archive.empty()) return snapshot;
 if (!Changes().Watch(snapshot.saves)) throw std::runtime_error("recording saves could not be watched");
 for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; ++slot) {
  const auto path = snapshot.saves / std::to_wstring(slot);
  replayfiles::Change body, checksum;
  if (!replayfiles::Snapshot(path, slots::kLargestReplay, body) || !replayfiles::Snapshot(path.wstring() + L".0", 4, checksum))
   throw std::runtime_error("recording slots could not be read");
  snapshot.slots.push_back({std::move(body.before), std::move(checksum.before)});
 }
 return snapshot;
}
replayfiles::NamePublication PublishNames(const fs::path& archive, const slots::Bytes& body, const replayfiles::ReplayNames& names) {
 const auto path = replayfiles::NamesPath(archive, body);
 return replayfiles::PublishBodyNames(path, body, names, [&](const slots::Bytes& bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  return !error && PublishFile(path, bytes);
 });
}
replayfiles::RecordingWorker& Provenance() {
 // Explicit shutdown joins this owner outside the loader lock, as it does
 // the lister. Shutdown need not create a worker that was never used.
 static auto* const worker = [] {
  auto* started = new replayfiles::RecordingWorker(RecordingSlots, PublishNames, {}, [](const char* reason) { spdlog::warn("Replays: provenance gave up: {}", reason); });
  provenanceWorker.store(started); return started;
 }();
 return *worker;
}

// "20261005-213503-69991186.emberreplay": the save time (UTC) and the CRC,
// both from the export's record. ParseArchiveName reads it back.
bool ArchiveName(const slots::Bytes& exported, wchar_t (&name)[64]) {
	const __time64_t saved = slots::ReadU32(exported.data() + 8 + 13);
	tm utc = {};
	return !_gmtime64_s(&utc, &saved) && SUCCEEDED(StringCchPrintfW(name, 64, L"%04d%02d%02d-%02d%02d%02d-%08x.emberreplay",
		utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, slots::ReadU32(exported.data() + 8 + 5)));
}

// The archive index contains only CRC-indexed paths. Parsing and exact-byte
// cache reuse belong to the serialized worker refresh, bounded to 16 files.
class ArchiveIndex {
public:
 std::vector<ArchivedReplay> Refresh(const fs::path& archive) {
  std::vector<ArchivedReplay> replays;
  candidates_.Refresh([&] {
   std::map<std::uint32_t, replayfiles::ArchiveCandidates::Paths> paths;
   std::error_code error;
   for (fs::recursive_directory_iterator at(archive, fs::directory_options::skip_permission_denied, error), end;
    !error && at != end; at.increment(error)) {
    const fs::path path = at->path();
    if (path.extension() != L".emberreplay" && path.extension() != L".usf4replay") continue;
    replayfiles::ArchiveFile read;
    if (replayfiles::ReadArchive(path, path.parent_path() == archive, read) != replayfiles::ArchiveState::Valid) continue;
    ArchivedReplay replay{path};
    replay.label = replayfiles::ReplayDateLabel(read.time); replay.time = read.time;
    replay.fighters[0] = read.fighters[0]; replay.fighters[1] = read.fighters[1];
    replay.summary = summaries_.Read(read.contents);
    ReadNames(archive, read.Body(), replay);
    replays.push_back(std::move(replay));
    paths[read.crc].push_back(path);
   }
   replayfiles::ArchiveCandidates::Map seen;
   for (auto& item : paths) seen.emplace(item.first, std::make_shared<const replayfiles::ArchiveCandidates::Paths>(std::move(item.second)));
   return seen;
  });
  return replays;
 }
 bool Verify(const fs::path& archive, const slots::Bytes& body, replayfiles::BackupEvidence& evidence) const {
  return replayfiles::VerifyBackup(candidates_.Candidates(slots::Crc32(body.data(), body.size())), archive, body, evidence);
 }
 bool Add(const fs::path& path) {
  replayfiles::ArchiveFile read;
  if (replayfiles::ReadArchive(path, true, read) != replayfiles::ArchiveState::Valid) return false;
  candidates_.Add(read.crc, path);
  return true;
 }
private:
 replayfiles::ArchiveCandidates candidates_;
 replayfiles::ArchiveSummaryCache summaries_;
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
 const fs::path target = archive / name;
 if (!replayfiles::PublishArchive(now.replay, exported,
  [&](replayfiles::ArchiveFile& existing) { return replayfiles::ReadArchive(target, true, existing); },
  [&](const slots::Bytes& bytes) { return PublishFile(target, bytes); })) {
  spdlog::warn("Replays: could not safely publish the copy of slot {}", slot); return Archived::Failed;
 }
	if (!Index().Add(archive / name)) { spdlog::warn("Replays: the copy of slot {} could not be verified", slot); return Archived::Failed; }
	return Archived::Copied;
}

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

namespace {
// Change notifications are armed before preparation, never rearmed. The game
// uses these only to reject invalidated snapshots; exact file checks in Apply
// establish freshness immediately before any Steam write.
struct ImportFreshness {
 SaveFolderFreshness saves;
 HANDLE account = nullptr;
 HKEY key = nullptr;
 ~ImportFreshness() { if (account) CloseHandle(account); if (key) RegCloseKey(key); }
 bool Arm(const fs::path& folder) {
  if (!saves.Arm(folder)) return false;
  account = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  return account &&
   RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", 0, KEY_NOTIFY, &key) == ERROR_SUCCESS &&
   RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, account, TRUE) == ERROR_SUCCESS;
 }
 bool NotInvalidated() const { return saves.NotInvalidated() && WaitForSingleObject(account, 0) == WAIT_TIMEOUT; }
};
PreparedImport PrepareImport(const fs::path& file) {
 PreparedImport transaction;
 transaction.source = file;
 const auto prepare = [&]() -> ImportResult {
  const auto resolved = ResolveReplayFile(WideToUtf8(file.wstring()));
  if (resolved.empty()) return ImportResult::NotAReplay;
  const fs::path source = fs::u8path(resolved);
		const Folders folders = FindFolders();
		// The files written through `write` are the running account's, so the
		// indexes are read from that account and no other.
		const fs::path& saves = folders.active;
		if (saves.empty() || folders.archive.empty()) { spdlog::warn("Replays: no save folder for the Steam account that is signed in"); return ImportResult::NoFolder; }
  auto guard = std::make_shared<ImportFreshness>();
  if (!guard->Arm(saves) || FindFolders().active != saves) return ImportResult::IndexBehind;
  transaction.notInvalidated = [guard] { return guard->NotInvalidated(); };
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
			change.path = saves / step.name;
			if (!replayfiles::Snapshot(change.path, kMostIndexBytes, change)) return ImportResult::RejectedBeforeWrite;
			// The plan must describe the files we are about to replace. A game
			// save during preparation requires a fresh plan, never stale indexes.
			if (change.before != step.before) return ImportResult::IndexBehind;
			changes.push_back(std::move(change));
		}
  transaction.recoveryBytes = RecoveryBytes(saves, changes);
  static std::atomic<unsigned> recoverySerial{0};
  transaction.recoveryFile = folders.archive / (L"recovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++recoverySerial) + L".ember-recovery");
  transaction.imported = std::move(imported);
  return ImportResult::Done;
 };
 try { transaction.result = prepare(); }
 catch (const std::exception& e) { spdlog::warn("Replays: import preparation stopped: {}", e.what()); }
 catch (...) {}
 return transaction;
}
using ImportWorker = replayfiles::PreparationWorker<PreparedImport>;
std::atomic<ImportWorker*> importWorker{nullptr};
ImportWorker& Imports() {
 static auto* const worker = [] {
  auto* started = new ImportWorker([](const fs::path& file) { return std::make_shared<const PreparedImport>(PrepareImport(file)); },
   [](const ImportTransaction& value) {
    bool persisted = false;
    try {
     std::error_code error;
     fs::create_directories(value->recoveryFile.parent_path(), error);
     persisted = !error && PublishFile(value->recoveryFile, value->recoveryBytes);
    } catch (...) {}
    spdlog::error(L"Replays: restoration incomplete; originals retained in this process; recovery file {} {}", value->recoveryFile.c_str(), persisted ? L"written" : L"could not be written");
   }, [](const ImportTransaction& value) { MarkWatched(value->source); });
  importWorker.store(started); return started;
 }();
 return *worker;
}
}
ImportResult WantImport(const fs::path& file) {
 try {
  auto& worker = Imports();
  if (worker.Failed()) return ImportResult::RecoveryIncomplete;
  return worker.Request(file) ? ImportResult::Done : ImportResult::RejectedBeforeWrite;
 } catch (...) { return ImportResult::RejectedBeforeWrite; }
}
bool TakeImport(ImportTransaction& out) { return Imports().Take(out); }
ImportResult CommitImport(const ImportTransaction& value, const Writer& write, const Remover& remove, const Publisher& publish, bool watched) {
 if (Imports().Failed()) return ImportResult::RecoveryIncomplete;
 if (!value) return ImportResult::RejectedBeforeWrite;
 if (value->result != ImportResult::Done) return value->result;
 const auto applied = replayfiles::Apply(value->changes, [&] { return value->Fresh(); }, write, remove, [&] { return publish(value->imported); });
 if (applied == replayfiles::ApplyOutcome::RejectedBeforeWrite) return ImportResult::IndexBehind;
 if (applied == replayfiles::ApplyOutcome::Done) { if (watched) Imports().CompleteLater(value); return ImportResult::Done; }
 if (applied == replayfiles::ApplyOutcome::FailedRestored) {
  spdlog::error("Replays: import failed; every original file was restored");
  return ImportResult::FailedRestored;
 }
 Imports().RetainRecovery(value);
 return ImportResult::RecoveryIncomplete;
}

void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating, int fighter1, int fighter2, bool recordingEnabled) {
 try {
  if (p1.size() > 1024 || p2.size() > 1024) { Provenance().Invalidate(); return; }
  replayfiles::RecordingBoundary event;
  event.kind = replayfiles::RecordingBoundary::Kind::Start;
  event.time = static_cast<std::uint64_t>(_time64(nullptr));
  event.recordingEnabled = recordingEnabled;
  event.names = {{p1, p2}, spectating}; event.fighters[0] = fighter1; event.fighters[1] = fighter2;
  Provenance().Post(std::move(event));
 } catch (...) { try { Provenance().Invalidate(); } catch (...) {} }
}
void NoteMatchEnd() {
 try {
  replayfiles::RecordingBoundary event;
  event.kind = replayfiles::RecordingBoundary::Kind::End;
  event.time = static_cast<std::uint64_t>(_time64(nullptr));
  Provenance().Post(std::move(event));
 } catch (...) { try { Provenance().Invalidate(); } catch (...) {} }
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
   replayinputs::DetailCompletion detail{request.revision, replayinputs::DetailState::Failed, {}};
   try { detail = details.Read(request.file, request.revision, FindFolders().archive); } catch (...) {}
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
			if (listed) {
    auto retired = std::move(lister.latest); lister.latest = std::move(listed);
    lock.unlock(); retired.reset(); lock.lock();
   }
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
 if (auto* worker = provenanceWorker.load()) worker->Stop();
 if (auto* changes = recordingChanges.load()) changes->Stop();
 if (auto* worker = importWorker.load()) worker->Stop();
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
