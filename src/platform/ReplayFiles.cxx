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
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Utf8.hxx"

namespace fs = std::filesystem;
namespace slots = sf4e::replayslots;

namespace sf4e { namespace platform { namespace replays {
namespace {

// The largest files read: an exported replay, and the game's two indexes
// (76 KB and 44 KB as the game writes them).
constexpr std::size_t kMostReplayBytes = slots::kLargestReplay + slots::kExportHeaderBytes, kMostIndexBytes = 1 << 20;

// The file's contents, or nothing when it is longer than most: a file may
// come from anyone, and no more than that is ever read of it.
slots::Bytes LoadFile(const fs::path& path, std::size_t most = kMostReplayBytes) {
	std::ifstream file(path, std::ios::binary);
	slots::Bytes contents(most + 1);
	file.read(reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
	contents.resize(static_cast<std::size_t>(file.gcount()));
	if (contents.size() > most) contents.clear();
	return contents;
}

// Written under another name first, so a cut-off write is never taken for a
// finished file. The name is this process's and this write's own: the
// launcher and the game may both be archiving the same replay.
bool SaveFile(const fs::path& path, const slots::Bytes& contents) {
	static std::atomic<unsigned> serial{0};
	const fs::path partial = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial) + L".tmp";
	std::ofstream out(partial, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(contents.data()), contents.size());
	out.close();
	std::error_code error;
	if (out.fail()) { fs::remove(partial, error); return false; }
	fs::rename(partial, path, error);
	if (error) { std::error_code ignored; fs::remove(partial, ignored); }
	return !error;
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
bool ReadArchived(const fs::path& path, bool inRoot, ArchivedReplay& replay, std::uint32_t& crc) {
	const slots::Bytes contents = LoadFile(path);
	std::uint64_t time = 0;
	replay = ArchivedReplay{path};
	if (path.extension() == L".usf4replay") {
		slots::ReplayHeaderInfo info;
		if (!slots::ReadReplayHeader(contents, info) || contents.size() > slots::kLargestReplay) return false;
		replay.fighters[0] = info.fighters[0]; replay.fighters[1] = info.fighters[1];
		time = info.time;
		crc = slots::Crc32(contents.data(), contents.size());
	}
	else {
		if (!inRoot || !slots::ParseArchiveName(path.filename().wstring(), time, crc) || !slots::WholeArchived(contents, crc)) return false;
		const slots::RecordInfo info = slots::ReadRecordInfo(contents.data() + 8);
		replay.fighters[0] = info.fighters[0]; replay.fighters[1] = info.fighters[1];
	}
	const __time64_t at = static_cast<__time64_t>(time);
	tm local = {};
	char label[32] = { 0 };
	if (_localtime64_s(&local, &at) || !std::strftime(label, sizeof(label), "%Y-%m-%d %H:%M", &local)) return false;
	replay.label = label; replay.time = time;
	return true;
}

// The archive as this process last read it: every replay file by its path,
// with its size and time, so a file is opened once and again only when it
// changed. The listing, the archiver and the import's check all read this one
// index. Refresh reads the folder on the caller's thread; Holds and Add only
// look at what is kept.
class ArchiveIndex {
public:
	struct Entry { std::uintmax_t size; fs::file_time_type written; bool replay; std::uint32_t crc; ArchivedReplay read; };

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
			// A folder or a link by a replay's name is not one.
			if (!at->is_regular_file(failed) || failed) continue;
			const std::uintmax_t size = at->file_size(failed);
			const fs::file_time_type written = failed ? fs::file_time_type{} : at->last_write_time(failed);
			if (failed) continue;
			const auto before = kept.find(path.wstring());
			Entry entry{size, written, false, 0, {}};
			if (before != kept.end() && before->second.size == size && before->second.written == written) entry = before->second;
			else entry.replay = ReadArchived(path, path.parent_path() == archive, entry.read, entry.crc);
			if (entry.replay) replays.push_back(entry.read);
			seen.emplace(path.wstring(), std::move(entry));
		}
		std::lock_guard<std::mutex> lock(mutex_);
		// A file archived while the folder was being read stays known.
		for (auto& added : added_) seen.emplace(added.first, std::move(added.second));
		added_.clear();
		files_ = std::move(seen);
		return replays;
	}
	bool Holds(std::uint32_t crc) const {
		std::lock_guard<std::mutex> lock(mutex_);
		const auto holds = [&](const std::map<std::wstring, Entry>& files) {
			return std::any_of(files.begin(), files.end(), [&](const auto& file) { return file.second.replay && file.second.crc == crc; });
		};
		return holds(files_) || holds(added_);
	}
	// A file this process just wrote into the archive.
	void Add(const fs::path& path) {
		Entry entry{0, {}, false, 0, {}};
		std::error_code ignored;
		entry.size = fs::file_size(path, ignored);
		entry.written = fs::last_write_time(path, ignored);
		entry.replay = ReadArchived(path, true, entry.read, entry.crc);
		std::lock_guard<std::mutex> lock(mutex_);
		added_[path.wstring()] = std::move(entry);
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
	if (!slots::ReadReplayHeader(now.replay, header) || Index().Holds(slots::Crc32(now.replay.data(), now.replay.size()))) return Archived::Nothing;
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
	if (!SaveFile(archive / name, exported)) { spdlog::warn("Replays: could not write the copy of slot {}", slot); return Archived::Failed; }
	Index().Add(archive / name);
	return Archived::Copied;
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

ImportResult ImportFile(const fs::path& file, const Writer& write, Imported& out) {
	try {
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
		slots::Bytes exported = LoadFile(file);
		// usf4-replay-saver keeps the game's replay as it is and the slot
		// record beside it, in .index/<crc>.entry.
		if (exported.size() >= 4 && !std::memcmp(exported.data(), "#BRP", 4)) {
			wchar_t crc[16] = { 0 };
			StringCchPrintfW(crc, 16, L"%08x", slots::Crc32(exported.data(), exported.size()));
			const slots::Bytes entry = LoadFile(file.parent_path() / L".index" / (std::wstring(crc) + L".entry"), slots::kRecordBytes);
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
		if (slots::ReadReplayHeader(held.replay, header) && !Index().Holds(slots::Crc32(held.replay.data(), held.replay.size())) &&
			ArchiveSlot(folders.archive, saves, slot, before.list, before.swan) != Archived::Copied) {
			spdlog::warn("Replays: slot {} holds a replay that could not be archived, so it is not replaced", slot);
			return ImportResult::NotArchived;
		}
		slots::WritePlan plan;
		if (!slots::PlanImport(exported, slot, static_cast<std::uint32_t>(_time64(nullptr)), before, plan)) {
			spdlog::warn(L"Replays: {} is not a replay Ember can import", file.c_str());
			return ImportResult::NotAReplay;
		}
		bool undone = true;
		if (!slots::RunPlan(plan, [&](const std::string& name, const slots::Bytes& contents) { return write(kSavePrefix + name, contents); }, undone)) {
			spdlog::error("Replays: could not write slot {} and its index; the files are {}", slot, undone ? "as they were" : "not all back as they were, and the slot's replay is in the archive");
			return ImportResult::WriteFailed;
		}
		// The record and the slot's bytes as written: the plan's index files hold them.
		const slots::Bytes& swan = plan[2].now;
		const std::uint8_t* record = slots::Record(slot < slots::kListSlots ? plan[4].now : before.list, swan, slot);
		out.slot = slot;
		out.record.assign(record, record + slots::kRecordBytes);
		out.slotBytes.assign(swan.begin() + slots::kSwanSlotBytesOffset + slot * 2, swan.begin() + slots::kSwanSlotBytesOffset + slot * 2 + 2);
		out.plan = std::move(plan);
		spdlog::info(L"Replays: imported {} into slot {}", file.c_str(), slot);
		return ImportResult::Done;
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: import stopped: {}", e.what());
		return ImportResult::WriteFailed;
	}
}

bool UndoImport(const Imported& imported, const Writer& write) {
	try {
		const bool undone = slots::UndoPlan(imported.plan, imported.plan.size(),
			[&](const std::string& name, const slots::Bytes& contents) { return write(kSavePrefix + name, contents); });
		if (undone) spdlog::info("Replays: slot {}'s files are back as they were", imported.slot);
		else spdlog::error("Replays: slot {}'s files could not all be put back; its own replay is in the archive", imported.slot);
		return undone;
	}
	catch (const std::exception& e) {
		spdlog::error("Replays: putting slot {} back stopped: {}", imported.slot, e.what());
		return false;
	}
}

void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating, int fighter1, int fighter2) {
	try {
		const Folders folders = FindFolders();
		if (folders.archive.empty()) return;
		std::error_code ignored;
		fs::create_directories(folders.archive, ignored);
		std::ofstream out(folders.archive / L"matches.jsonl", std::ios::app);
		out << nlohmann::json{{"started", static_cast<std::uint64_t>(_time64(nullptr))}, {"p1", p1}, {"p2", p2}, {"spectated", spectating},
			{"f1", fighter1}, {"f2", fighter2}}.dump() << '\n';
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: the match's names were not noted: {}", e.what());
	}
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
	std::ifstream in(file);
	for (std::string line; std::getline(in, line);) if (!line.empty()) names.push_back(line);
	return names;
}

// A line that is not a match as NoteMatchStart writes one is passed over:
// the file is the player's to edit, or to damage. A note from before the
// fighters were written has none.
std::vector<slots::NotedMatch> NotedMatches(const fs::path& file) {
	std::vector<slots::NotedMatch> matches;
	std::ifstream in(file);
	for (std::string line; std::getline(in, line);) {
		const auto json = nlohmann::json::parse(line, nullptr, false);
		if (!json.is_object()) continue;
		const auto started = json.find("started"), spectated = json.find("spectated");
		if (started == json.end() || !started->is_number_unsigned()) continue;
		const auto name = [&](const char* key) { const auto at = json.find(key); return at != json.end() && at->is_string() ? at->get<std::string>() : std::string(); };
		const auto fighter = [&](const char* key) {
			const auto at = json.find(key);
			return at != json.end() && at->is_number_integer() && at->get<std::int64_t>() >= 0 && at->get<std::int64_t>() < 64 ? static_cast<int>(at->get<std::int64_t>()) : -1;
		};
		matches.push_back({started->get<std::uint64_t>(), {name("p1"), name("p2")}, {fighter("f1"), fighter("f2")}, spectated != json.end() && spectated->is_boolean() && spectated->get<bool>()});
	}
	return matches;
}

// The two lists kept beside the archive's replays, read again only when
// their files changed.
struct NotesCache {
	fs::file_time_type notedWritten{}, watchedWritten{};
	std::vector<slots::NotedMatch> noted;
	std::vector<std::string> watched;
};

std::vector<ArchivedReplay> List(NotesCache& cache) {
	const Folders folders = FindFolders();
	if (folders.archive.empty()) return {};
	std::error_code ignored;
	const fs::path notedFile = folders.archive / L"matches.jsonl", watchedFile = folders.archive / L"watched.txt";
	const auto notedWritten = fs::last_write_time(notedFile, ignored);
	if (ignored) cache.noted.clear(); else if (notedWritten != cache.notedWritten) cache.noted = NotedMatches(notedFile);
	cache.notedWritten = notedWritten;
	const auto watchedWritten = fs::last_write_time(watchedFile, ignored);
	if (ignored) cache.watched.clear(); else if (watchedWritten != cache.watchedWritten) cache.watched = WatchedNames(watchedFile);
	cache.watchedWritten = watchedWritten;

	std::vector<ArchivedReplay> archived = Index().Refresh(folders.archive);
	for (ArchivedReplay& replay : archived) {
		if (const slots::NotedMatch* match = slots::MatchOf(cache.noted, replay.time, replay.fighters)) {
			replay.names[0] = match->names[0]; replay.names[1] = match->names[1]; replay.spectated = match->spectated;
		}
		replay.watched = std::find(cache.watched.begin(), cache.watched.end(), WideToUtf8(replay.path.filename().wstring())) != cache.watched.end();
		std::error_code failed;
		replay.video = fs::exists(VideoOf(replay.path), failed);
	}
	std::sort(archived.begin(), archived.end(), [](const ArchivedReplay& a, const ArchivedReplay& b) { return a.time > b.time; });
	return archived;
}

// The one lister of this process. Its thread is started by the first Want
// and ended by StopListing, which waits for it.
struct Lister {
	std::mutex mutex;
	std::condition_variable wake;
	bool wanted = false, stop = false;
	std::thread thread;
	std::shared_ptr<const std::vector<ArchivedReplay>> latest;
};
Lister& TheLister() { static Lister* const lister = new Lister; return *lister; }

void ListUntilStopped(Lister& lister) {
	NotesCache cache;
	std::unique_lock<std::mutex> lock(lister.mutex);
	while (!lister.stop) {
		lister.wake.wait(lock, [&] { return lister.wanted || lister.stop; });
		if (lister.stop) break;
		lister.wanted = false;
		lock.unlock();
		std::shared_ptr<const std::vector<ArchivedReplay>> listed;
		// Whatever a file or a folder throws ends this listing, not the game.
		try { listed = std::make_shared<const std::vector<ArchivedReplay>>(List(cache)); }
		catch (const std::exception& e) { spdlog::warn("Replays: the archive was not listed: {}", e.what()); }
		catch (...) { spdlog::warn("Replays: the archive was not listed"); }
		lock.lock();
		if (listed) lister.latest = std::move(listed);
		// Two seconds between listings, however often one is asked for.
		lister.wake.wait_for(lock, std::chrono::seconds(2), [&] { return lister.stop; });
	}
}
}

void WantListing() {
	Lister& lister = TheLister();
	std::lock_guard<std::mutex> lock(lister.mutex);
	if (lister.stop) return;
	lister.wanted = true;
	lister.wake.notify_all();
	if (!lister.thread.joinable()) lister.thread = std::thread([&lister] { ListUntilStopped(lister); });
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
