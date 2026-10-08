#include "ReplayFiles.hxx"

#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <time.h>

#include <algorithm>
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
#include "../common/ReplayFileSafety.hxx"

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

// The replays the archive holds, verified from each file's contents.
std::set<std::uint32_t> HeldCrcs(const fs::path& archive) {
	std::set<std::uint32_t> held;
	std::error_code ignored;
	for (fs::recursive_directory_iterator at(archive, fs::directory_options::skip_permission_denied, ignored), end; !ignored && at != end; at.increment(ignored)) {
		std::uint32_t crc = 0;
		if (replayfiles::ArchivedCrc(at->path(), crc)) held.insert(crc);
	}
	return held;
}

// Written under another name first, so a cut-off write is never taken for a
// finished file.
bool SaveFile(const fs::path& path, const slots::Bytes& contents) {
	const fs::path partial = path.wstring() + L".tmp";
	std::ofstream out(partial, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(contents.data()), contents.size());
	out.close();
	if (out.fail()) return false;
	std::error_code error;
	fs::rename(partial, path, error);
	return !error;
}

// "20261005-213503-69991186.emberreplay": the save time (UTC) and the CRC,
// both from the export's record.
bool ArchiveName(const slots::Bytes& exported, wchar_t (&name)[64]) {
	const __time64_t saved = slots::ReadU32(exported.data() + 8 + 13);
	tm utc = {};
	return !_gmtime64_s(&utc, &saved) && SUCCEEDED(StringCchPrintfW(name, 64, L"%04d%02d%02d-%02d%02d%02d-%08x.emberreplay",
		utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, slots::ReadU32(exported.data() + 8 + 5)));
}

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
		std::error_code ignored;
		std::set<std::uint32_t> held = HeldCrcs(folders.archive);
		int copied = 0;
		for (const fs::path& saves : folders.saves) {
			const slots::Bytes list = LoadFile(saves / L"LIST", kMostIndexBytes), swan = LoadFile(saves / L"replays-swan.dat", kMostIndexBytes);
			if (!slots::ValidSwan(swan)) continue;
			for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; slot++) {
				// The file is what is archived, not what the index says the slot
				// holds: the index can be behind it (ReplaySlots.hxx: ExportSlotFile).
				const fs::path file = saves / std::to_wstring(slot);
				const slots::Bytes replay = LoadFile(file);
				const std::uint32_t crc = slots::Crc32(replay.data(), replay.size());
				wchar_t name[64] = { 0 };
				slots::Bytes exported;
				bool fromRecord = false;
				if (replay.size() < 4 || std::memcmp(replay.data(), "#BRP", 4) || held.count(crc)) continue;
				if (!slots::ExportSlotFile(list, swan, slot, replay, LoadFile(file.wstring() + L".0", 4), exported, fromRecord) || !ArchiveName(exported, name)) {
					spdlog::debug("Replays: slot {} is not a whole replay, not archived", slot);
					continue;
				}
				// Two minutes for the game to write the slot's record, which
				// says more than the file's header does.
				if (!fromRecord && fs::file_time_type::clock::now() - fs::last_write_time(file, ignored) < std::chrono::minutes(2)) continue;
				if (!fromRecord) spdlog::info("Replays: slot {} holds a replay the game's index does not list; archived from the file alone", slot);
				fs::create_directories(folders.archive, ignored);
				if (!SaveFile(folders.archive / name, exported)) { spdlog::warn("Replays: could not write the copy of slot {}", slot); continue; }
				held.insert(crc);
				copied++;
			}
		}
		if (copied) spdlog::info(L"Replays: archived {} to {}", copied, folders.archive.c_str());
		return copied;
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: archiving stopped: {}", e.what());
		return -1;
	}
}

