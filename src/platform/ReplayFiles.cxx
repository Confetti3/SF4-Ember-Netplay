#include "ReplayFiles.hxx"

#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <time.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
namespace slots = sf4e::replayslots;

namespace sf4e { namespace platform { namespace replays {
namespace {

slots::Bytes LoadFile(const fs::path& path) {
	std::ifstream file(path, std::ios::binary);
	return slots::Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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

// "20261005-213503-69991186.emberreplay": the save time (UTC) and the CRC.
bool ArchiveName(const slots::SlotInfo& info, wchar_t (&name)[64]) {
	const __time64_t saved = info.time;
	tm utc = {};
	return !_gmtime64_s(&utc, &saved) && SUCCEEDED(StringCchPrintfW(name, 64, L"%04d%02d%02d-%02d%02d%02d-%08x.emberreplay",
		utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, info.crc));
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
	std::error_code ignored;
	for (const auto& account : fs::directory_iterator(fs::path(steamPath) / L"userdata", ignored)) {
		const fs::path saves = account.path() / L"45760" / L"remote" / L"capcom" / L"superstreetfighteriv" / L"ssf4_savedata";
		if (fs::exists(saves / L"replays-swan.dat", ignored)) folders.saves.push_back(saves);
	}
	return folders;
}

int Archive() {
	try {
		const Folders folders = FindFolders();
		if (folders.archive.empty()) return -1;
		std::error_code ignored;
		int copied = 0;
		for (const fs::path& saves : folders.saves) {
			const slots::Bytes list = LoadFile(saves / L"LIST"), swan = LoadFile(saves / L"replays-swan.dat");
			if (!slots::ValidSwan(swan)) continue;
			for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; slot++) {
				const slots::SlotInfo info = slots::ReadSlot(list, swan, slot);
				wchar_t name[64] = { 0 };
				if (!info.used || !ArchiveName(info, name) || fs::exists(folders.archive / name, ignored)) continue;
				slots::Bytes exported;
				if (!slots::Export(list, swan, slot, LoadFile(saves / std::to_wstring(slot)), exported)) {
					spdlog::debug("Replays: slot {} does not match its file, not archived", slot);
					continue;
				}
				fs::create_directories(folders.archive, ignored);
				if (!SaveFile(folders.archive / name, exported)) { spdlog::warn("Replays: could not write the copy of slot {}", slot); continue; }
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

bool ImportFile(const fs::path& file, const Writer& write, Imported& out) {
	try {
		Archive();
		const Folders folders = FindFolders();
		if (folders.saves.empty()) { spdlog::warn("Replays: no save folder to import into"); return false; }
		// The account that played most recently.
		std::error_code ignored;
		fs::path saves = folders.saves.front();
		for (const fs::path& candidate : folders.saves) {
			if (fs::last_write_time(candidate / L"replays-swan.dat", ignored) > fs::last_write_time(saves / L"replays-swan.dat", ignored)) saves = candidate;
		}
		slots::Bytes list = LoadFile(saves / L"LIST"), swan = LoadFile(saves / L"replays-swan.dat"), replay;
		const int slot = slots::SlotToReplace(list, swan, kFirstMatchSlot, kLastMatchSlot);
		if (slot < 0 || !slots::Import(LoadFile(file), slot, static_cast<std::uint32_t>(_time64(nullptr)), list, swan, replay)) {
			spdlog::warn(L"Replays: {} is not a replay Ember can import, or the save index is damaged", file.c_str());
			return false;
		}
		const std::string prefix = "capcom/superstreetfighteriv/ssf4_savedata/", name = std::to_string(slot);
		bool written = write(prefix + name, replay) && write(prefix + name + ".0", slots::Sidecar(replay)) &&
			write(prefix + "replays-swan.dat", swan) && write(prefix + "replays-swan.dat.0", slots::Sidecar(swan));
		if (written && slot < slots::kListSlots) written = write(prefix + "LIST", list) && write(prefix + "LIST.0", slots::Sidecar(list));
		if (!written) { spdlog::error("Replays: could not write slot {} and its index", slot); return false; }
		const std::uint8_t* record = slots::Record(list, swan, slot);
		out.slot = slot;
		out.record.assign(record, record + slots::kRecordBytes);
		out.slotBytes.assign(swan.begin() + slots::kSwanSlotBytesOffset + slot * 2, swan.begin() + slots::kSwanSlotBytesOffset + slot * 2 + 2);
		spdlog::info(L"Replays: imported {} into slot {}", file.c_str(), slot);
		return true;
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: import stopped: {}", e.what());
		return false;
	}
}

void NoteMatchStart(const std::string& p1, const std::string& p2) {
	try {
		const Folders folders = FindFolders();
		if (folders.archive.empty()) return;
		std::error_code ignored;
		fs::create_directories(folders.archive, ignored);
		std::ofstream out(folders.archive / L"matches.jsonl", std::ios::app);
		out << nlohmann::json{{"started", static_cast<std::uint64_t>(_time64(nullptr))}, {"p1", p1}, {"p2", p2}}.dump() << '\n';
	}
	catch (const std::exception& e) {
		spdlog::warn("Replays: the match's names were not noted: {}", e.what());
	}
}

namespace {
struct NotedMatch { std::uint64_t started; std::string names[2]; };

std::vector<NotedMatch> NotedMatches(const fs::path& archive) {
	std::vector<NotedMatch> matches;
	std::ifstream in(archive / L"matches.jsonl");
	for (std::string line; std::getline(in, line);) {
		const auto json = nlohmann::json::parse(line, nullptr, false);
		if (!json.is_object() || !json.contains("started")) continue;
		matches.push_back({json.value("started", std::uint64_t(0)), {json.value("p1", ""), json.value("p2", "")}});
	}
	return matches;
}
}

std::vector<ArchivedReplay> ListArchive() {
	std::vector<ArchivedReplay> archived;
	const Folders folders = FindFolders();
	if (folders.archive.empty()) return archived;
	const std::vector<NotedMatch> noted = NotedMatches(folders.archive);
	std::error_code ignored;
	for (const auto& entry : fs::directory_iterator(folders.archive, ignored)) {
		const std::wstring name = entry.path().filename().wstring();
		tm utc = {};
		if (entry.path().extension() != L".emberreplay" || name.size() < 15 ||
			swscanf_s(name.c_str(), L"%4d%2d%2d-%2d%2d%2d", &utc.tm_year, &utc.tm_mon, &utc.tm_mday, &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6) continue;
		utc.tm_year -= 1900; utc.tm_mon -= 1;
		const __time64_t time = _mkgmtime64(&utc);
		tm local = {};
		char label[32] = { 0 };
		if (time < 0 || _localtime64_s(&local, &time) || !std::strftime(label, sizeof(label), "%Y-%m-%d %H:%M", &local)) continue;
		ArchivedReplay replay{entry.path(), label, static_cast<std::uint64_t>(time)};
		std::ifstream file(entry.path(), std::ios::binary);
		slots::Bytes head(slots::kExportHeaderBytes);
		if (file.read(reinterpret_cast<char*>(head.data()), head.size()) && !std::memcmp(head.data(), slots::kExportMagic, 8)) {
			const slots::RecordInfo info = slots::ReadRecordInfo(head.data() + 8);
			replay.fighters[0] = info.fighters[0]; replay.fighters[1] = info.fighters[1];
		}
		// The last match started before the save, allowing two minutes of clock skew.
		const NotedMatch* match = nullptr;
		for (const NotedMatch& candidate : noted)
			if (candidate.started <= replay.time + 120 && replay.time < candidate.started + 3600 && (!match || candidate.started > match->started)) match = &candidate;
		if (match) { replay.names[0] = match->names[0]; replay.names[1] = match->names[1]; }
		archived.push_back(replay);
	}
	std::sort(archived.begin(), archived.end(), [](const ArchivedReplay& a, const ArchivedReplay& b) { return a.time > b.time; });
	return archived;
}

} } }
