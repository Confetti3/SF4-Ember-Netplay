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
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../platform/DurableFile.hxx"
#include "../platform/GameThemeGuard.hxx"
#include "../platform/GameThemes.hxx"
#include "../platform/HudBundleCheck.hxx"
#include "../platform/Utf8.hxx"
#include "../training/GameFiles.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__GameThemes.hxx"
#include "sf4e__Game__Battle__Hud.hxx"
#include "sf4e__NetplayFacade.hxx"

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
	// The theme whose files are in ember\theme for this part now: empty for the
	// game's own, or when the pick could not be copied.
	std::string loaded;
};
std::array<Part, sf4e::netplay::GameThemePartCount> parts;
// The crash guard (platform/GameThemeGuard.hxx), and the picks it turned off at start-up that the
// runtime has yet to put back in the settings and tell the player about.
game_themes::Guard guard;
std::vector<sf4e::GameThemes::Turned> turned;
// What Use found about a pick this session, by part and theme: refused before use, or used with some HUD pictures missing.
std::array<std::map<std::string, sf4e::netplay::GameThemeNote>, sf4e::netplay::GameThemePartCount> noted;
std::string PartFolder(int part) { return sf4e::platform::WideToUtf8(game_themes::Parts[part]); }
// The game's own newest copy of a file of battle\hud, by the game's folders, newest first.
std::filesystem::path GameHudFile(const std::wstring& relative) {
	std::error_code ignored;
	for (const char* layer : sf4e::training::GameLayers) {
		const std::filesystem::path path = std::filesystem::path(gameDirectory) / layer / L"battle\\hud" / relative;
		if (std::filesystem::is_regular_file(path, ignored)) return path;
	}
	return {};
}
// What a part must satisfy before it is used: the battle HUD's layouts and sheets belong together.
const game_themes::Rules& ThemeRules() {
	static const game_themes::Rules rules = [] {
		game_themes::Rules made;
		made.battleHud = [](const std::wstring& folder, std::string& reason) { return sf4e::platform::hud_bundle::Check(folder, GameHudFile, reason); };
		made.pictureGuard = sf4e::Game::Battle::Hud::PictureGuardInstalled();
		return made;
	}();
	return rules;
}
void Remember() {
	if (!game_themes::Save(gameDirectory, guard)) spdlog::warn("In-game themes: the crash guard's state could not be saved");
}
// Use has run once: what it does after is the player's pick.
bool started = false;
bool searched = false;
// Game thread only: the next Use is a preview's.
bool reloadTraining = false;
// The developer page's asks, from any thread: the last preview, the themes to
// move out, and every theme's verdict (ember\themes\review.json).
std::mutex previewLock;
// files: with a battle HUD, the editor's files to write over the theme once it is copied (path under battle\hud -> bytes).
struct Asked { int part; std::string name; int row; std::map<std::string, std::string> files; };
std::optional<Asked> preview;
std::vector<std::string> removals;
std::atomic<bool> restartAsk{ false };
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
	// A theme still armed is one the game closed on while loading it.
	guard = game_themes::Load(gameDirectory);
	for (const auto& off : game_themes::Recover(guard)) {
		for (int i = 0; i < netplay::GameThemePartCount; ++i)
			if (PartFolder(i) == off.part) turned.push_back({ static_cast<netplay::GameThemePart>(i), off.theme });
		spdlog::warn("In-game themes: the game closed while loading {} for {}; it is turned off", off.theme, off.part);
	}
	if (!turned.empty()) Remember();
}

void sf4e::GameThemes::TurnOff(std::array<std::string, netplay::GameThemePartCount>& picks) {
	for (const auto& off : turned) if (picks[off.part] == off.theme) picks[off.part].clear();
}

const std::vector<sf4e::GameThemes::Turned>& sf4e::GameThemes::TurnedOff() { return turned; }

void sf4e::GameThemes::TurnedOffTold() { turned.clear(); }

