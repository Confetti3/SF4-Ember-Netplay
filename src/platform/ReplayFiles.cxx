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
#include "DurableFile.hxx"
#include "ReplayRecordingChanges.hxx"
#include "ReplayImportFreshness.hxx"
#include "ReplayRecordingWorker.hxx"
#include "ReplayImportWorker.hxx"
#include "ReplayArchiveIndex.hxx"
#include "ReplayArchivePass.hxx"
#include "ReplayArchiveWorker.hxx"
#include "../common/ReplayFileSafety.hxx"
#include "../common/ReplayInputDetails.hxx"
#include "../common/ReplaySummaryCache.hxx"

namespace fs = std::filesystem;
namespace slots = sf4e::replayslots;

namespace sf4e { namespace platform { namespace replays {
namespace {

// The largest index read: the game writes LIST at 76 KB and replays-swan.dat
// at 44 KB.
constexpr std::size_t kMostIndexBytes = 1 << 20;

// Every file Ember publishes here is create-only: a destination that appears
// during the write wins, and neither unreadable nor confirmed damaged files
// are replaced.
bool PublishFile(const fs::path& path, const slots::Bytes& contents) {
 return static_cast<bool>(durable::PublishCreateOnly(path, durable::PartialPath(path), contents.data(), contents.size()));
}

// All ingestion uses replayfiles::ReadFile and its bounded read-error policy.
slots::Bytes LoadFile(const fs::path& path, std::size_t most = replayfiles::kMostArchiveBytes) {
 auto bytes = replayfiles::ReadFile(path, most);
 return bytes ? std::move(*bytes) : slots::Bytes{};
}

void ReadNames(const fs::path& archive, const slots::Bytes& body, ArchivedReplay& replay) {
 const auto names = replayfiles::ReadBodyNames(archive, body);
 replay.names[0] = names.players[0]; replay.names[1] = names.players[1]; replay.spectated = names.spectated;
}
std::atomic<replayfiles::RecordingWorker*> provenanceWorker{nullptr};
std::atomic<replayfiles::ArchiveWorker*> archiveWorker{nullptr};
std::atomic<RecordingChanges*> recordingChanges{nullptr};
// A save of the game's wakes both the provenance and the archive workers.
RecordingChanges& Changes() {
 static auto* const changes = [] {
  auto* started = new RecordingChanges([] {
   if (auto* worker = provenanceWorker.load()) worker->FilesChanged();
   if (auto* archiver = archiveWorker.load()) archiver->Want();
  });
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

// The archive index contains only CRC-indexed paths, and whether the last
// refresh listed the archive whole, published together. Parsing and exact-byte cache reuse belong
// to the serialized worker refresh, bounded to ArchiveSummaryCache::kCapacity
// files.
class ArchiveIndex {
public:
 std::vector<ArchivedReplay> Refresh(const fs::path& archive) {
  std::vector<ArchivedReplay> replays;
  candidates_.Refresh([&] {
   summaries_.NextListing();
   return replayfiles::ScanArchive(archive, [&](const fs::path& path, const replayfiles::ArchiveFile& read) {
    ArchivedReplay replay{path};
    replay.label = replayfiles::ReplayDateLabel(read.time); replay.time = read.time;
    replay.fighters[0] = read.fighters[0]; replay.fighters[1] = read.fighters[1];
    replay.summary = summaries_.Read(read.contents);
    ReadNames(archive, read.Body(), replay);
    replays.push_back(std::move(replay));
   });
  });
  return replays;
 }
 replayfiles::ArchiveCandidates& Held() { return candidates_; }
 bool Complete() const { return candidates_.Current()->complete; }
private:
 replayfiles::ArchiveCandidates candidates_;
 replayfiles::ArchiveSummaryCache summaries_;
};
ArchiveIndex& Index() { static ArchiveIndex index; return index; }

// Where this process copies the game's replays to (ReplayArchivePass.hxx).
// ArchiveSaves and BackUp are the only copiers, under one lock, so a pass and
// an import's backup of the slot it replaces never decide on a slot together.
replayfiles::SlotFileCache& SlotFiles() { static replayfiles::SlotFileCache cache; return cache; }
// Sidecar's passes settle at most this many outbox entries, so a backlog
// never holds up the next reading of the slots; the next pass goes on from
// where it stopped. The launcher's settle them all.
constexpr std::size_t kSettleBudget = 32;
replayfiles::ArchiveTarget Target(const Folders& folders, std::size_t settleBudget, replayfiles::ArchiveView& view) {
	static std::size_t settleFrom = 0;
	return {folders.archive, folders.outbox, settleBudget, settleFrom, view, SlotFiles(), PublishFile};
}
std::mutex& CopyLock() { static std::mutex lock; return lock; }

// More replays than this waiting in the outbox, about 10 MB, is worth a
// warning: the archive has not been writable for a long while.
constexpr int kOutboxWarning = 1000;

// A pass over the outbox and these save folders, none at all being fine.
// Reads the archive for the index. One not listed whole, or not at all, still
// lets a pass put the slots' replays into the outbox; only settling waits for
// an index listed whole.
void RefreshIndex(const fs::path& archive) {
	try { Index().Refresh(archive); }
	catch (const std::exception& e) { spdlog::warn("Replays: the archive could not be read: {}", e.what()); }
	if (!Index().Complete()) spdlog::warn(L"Replays: the archive {} could not be listed whole; replays wait in the outbox", archive.c_str());
}

replayfiles::ArchivePass ArchiveSaves(const Folders& folders, const std::vector<fs::path>& accounts, std::size_t settleBudget) {
	std::lock_guard<std::mutex> lock(CopyLock());
	replayfiles::ArchiveView view(Index().Held());
	const auto pass = replayfiles::RunPass(Target(folders, settleBudget, view), accounts, kFirstMatchSlot, kLastMatchSlot);
	if (pass.copied) spdlog::info(L"Replays: archived {} to {}", pass.copied, folders.archive.c_str());
	if (pass.waiting > kOutboxWarning) spdlog::warn(L"Replays: {} replays wait in {}; the archive {} has not taken them", pass.waiting, folders.outbox.c_str(), folders.archive.c_str());
	else if (pass.waiting) spdlog::info(L"Replays: {} replays wait in {} until the archive takes them", pass.waiting, folders.outbox.c_str());
	if (pass.failed) spdlog::warn("Replays: {} replays could not be archived or kept in the outbox yet", pass.failed);
	return pass;
}

bool BackUp(const Folders& folders, const fs::path& saves, int slot, const slots::Bytes& body, replayfiles::BackupEvidence& evidence) {
	std::lock_guard<std::mutex> lock(CopyLock());
	replayfiles::ArchiveView view(Index().Held());
	return replayfiles::EnsureBackup(Target(folders, kSettleBudget, view), saves, slot, body, evidence);
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
	PWSTR appData = NULL, localAppData = NULL;
	// Where replays go does not depend on Steam, so an outbox is drained even
	// when no game's saves can be found.
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appData) == S_OK) folders.archive = fs::path(appData) / L"sf4e" / L"replays";
	CoTaskMemFree(appData);
	if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData) == S_OK) folders.outbox = fs::path(localAppData) / L"sf4e" / L"replay-outbox";
	CoTaskMemFree(localAppData);
	if (folders.archive.empty() ||
		RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steamPath, &steamPathBytes) != ERROR_SUCCESS) return folders;
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
		RefreshIndex(folders.archive);
		return ArchiveSaves(folders, folders.saves, replayfiles::kSettleAll).copied;
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
  // Each file the import may replace is snapshotted once and planned on as read.
  // Commit proceeds only while every one still holds exactly these bytes.
  replayfiles::ImportFiles files;
  const auto snapshot = [&](replayfiles::Change& change, const std::string& name) {
   change.name = kSavePrefix + name; change.path = saves / name;
   return replayfiles::Snapshot(change.path, kMostIndexBytes, change);
  };
  const bool indexesRead = snapshot(files.list, "LIST") && snapshot(files.listSidecar, "LIST.0") &&
   snapshot(files.swan, "replays-swan.dat") && snapshot(files.swanSidecar, "replays-swan.dat.0");
  const slots::Bytes &list = files.list.before, &swan = files.swan.before;
		// An index that is not the one its ".0" names is half written, or
		// damaged: it is not built on and not written out again.
		if (!indexesRead || !slots::ValidList(list) || !slots::ValidSwan(swan) ||
			files.listSidecar.before != slots::Sidecar(list) || files.swanSidecar.before != slots::Sidecar(swan)) {
			spdlog::warn("Replays: the game's replay index does not match its checksum file; nothing is imported over it");
			return ImportResult::IndexDamaged;
		}
		// The game writes a match's replay file as the match ends and its
		// index at a later save. Until then the index still has that slot as
		// its oldest, and choosing by it would put the import over the match
		// just played.
		for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; slot++) {
			const fs::path slotFile = saves / std::to_wstring(slot);
			const replayfiles::SlotFile now = replayfiles::ReadSlotFile(SlotFiles(), slotFile);
			if (!slots::FileAheadOfRecord(list, swan, slot, now.replay, now.sidecar)) continue;
			// An index saved since the file was written is not behind it: the
			// file is one the game no longer lists, and waits for nothing.
			std::error_code failed;
			const auto indexWritten = fs::last_write_time(saves / (slot < slots::kListSlots ? L"LIST" : L"replays-swan.dat"), failed);
			const auto fileWritten = failed ? fs::file_time_type{} : fs::last_write_time(slotFile, failed);
			if (!failed && fileWritten <= indexWritten) continue;
			spdlog::info("Replays: slot {} holds a replay the game has not put in its index yet; the import waits for that", slot);
			return ImportResult::IndexBehind;
		}
		const int slot = slots::SlotToReplace(list, swan, kFirstMatchSlot, kLastMatchSlot);
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
		const replayfiles::SlotFile held = replayfiles::ReadSlotFile(SlotFiles(), saves / std::to_wstring(slot));

