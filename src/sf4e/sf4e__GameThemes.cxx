#include <windows.h>

#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <iterator>
#include <optional>

#include <detours/detours.h>
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"

#include "../Dimps/Dimps.hxx"
#include "../common/install_paths.hxx"
#include "../platform/GameThemes.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__GameThemes.hxx"

namespace {
std::wstring gameDirectory;
namespace game_themes = sf4e::platform::game_themes;
static_assert(std::size(game_themes::Parts) == sf4e::netplay::GameThemePartCount, "a folder for each part");
static_assert(game_themes::Parts[sf4e::netplay::GameThemeMainMenu] == game_themes::MainMenu &&
	game_themes::Parts[sf4e::netplay::GameThemeBattleHud] == game_themes::BattleHud, "the folders in part order");
struct Part {
	std::vector<std::string> names;
	// The theme last picked, once Use has run: in use, or settled as the game's
	// own when it is gone. Unknown while the part in use could not be swapped.
	std::optional<std::string> used;
};
std::array<Part, sf4e::netplay::GameThemePartCount> parts;
// Use has run once: what it does after is the player's pick.
bool started = false;
bool searched = false;
// Game thread only: the next Use is a preview's.
bool reloadTraining = false;
// The developer page's asks, from any thread: the last preview, the themes to
// move out, and every theme's verdict (ember\themes\review.json).
std::mutex previewLock;
struct Asked { int part; std::string name; int row; };
std::optional<Asked> preview;
std::vector<std::string> removals;
// The editor's ask: a theme's battle HUD with these files over it. Game thread:
// the files to write once the next Use has copied the theme.
struct HudAsk { std::string name; std::map<std::string, std::string> files; };
std::optional<HudAsk> hudAsk;
std::atomic<bool> restartAsk{ false };
std::map<std::string, std::string> hudFiles;
std::map<std::string, std::string> verdicts;
std::atomic<int> opens{ 0 };
std::string lastOpen;
namespace fs = std::filesystem;
fs::path Themes() { return fs::path(gameDirectory) / game_themes::Folder; }

// Above the game's own patch folders: their priority (8), and a later order.
constexpr unsigned short Priority = 8, Order = 1;
}

void sf4e::GameThemes::Start(const std::wstring& directory) {
	gameDirectory = directory;
	{
		std::lock_guard<std::mutex> guard(previewLock);
		std::ifstream in(Themes() / "review.json");
		const auto review = nlohmann::json::parse(in, nullptr, false);
		if (review.is_object())
			for (const auto& [name, verdict] : review.items()) if (verdict.is_string()) verdicts[name] = verdict.get<std::string>();
	}
	for (int i = 0; i < netplay::GameThemePartCount; ++i) {
		parts[i].names = game_themes::Names(gameDirectory, game_themes::Parts[i]);
		spdlog::info("In-game themes: {} with {}", parts[i].names.size(), platform::WideToUtf8(game_themes::Parts[i]));
	}
}

void sf4e::GameThemes::Search() {
	void* const list = Dimps::App::resourcePathList ? *Dimps::App::resourcePathList : nullptr;
	if (searched || !list) return;
	using game_themes::ActiveFolder;
	Dimps::App::ShortString folder = {};
	constexpr unsigned size = sizeof(ActiveFolder) / sizeof(*ActiveFolder) - 1;
	static_assert(size < sizeof(folder.text), "the folder's name fits the short form");
	for (unsigned i = 0; i < size; ++i) folder.text[i] = static_cast<char>(ActiveFolder[i]);
	folder.size = size;
	folder.capacity = 15;
	Dimps::App::AddResourcePath(list, Priority, &folder, Order);
	searched = true;
	spdlog::info("In-game themes: the game searches ember\\theme first");
}

std::wstring sf4e::GameThemes::Directory() { return gameDirectory; }

void sf4e::GameThemes::ApplyHud(const std::string& name, std::map<std::string, std::string> files) {
	std::lock_guard<std::mutex> guard(previewLock);
	hudAsk = HudAsk{ name, std::move(files) };
}