bool ImportFile(const fs::path& file, const Writer& write, const Remover& remove, const Publisher& publish, Imported& out) {
	try {
		// Everything in the game's slots is archived first, so what the import
		// replaces is kept; without that nothing is replaced.
		if (Archive() < 0) { spdlog::warn("Replays: the slots could not be archived, so nothing is imported over them"); return false; }
		const Folders folders = FindFolders();
		// The files written through `write` are the running account's, so the
		// indexes are read from that account and no other.
		const fs::path& saves = folders.active;
		if (saves.empty()) { spdlog::warn("Replays: no save folder for the Steam account that is signed in"); return false; }
		slots::Bytes list = LoadFile(saves / L"LIST", kMostIndexBytes), swan = LoadFile(saves / L"replays-swan.dat", kMostIndexBytes), replay;
		const int slot = slots::SlotToReplace(list, swan, kFirstMatchSlot, kLastMatchSlot);
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
				spdlog::warn(L"Replays: {} has no slot record beside it and no readable header", file.c_str()); return false;
			}
			exported = std::move(built);
		}
		// The replay the slot holds now, to put back should a write fail, and
		// to be sure it is in the archive before it goes.
		const std::string prefix = "capcom/superstreetfighteriv/ssf4_savedata/", name = std::to_string(slot);
		const slots::Bytes before = slot < 0 ? slots::Bytes() : LoadFile(saves / std::to_wstring(slot));
		if (before.size() >= 4 && !std::memcmp(before.data(), "#BRP", 4) && !HeldCrcs(folders.archive).count(slots::Crc32(before.data(), before.size()))) {
			spdlog::warn("Replays: slot {} holds a replay that is not in the archive yet, so it is not replaced", slot);
			return false;
		}
		if (slot < 0 || !slots::Import(exported, slot, static_cast<std::uint32_t>(_time64(nullptr)), list, swan, replay)) {
			spdlog::warn(L"Replays: {} is not a replay Ember can import, or the save index is damaged", file.c_str());
			return false;
		}
		const std::uint8_t* record = slots::Record(list, swan, slot);
		Imported imported;
		imported.slot = slot;
		imported.record.assign(record, record + slots::kRecordBytes);
		imported.slotBytes.assign(swan.begin() + slots::kSwanSlotBytesOffset + slot * 2, swan.begin() + slots::kSwanSlotBytesOffset + slot * 2 + 2);
		std::vector<replayfiles::Change> changes;
		const auto stage = [&](const std::string& leaf, const slots::Bytes& contents) {
			replayfiles::Change change;
			change.name = prefix + leaf; change.after = contents;
			if (!replayfiles::Snapshot(saves / leaf, kMostIndexBytes, change)) return false;
			changes.push_back(std::move(change));
			return true;
		};
		if (!stage(name, replay) || !stage(name + ".0", slots::Sidecar(replay)) ||
			!stage("replays-swan.dat", swan) || !stage("replays-swan.dat.0", slots::Sidecar(swan)) ||
			(slot < slots::kListSlots && (!stage("LIST", list) || !stage("LIST.0", slots::Sidecar(list))))) return false;
		bool restored = true;
		if (!replayfiles::Apply(changes, write, remove, [&] { return publish(imported); }, restored)) {
			spdlog::error("Replays: slot {} was not imported; original files restored: {}", slot, restored);
			return false;
		}
		out = std::move(imported);
		spdlog::info(L"Replays: imported {} into slot {}", file.c_str(), slot);
		return true;
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: import stopped: {}", e.what());
		return false;
	}
}