  slots::ReplayHeaderInfo header;
  if (slots::ReadReplayHeader(held.replay, header) && !BackUp(folders, saves, slot, held.replay, transaction.backup)) {
   spdlog::warn("Replays: slot {} could not be backed up and verified; it is not replaced", slot);
   return ImportResult::ArchiveFailed;
  }
  // The slot's files exactly as they are, which the replay held in the slot
  // cache must match: a game save since then requires a fresh plan.
  const std::string slotName = std::to_string(slot);
  if (!snapshot(files.replay, slotName) || !snapshot(files.replaySidecar, slotName + ".0")) return ImportResult::RejectedBeforeWrite;
  if (files.replay.before != held.replay || files.replaySidecar.before != held.sidecar) return ImportResult::IndexBehind;
  // Execute only this plan, with exact-file snapshots and exception-safe
  // rollback. Empty and missing files stay distinct.
  auto& changes = transaction.changes;
		if (!replayfiles::PlanImport(exported, slot, static_cast<std::uint32_t>(_time64(nullptr)), files, changes)) {
			spdlog::warn(L"Replays: {} is not a replay Ember can import", file.c_str());
			return ImportResult::NotAReplay;
		}
		// Prepare the live-table record before writing, so an allocation failure
		// cannot leave the files ahead of the table. A slot of swan leaves LIST as it was.
		const slots::Bytes& swanAfter = files.swan.after;
		const std::uint8_t* record = slots::Record(slot < slots::kListSlots ? files.list.after : list, swanAfter, slot);
		if (!record) return ImportResult::IndexDamaged;
		Imported imported;
		imported.slot = slot;
		imported.record.assign(record, record + slots::kRecordBytes);
		imported.slotBytes.assign(swanAfter.begin() + slots::kSwanSlotBytesOffset + slot * 2, swanAfter.begin() + slots::kSwanSlotBytesOffset + slot * 2 + 2);
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

namespace {
// Sidecar's pass: the outbox and the signed-in account's slots, as the game
// saves them. The archive is read once for what it holds, then each copy is
// added to that.
replayfiles::ArchivePass ArchiveActiveAccount() {
 // Only the worker's thread runs this: what it has done once.
 static bool unwatched = false;
 const Folders folders = FindFolders();
 if (folders.archive.empty()) return {};
 if (!folders.active.empty() && !Changes().Watch(folders.active) && !unwatched) {
  unwatched = true;
  spdlog::warn("Replays: the save folder cannot be watched; replays are archived at each match start and for two minutes after each match end");
 }
 if (!Index().Complete()) RefreshIndex(folders.archive);
 // The outbox is drained without a signed-in account too.
 return ArchiveSaves(folders, folders.active.empty() ? std::vector<fs::path>{} : std::vector<fs::path>{folders.active}, kSettleBudget);
}
// Passes a second apart, then two, four and so on up to thirty: for two
// minutes after a match ends, which covers the game's save after the battle
// closes, and eight times after a retry. Every
// thirty seconds while the outbox holds replays.
replayfiles::ArchiveWorker& Archiver() {
 static auto* const worker = [] {
  auto* started = new replayfiles::ArchiveWorker([] {
   try { return ArchiveActiveAccount(); }
   catch (const std::exception& e) {
    spdlog::warn("Replays: archiving stopped: {}", e.what());
    replayfiles::ArchivePass failed;
    failed.retry = true;
    return failed;
   }
  }, {std::chrono::seconds(1), std::chrono::seconds(30), 8, std::chrono::minutes(2), std::chrono::seconds(30)});
  archiveWorker.store(started); return started;
 }();
 return *worker;
}
}

void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating, int fighter1, int fighter2, bool recordingEnabled) {
 try { Archiver().Want(); } catch (...) {}
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
 try { Archiver().MatchEnded(); } catch (...) {}
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
	// One request and completion per requester (DetailFor).
	replayinputs::DetailRequests details[2];
	replayinputs::DetailRequests* PendingDetail() { for (auto& requests : details) if (requests.Pending()) return &requests; return nullptr; }
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
		if (auto* requests = lister.PendingDetail()) {
   auto request = requests->Take();
			lock.unlock();
   replayinputs::DetailCompletion detail{request.revision, replayinputs::DetailState::Failed, {}};
   try { detail = details.Read(request.file, request.revision, FindFolders().archive); } catch (...) {}
   lock.lock();
   requests->Complete(detail);
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
		else if (lister.wanted) lister.wake.wait_until(lock, nextListing, [&] { return lister.stop || lister.PendingDetail(); });
		else lister.wake.wait(lock, [&] { return lister.wanted || lister.stop || lister.PendingDetail(); });
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

void WantDetail(const std::string& file, std::uint64_t revision, DetailFor requester) {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
 auto& details = lister.details[requester == DetailFor::Playback ? 1 : 0];
 if (lister.stop || !details.Want(file, revision)) return;
 try { Wake(lister); }
 catch (...) { details.Fail(revision); }
}

replayinputs::DetailCompletion LatestDetail(DetailFor requester) {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
	return lister.details[requester == DetailFor::Playback ? 1 : 0].Latest();
}

std::shared_ptr<const std::vector<ArchivedReplay>> LatestListing() {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
	return lister.latest;
}

void StopListing() {
 if (auto* worker = archiveWorker.load()) worker->Stop();
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