const std::vector<std::string>& sf4e::GameThemes::Names(netplay::GameThemePart part) { return parts[part].names; }

void sf4e::GameThemes::Preview(netplay::GameThemePart part, const std::string& name, int mainMenuRow) {
	std::lock_guard<std::mutex> guard(previewLock);
	preview = Asked{ part, name, mainMenuRow };
	opens = 0;
	lastOpen.clear();
}

namespace {
#ifdef SF4E_DEVELOPER_UI
decltype(&CreateFileA) realCreateFileA = CreateFileA;
decltype(&CreateFileW) realCreateFileW = CreateFileW;
template <typename Char> void NoteOpen(const Char* name) {
	if (!name) return;
	std::basic_string<Char> path(name);
	for (auto& c : path) c = static_cast<Char>(towlower(c));
	constexpr Char marker[] = { 'e','m','b','e','r','\\','t','h','e','m','e','\\',0 };
	if (path.find(marker) == std::basic_string<Char>::npos) return;
	++opens;
	std::lock_guard<std::mutex> guard(previewLock);
	lastOpen.assign(path.begin(), path.end());
}
HANDLE WINAPI ThemeCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD how, DWORD flags, HANDLE templ) {
	NoteOpen(name);
	return realCreateFileA(name, access, share, security, how, flags, templ);
}
HANDLE WINAPI ThemeCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD how, DWORD flags, HANDLE templ) {
	NoteOpen(name);
	return realCreateFileW(name, access, share, security, how, flags, templ);
}
#endif
}

void sf4e::GameThemes::Install() {
#ifdef SF4E_DEVELOPER_UI
	DetourAttach((PVOID*)&realCreateFileA, ThemeCreateFileA);
	DetourAttach((PVOID*)&realCreateFileW, ThemeCreateFileW);
#endif
}

int sf4e::GameThemes::ThemeFileOpens() { return opens; }

std::string sf4e::GameThemes::LastThemeFile() {
	std::lock_guard<std::mutex> guard(previewLock);
	return lastOpen;
}

std::string sf4e::GameThemes::Verdict(const std::string& name) {
	std::lock_guard<std::mutex> guard(previewLock);
	const auto found = verdicts.find(name);
	return found == verdicts.end() ? std::string() : found->second;
}

void sf4e::GameThemes::Judge(const std::string& name, const std::string& verdict) {
	std::lock_guard<std::mutex> guard(previewLock);
	verdicts[name] = verdict;
	if (verdict == "remove") removals.push_back(name);
	std::ofstream(Themes() / "review.json") << nlohmann::json(verdicts).dump(1);
}

void sf4e::GameThemes::Restart() { restartAsk = true; }