void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating) {
	try {
		const Folders folders = FindFolders();
		if (folders.archive.empty()) return;
		std::error_code ignored;
		fs::create_directories(folders.archive, ignored);
		std::ofstream out(folders.archive / L"matches.jsonl", std::ios::app);
		out << nlohmann::json{{"started", static_cast<std::uint64_t>(_time64(nullptr))}, {"p1", p1}, {"p2", p2}, {"spectated", spectating}}.dump() << '\n';
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
struct NotedMatch { std::uint64_t started; std::string names[2]; bool spectated; };

std::vector<std::string> WatchedNames(const fs::path& file) {
	std::vector<std::string> names;
	std::ifstream in(file);
	for (std::string line; std::getline(in, line);) if (!line.empty()) names.push_back(line);
	return names;
}

// A line that is not a match as NoteMatchStart writes one is passed over:
// the file is the player's to edit, or to damage.
std::vector<NotedMatch> NotedMatches(const fs::path& file) {
	std::vector<NotedMatch> matches;
	std::ifstream in(file);
	for (std::string line; std::getline(in, line);) {
		const auto json = nlohmann::json::parse(line, nullptr, false);
		if (!json.is_object()) continue;
		const auto started = json.find("started"), spectated = json.find("spectated");
		if (started == json.end() || !started->is_number_unsigned()) continue;
		const auto name = [&](const char* key) { const auto at = json.find(key); return at != json.end() && at->is_string() ? at->get<std::string>() : std::string(); };
		matches.push_back({started->get<std::uint64_t>(), {name("p1"), name("p2")}, spectated != json.end() && spectated->is_boolean() && spectated->get<bool>()});
	}
	return matches;
}

// What a replay's own file says of it: when it was saved and who fought.
// Ember's files sit in the archive root, named by their save time;
// usf4-replay-saver's (.usf4replay, the game's file as it is) may be dropped
// in any folder under it and tell their time and fighters themselves. False
// for any other file.
bool ReadArchived(const fs::path& path, bool inRoot, ArchivedReplay& replay) {
	const std::wstring name = path.filename().wstring();
	const bool saver = path.extension() == L".usf4replay";
	tm utc = {};
	__time64_t time = -1;
	replay = ArchivedReplay{path};
	std::ifstream file(path, std::ios::binary);
	if (saver) {
		slots::Bytes head(slots::kReplayHeaderBytes);
		slots::ReplayHeaderInfo info;
		if (!file.read(reinterpret_cast<char*>(head.data()), head.size()) || !slots::ReadReplayHeader(head, info)) return false;
		replay.fighters[0] = info.fighters[0]; replay.fighters[1] = info.fighters[1];
		time = info.time;
	}
	else {
		if (!inRoot || path.extension() != L".emberreplay" || name.size() < 15 ||
			swscanf_s(name.c_str(), L"%4d%2d%2d-%2d%2d%2d", &utc.tm_year, &utc.tm_mon, &utc.tm_mday, &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6) return false;
		utc.tm_year -= 1900; utc.tm_mon -= 1;
		time = _mkgmtime64(&utc);
		slots::Bytes head(slots::kExportHeaderBytes);
		if (file.read(reinterpret_cast<char*>(head.data()), head.size()) && !std::memcmp(head.data(), slots::kExportMagic, 8)) {
			const slots::RecordInfo info = slots::ReadRecordInfo(head.data() + 8);
			replay.fighters[0] = info.fighters[0]; replay.fighters[1] = info.fighters[1];
		}
	}
	tm local = {};
	char label[32] = { 0 };
	if (time < 0 || _localtime64_s(&local, &time) || !std::strftime(label, sizeof(label), "%Y-%m-%d %H:%M", &local)) return false;
	replay.label = label; replay.time = static_cast<std::uint64_t>(time);
	// The whole replay, once per file (the lister keeps what is read): its
	// score, its length and what each player chose and pressed.
	const slots::Bytes whole = LoadFile(path);
	replayinputs::Match played;
	if (replayinputs::Parse(whole.data(), whole.size(), played)) {
		replay.read = true;
		replayinputs::Score(played, replay.score);
		replay.rounds = static_cast<unsigned>(played.rounds.size());
		for (const auto& round : played.rounds) replay.frames += round.frames;
		for (int side = 0; side < 2; side++) { replay.players[side] = played.players[side]; replay.stats[side] = replayinputs::Count(played, side); }
	}
	return true;
}

// What the lister keeps between two listings, so a listing opens only the
// files that are new or changed: each replay as read, by its path, size and
// time, and the two lists beside them by their time.
struct ListCache {
	struct Entry { std::uintmax_t size; fs::file_time_type written; ArchivedReplay replay; };
	std::map<std::wstring, Entry> replays;
	fs::file_time_type notedWritten{}, watchedWritten{};
	std::vector<NotedMatch> noted;
	std::vector<std::string> watched;
};

std::vector<ArchivedReplay> List(ListCache& cache) {
	std::vector<ArchivedReplay> archived;
	const Folders folders = FindFolders();
	if (folders.archive.empty()) return archived;
	std::error_code ignored;
	const fs::path notedFile = folders.archive / L"matches.jsonl", watchedFile = folders.archive / L"watched.txt";
	const auto notedWritten = fs::last_write_time(notedFile, ignored);
	if (ignored) cache.noted.clear(); else if (notedWritten != cache.notedWritten) cache.noted = NotedMatches(notedFile);
	cache.notedWritten = notedWritten;
	const auto watchedWritten = fs::last_write_time(watchedFile, ignored);
	if (ignored) cache.watched.clear(); else if (watchedWritten != cache.watchedWritten) cache.watched = WatchedNames(watchedFile);
	cache.watchedWritten = watchedWritten;

	std::map<std::wstring, ListCache::Entry> seen;
	for (fs::recursive_directory_iterator at(folders.archive, fs::directory_options::skip_permission_denied, ignored), end; !ignored && at != end; at.increment(ignored)) {
		const fs::path& path = at->path();
		const std::wstring extension = path.extension().wstring();
		if (extension != L".emberreplay" && extension != L".usf4replay") continue;
		std::error_code failed;
		const std::uintmax_t size = at->file_size(failed);
		const fs::file_time_type written = failed ? fs::file_time_type{} : at->last_write_time(failed);
		if (failed) continue;
		const auto kept = cache.replays.find(path.wstring());
		ListCache::Entry entry{size, written, {}};
		if (kept != cache.replays.end() && kept->second.size == size && kept->second.written == written) entry = kept->second;
		else if (!ReadArchived(path, path.parent_path() == folders.archive, entry.replay)) continue;
		ArchivedReplay replay = entry.replay;
		seen.emplace(path.wstring(), std::move(entry));
		// The last match started before the save, allowing two minutes of clock skew.
		const NotedMatch* match = nullptr;
		for (const NotedMatch& candidate : cache.noted)
			if (candidate.started <= replay.time + 120 && replay.time < candidate.started + 3600 && (!match || candidate.started > match->started)) match = &candidate;
		if (match) { replay.names[0] = match->names[0]; replay.names[1] = match->names[1]; replay.spectated = match->spectated; }
		replay.watched = std::find(cache.watched.begin(), cache.watched.end(), WideToUtf8(path.filename().wstring())) != cache.watched.end();
		fs::path video = path;
		replay.video = fs::exists(video.replace_extension(L".mp4"), failed);
		archived.push_back(std::move(replay));
	}
	cache.replays = std::move(seen);
	std::sort(archived.begin(), archived.end(), [](const ArchivedReplay& a, const ArchivedReplay& b) { return a.time > b.time; });
	return archived;
}
}

struct ArchiveLister::State {
	std::mutex mutex;
	std::condition_variable wake;
	bool wanted = false, stop = false, running = false;
	std::shared_ptr<const std::vector<ArchivedReplay>> latest;
};

ArchiveLister::ArchiveLister() : state_(std::make_shared<State>()) {}

// The thread is not joined: it holds the state itself and ends at its next
// wake, and the owner may be going away under the loader lock.
ArchiveLister::~ArchiveLister() {
	std::lock_guard<std::mutex> lock(state_->mutex);
	state_->stop = true;
	state_->wake.notify_all();
}

std::shared_ptr<const std::vector<ArchivedReplay>> ArchiveLister::Latest() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->latest;
}

void ArchiveLister::Want() {
	std::lock_guard<std::mutex> lock(state_->mutex);
	state_->wanted = true;
	state_->wake.notify_all();
	if (state_->running) return;
	state_->running = true;
	std::thread([state = state_] {
		ListCache cache;
		std::unique_lock<std::mutex> lock(state->mutex);
		while (!state->stop) {
			state->wake.wait(lock, [&] { return state->wanted || state->stop; });
			if (state->stop) break;
			state->wanted = false;
			lock.unlock();
			std::shared_ptr<const std::vector<ArchivedReplay>> listed;
			// Whatever a file or a folder throws ends this listing, not the game.
			try { listed = std::make_shared<const std::vector<ArchivedReplay>>(List(cache)); }
			catch (const std::exception& e) { spdlog::warn("Replays: the archive was not listed: {}", e.what()); }
			catch (...) { spdlog::warn("Replays: the archive was not listed"); }
			lock.lock();
			if (listed) state->latest = std::move(listed);
			// Two seconds between listings, however often one is asked for.
			state->wake.wait_for(lock, std::chrono::seconds(2), [&] { return state->stop; });
		}
	}).detach();
}

} } }