sf4e::netplay::GameThemeNote sf4e::GameThemes::Note(netplay::GameThemePart part, const std::string& theme) {
	const auto found = noted[part].find(theme);
	if (found != noted[part].end()) return found->second;
	return game_themes::Closed(guard, PartFolder(part), theme) ? netplay::GameThemeNote::ClosedGame : netplay::GameThemeNote::None;
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

// Set once by Start at launch, before the remote can run; read-only after.
std::wstring sf4e::GameThemes::Directory() { return gameDirectory; }

void sf4e::GameThemes::ApplyHud(const std::string& name, std::map<std::string, std::string> files) {
	std::lock_guard<std::mutex> guard(previewLock);
	preview = Asked{ netplay::GameThemeBattleHud, name, -1, std::move(files) };
}

const std::vector<std::string>& sf4e::GameThemes::Names(netplay::GameThemePart part) { return parts[part].names; }

void sf4e::GameThemes::Preview(netplay::GameThemePart part, const std::string& name, int mainMenuRow) {
	std::lock_guard<std::mutex> guard(previewLock);
	preview = Asked{ part, name, mainMenuRow, {} };
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
	const std::string text = nlohmann::json(verdicts).dump(1);
	const fs::path file = Themes() / "review.json";
	if (!durable::PublishReplace(file, durable::PartialPath(file), text.data(), text.size())) spdlog::warn("In-game themes: the verdicts could not be saved");
}

void sf4e::GameThemes::Restart() { restartAsk = true; }

namespace {
// The battle HUD is read as a battle loads. A battle that is loading arms the
// guard for the HUD theme in use, and one that has reached the fight proves it.
// A game that closes in between leaves the mark for the next start (Start).
void WatchBattleHud() {
	using System = Dimps::Game::Battle::System;
	const std::string& theme = parts[sf4e::netplay::GameThemeBattleHud].loaded;
	// The game's singleton getter is set once its offsets are known; without the game (the runtime's tests) there is no battle.
	if (theme.empty() || !System::staticMethods.GetSingleton) return;
	System* const system = System::staticMethods.GetSingleton();
	if (!system) return;
	const int state = *System::GetReadyState(system);
	const std::string folder = PartFolder(sf4e::netplay::GameThemeBattleHud);
	const bool loading = state > System::RS_START && state < System::RS_FIGHT;
	const bool changed = state == System::RS_FIGHT ? game_themes::Prove(guard, folder, theme) : loading && game_themes::Arm(guard, folder, theme);
	if (!changed) return;
	spdlog::info("In-game themes: {} {} {}", folder, theme, loading ? "is loading" : "loaded");
	Remember();
}
}

void sf4e::GameThemes::TakePreview() {
	WatchBattleHud();
	// The phone's answer was given when its request began; a match may have started since, so the rule is asked again here.
	const bool inMatch = NetplayFacade::GetRuntimeSnapshotShared()->InMatch();
	if (restartAsk.exchange(false)) {
		if (!inMatch && platform::StartLauncherAfterExit({})) {
			spdlog::info("In-game themes: restarting the game");
			Dimps::Platform::Main::Close();
			return;
		}
		spdlog::warn(inMatch ? "In-game themes: the restart was not done, a match is on" : "In-game themes: the game could not be restarted");
	}
	std::optional<Asked> taken;
	std::vector<std::string> removed;
	{ std::lock_guard<std::mutex> guard(previewLock); taken.swap(preview); removed.swap(removals); }
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
	if (!taken || !started) return;
	if (inMatch) { spdlog::warn("In-game themes: {} was not applied, a match is on", taken->name); return; }
	std::array<std::string, netplay::GameThemePartCount> names;
	for (int i = 0; i < netplay::GameThemePartCount; ++i) names[i] = parts[i].used.value_or(std::string());
	names[taken->part] = taken->name;
	reloadTraining = taken->part == netplay::GameThemeBattleHud;
	// Used again even when it is the theme in use: the copy under ember\theme is the one the files go over.
	if (!taken->files.empty()) parts[taken->part].used.reset();
	Use(names, taken->files);
	if (taken->row >= 0 && GameEvents::MainMenu::Open(taken->row)) spdlog::info("In-game themes: main menu row {} opened for the preview", taken->row);
}

void sf4e::GameThemes::Use(const std::array<std::string, netplay::GameThemePartCount>& names, const std::map<std::string, std::string>& hudFiles) {
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
		const game_themes::Outcome outcome = game_themes::Use(gameDirectory, game_themes::Parts[i], name, error, ThemeRules());
		const bool used = game_themes::InUse(outcome);
		if (used && i == netplay::GameThemeBattleHud) {
			for (const auto& [file, bytes] : hudFiles) {
				std::string problem;
				if (!game_themes::WriteActive(gameDirectory, game_themes::Parts[i], file, bytes, problem))
					spdlog::warn("In-game themes: HUD file {} not written: {}", file, problem);
			}
		}
		if (used) {
			part.loaded = name;
			noted[i].erase(name);
			if (outcome == game_themes::Outcome::UsedWithMissingPictures) noted[i][name] = netplay::GameThemeNote::PicturesMissing;
			// Files written over the copy are not the files that loaded before.
			if (i == netplay::GameThemeBattleHud && !hudFiles.empty() && game_themes::Forget(guard, folder)) Remember();
			spdlog::info("In-game themes: {} {}", folder, name.empty() ? std::string("the game's own") : name);
			if (outcome == game_themes::Outcome::UsedWithMissingPictures) spdlog::warn("In-game themes: {} {} is used, and some pictures its HUD names are not drawn: {}", folder, name, error);
		}
		else {
			if (outcome != game_themes::Outcome::Unchanged) part.loaded.clear();
			if (outcome == game_themes::Outcome::PicturesMismatch) noted[i][name] = netplay::GameThemeNote::PicturesMismatch;
			if (outcome == game_themes::Outcome::Unreadable) noted[i][name] = netplay::GameThemeNote::Unreadable;
			spdlog::warn("In-game themes: {} {} could not be used: {}", folder, name, error);
		}
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