void sf4e::GameThemes::TakePreview() {
	if (restartAsk.exchange(false)) {
		wchar_t root[MAX_PATH] = {};
		if (install::GetInstallRoot(root, MAX_PATH)) {
			const auto launcher = fs::path(root) / L"Launcher.exe";
			std::wstring command = L"\"" + launcher.wstring() + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId());
			STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
			if (CreateProcessW(launcher.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, root, &startup, &process)) {
				CloseHandle(process.hThread); CloseHandle(process.hProcess);
				spdlog::info("In-game themes: restarting the game");
				Dimps::Platform::Main::Close();
				return;
			}
		}
		spdlog::warn("In-game themes: the game could not be restarted");
	}
	std::optional<Asked> taken;
	std::optional<HudAsk> hud;
	std::vector<std::string> removed;
	{ std::lock_guard<std::mutex> guard(previewLock); taken.swap(preview); removed.swap(removals); hud.swap(hudAsk); }
	// A removed theme goes to ember\themes-removed, never away; parts using it
	// go back to the game's own, and the lists are found again.
	for (const auto& name : removed) {
		std::array<std::string, netplay::GameThemePartCount> names;
		for (int i = 0; i < netplay::GameThemePartCount; ++i) {
			names[i] = parts[i].used.value_or(std::string());
			if (names[i] == name) names[i].clear();
		}
		Use(names);
		std::error_code error;
		const fs::path out = fs::path(gameDirectory) / L"ember\\themes-removed";
		fs::create_directories(out, error);
		fs::rename(Themes() / platform::Utf8ToWide(name.c_str()), out / platform::Utf8ToWide(name.c_str()), error);
		if (error) spdlog::warn("In-game themes: {} not moved out: {}", name, error.message());
		else spdlog::info("In-game themes: {} moved to ember\\themes-removed", name);
		for (int i = 0; i < netplay::GameThemePartCount; ++i) parts[i].names = game_themes::Names(gameDirectory, game_themes::Parts[i]);
	}
	if (hud && started) {
		// Used again even when it is the theme in use: the copy under ember\theme is the one the files go over.
		parts[netplay::GameThemeBattleHud].used.reset();
		hudFiles = std::move(hud->files);
		taken = Asked{ netplay::GameThemeBattleHud, hud->name, -1 };
	}
	if (!taken || !started) return;
	std::array<std::string, netplay::GameThemePartCount> names;
	for (int i = 0; i < netplay::GameThemePartCount; ++i) names[i] = parts[i].used.value_or(std::string());
	names[taken->part] = taken->name;
	reloadTraining = taken->part == netplay::GameThemeBattleHud;
	Use(names);
	if (taken->row >= 0 && GameEvents::MainMenu::Open(taken->row)) spdlog::info("In-game themes: main menu row {} opened for the preview", taken->row);
}

void sf4e::GameThemes::Use(const std::array<std::string, netplay::GameThemePartCount>& names) {
	if (gameDirectory.empty()) return;
	const bool picked = started, reload = reloadTraining;
	reloadTraining = false;
	started = true;
	bool mainMenuChanged = false, hudChanged = false;
	for (int i = 0; i < netplay::GameThemePartCount; ++i) {
		Part& part = parts[i];
		const std::string& name = names[i];
		if (part.used == name) continue;
		std::string error;
		const std::string folder = platform::WideToUtf8(game_themes::Parts[i]);
		const game_themes::Outcome outcome = game_themes::Use(gameDirectory, game_themes::Parts[i], name, error);
		if (outcome == game_themes::Outcome::Used && i == netplay::GameThemeBattleHud) {
			for (const auto& [file, bytes] : hudFiles) {
				std::string problem;
				if (!game_themes::WriteActive(gameDirectory, game_themes::Parts[i], file, bytes, problem))
					spdlog::warn("In-game themes: HUD file {} not written: {}", file, problem);
			}
		}
		if (i == netplay::GameThemeBattleHud) hudFiles.clear();
		if (outcome == game_themes::Outcome::Used)
			spdlog::info("In-game themes: {} {}", folder, name.empty() ? std::string("the game's own") : name);
		else spdlog::warn("In-game themes: {} {} could not be used: {}", folder, name, error);
		// Only a part that could not be swapped is tried again at the next pick.
		part.used = outcome == game_themes::Outcome::Unchanged ? std::nullopt : std::optional<std::string>(name);
		mainMenuChanged |= i == netplay::GameThemeMainMenu && outcome != game_themes::Outcome::Unchanged;
		hudChanged |= i == netplay::GameThemeBattleHud && outcome != game_themes::Outcome::Unchanged;
	}
	if (picked && hudChanged && reload && GameEvents::TrainingMode::Reload())
		spdlog::info("In-game themes: Training battle loaded again for its HUD");
	// A main menu picked on the main menu shows at once: the menu opens again,
	// as it does coming back from a mode. Other screens load theirs as they open.
	if (picked && mainMenuChanged && GameEvents::MainMenu::Open(Dimps::GameEvents::MainMenu::FlowRowMainMenuAgain))
		spdlog::info("In-game themes: main menu opened again");
}
