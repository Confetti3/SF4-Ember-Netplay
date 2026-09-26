#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <pathcch.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <winuser.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include "../ui/RecoverySurface.hxx"
#include "../platform/LauncherInstance.hxx"
#include "../platform/Utf8.hxx"
#include "../platform/WineBuiltin.hxx"

#include <CLI/CLI.hpp>
#include <detours/detours.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "../sf4e/sf4e.hxx"
#include "../sidecar/sidecar.hxx"
#include "../common/CrashReport.hxx"
#include "../common/sf4e__NetplayConfig.hxx"
#include "../common/install_paths.hxx"
#include "../common/Localization.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include "GameLocator.hxx"
#include "netplay/netplay_persist.hxx"
#include "update/github_release_client.hxx"

LPCWCH szLibrarySuffix = L"steamapps\\common\\Super Street Fighter IV - Arcade Edition";

void ConfigureLauncherLogging() {
	PWSTR appData = NULL;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appData) != S_OK) {
		return;
	}

	wchar_t sf4eDir[MAX_PATH] = { 0 };
	wchar_t logsDir[MAX_PATH] = { 0 };
	wchar_t logPath[MAX_PATH] = { 0 };
	if (SUCCEEDED(PathCchCombine(sf4eDir, MAX_PATH, appData, L"sf4e"))) {
		CreateDirectoryW(sf4eDir, NULL);
		if (SUCCEEDED(PathCchCombine(logsDir, MAX_PATH, sf4eDir, L"logs"))) {
			CreateDirectoryW(logsDir, NULL);
			if (SUCCEEDED(PathCchCombine(logPath, MAX_PATH, logsDir, L"launcher.log"))) {
				try {
					std::vector<spdlog::sink_ptr> sinks;
					sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
						logPath,
						1048576 * 5,
						10,
						true
					));
					auto logger = std::make_shared<spdlog::logger>("sf4e-launcher", sinks.begin(), sinks.end());
					spdlog::set_default_logger(logger);
					spdlog::flush_on(spdlog::level::info);
					spdlog::info("Launcher logging initialized");
				}
				catch (const spdlog::spdlog_ex&) {
					// Logging should never block game startup.
				}
			}
		}
	}
	CoTaskMemFree(appData);
}

int FindSF4ByEnvironmentVariable(
	_Out_ LPWSTR szGameDirectory, _In_ int nGameDirSize,
	_Out_ LPWSTR szExePath, _In_ int nExeSize
) {
	DWORD nDirSize = 0;
	DWORD err = 0;
	HRESULT res = S_OK;
	nDirSize = GetEnvironmentVariableW(L"STEAM_APP_PATH", szGameDirectory, nGameDirSize);

	if (nDirSize == 0) {
		err = GetLastError();
		// Most Windows users likely won't define this- don't warn on a very
		// common case.
		if (err != ERROR_ENVVAR_NOT_FOUND) {
			spdlog::warn(L"FindSF4ByEnvironmentVariable: GetEnvironmentVariable(\"STEAM_APP_PATH\", ...) failed: {}", err);
		}
		return 0;
	}

	if (nDirSize > nGameDirSize) {
		spdlog::warn(L"FindSF4ByEnvironmentVariable: STEAM_APP_PATH declared but buffer too small; had {}, needed {}", nGameDirSize, nDirSize);
		return 0;
	}

	if ((res = PathCchCombine(szExePath, nExeSize, szGameDirectory, sf4e::launcher::kGameExecutableName)) != S_OK) {
		spdlog::warn(L"FindSF4ByEnvironmentVariable: PathCchCombine failed: {}", res);
		return 0;
	}

	if (!PathFileExistsW(szExePath)) {
		spdlog::warn(L"FindSF4ByEnvironmentVariable: STEAM_APP_PATH provided as {}, but {} not found", szGameDirectory, szExePath);
		return 0;
	}

	return 1;
}

int FindSF4ByEstimatedSteamPath(
	_Out_ LPWSTR szGameDirectory, _In_ int nGameDirSize,
	_Out_ LPWSTR szExePath, _In_ int nExeSize
) {
	wchar_t szSteamPath[1024] = { 0 };
	DWORD dwDataRead = sizeof(szSteamPath);
	wchar_t szLibraryFolderVDFPath[1024];
	HRESULT res = S_OK;

	// Capture SteamPath, which always acts as the first library
	LSTATUS lQueryStatus = RegGetValueW(
		HKEY_CURRENT_USER,
		L"Software\\Valve\\Steam",
		L"SteamPath",
		RRF_RT_REG_SZ,
		NULL,
		szSteamPath,
		&dwDataRead
	);
	if (lQueryStatus != ERROR_SUCCESS) {
		spdlog::warn(L"FindSF4ByEstimatedSteamPath: Could not query registry for SteamPath: {}", lQueryStatus);
		return 0;
	}

	// Read the library paths from `libraryfolders.vdf` inside SteamPath. A
	// missing or unreadable file only costs the extra libraries.
	std::string libraryFolders;
	if ((res = PathCchCombine(szLibraryFolderVDFPath, 1024, szSteamPath, L"steamapps\\libraryfolders.vdf")) != S_OK) {
		spdlog::warn(L"FindSF4ByEstimatedSteamPath: szLibraryFolderVDFPath PathCchCombine failed: {}", res);
	}
	else {
		std::ifstream libraryFoldersFile(szLibraryFolderVDFPath, std::ios::binary);
		if (libraryFoldersFile.is_open()) {
			libraryFolders.assign(std::istreambuf_iterator<char>(libraryFoldersFile), std::istreambuf_iterator<char>());
		}
		else {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: could not open {}, searching SteamPath only", szLibraryFolderVDFPath);
		}
	}
	const std::vector<std::wstring> libraries = sf4e::launcher::LibraryCandidates(szSteamPath, libraryFolders);
	spdlog::info(L"FindSF4ByEstimatedSteamPath: searching {} Steam libraries", libraries.size());

	// Search the discovered libraries
	for (const std::wstring& library : libraries) {
		if (!PathIsDirectoryW(library.c_str())) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: detected library {} does not exist", library.c_str());
			continue;
		}

		if ((res = PathCchCombine(szGameDirectory, nGameDirSize, library.c_str(), szLibrarySuffix)) != S_OK) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: szGameDirectory PathCchCombine for {} failed: {}", library.c_str(), res);
			continue;
		}

		if (!PathIsDirectoryW(szGameDirectory)) {
			// A common case- any given library may not contain SF4, so logging would
			// add more noise than signal.
			continue;
		}

		if ((res = PathCchCombine(szExePath, nExeSize, szGameDirectory, sf4e::launcher::kGameExecutableName)) != S_OK) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: szExePath PathCchCombine failed: {}", res);
			continue;
		}

		if (PathFileExistsW(szExePath)) {
			return 1;
		}
	}

	return 0;
}

int FindSF4(
	_Out_ LPWSTR szGameDirectory, _In_ int nGameDirSize,
	_Out_ LPWSTR szExePath, _In_ int nExeSize
) {
	if (FindSF4ByEnvironmentVariable(szGameDirectory, nGameDirSize, szExePath, nExeSize)) {
		return 1;
	}

	if (FindSF4ByEstimatedSteamPath(szGameDirectory, nGameDirSize, szExePath, nExeSize)) {
		return 1;
	}

	return 0;
}

void CreateAppIDFile(LPWSTR szGuiltyDirectory) {
	wchar_t szAppIDPath[1024] = { 0 };
	DWORD nBytesWritten = 0;

	SetEnvironmentVariableA("SteamAppId", "45760");
	SetEnvironmentVariableA("SteamGameId", "45760");

	PathCombine(szAppIDPath, szGuiltyDirectory, L"steam_appid.txt");
	if (PathFileExistsW(szAppIDPath)) {
		return;
	}

	char allow[8] = { 0 };
	if (GetEnvironmentVariableA("SF4E_ALLOW_STEAM_APPID_WRITE", allow, sizeof(allow)) == 0) {
		spdlog::warn(L"Game steam_appid.txt missing at {}; runtime write disabled, relying on inherited SteamAppId/SteamGameId env", szAppIDPath);
		return;
	}

	HANDLE hAppIDHandle = CreateFile(
		szAppIDPath,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);


	if (hAppIDHandle != INVALID_HANDLE_VALUE) {
		// Dev fallback only. Tester packages rely on env vars and packaged app id files.
		WriteFile(hAppIDHandle, "45760", 6, &nBytesWritten, NULL);
		spdlog::info(L"Created game steam_appid.txt dev fallback at {}", szAppIDPath);
		if (nBytesWritten != 6) {
			spdlog::warn(L"Could not fully write game steam_appid.txt at {}", szAppIDPath);
		}
		CloseHandle(hAppIDHandle);
	}
	else {
		spdlog::warn(L"Could not create game steam_appid.txt dev fallback at {} (Win32 {})", szAppIDPath, GetLastError());
	}
}

HANDLE CreateSF4Process(
	const sf4e::Payload& payload,
	sf4e::platform::HelperProcess& helper,
    sf4e::platform::HelperProcess& discord,
	const std::wstring& helperPath,
	LPWSTR szGameDirectory,
	LPWSTR szExePath,
	int nDlls,
	LPCSTR* rlpDlls
) {
	wchar_t szErrorString[1024] = { 0 };
	DWORD dwError;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	si.cb = sizeof(si);
	spdlog::info(
		L"CreateSF4Process start exe={} gameDir={} mode={} configVersion={} devOverlay={}",
		szExePath,
		szGameDirectory,
		payload.netplay.mode,
		payload.netplay.version,
		(int)payload.netplay.devOverlay
	);
	HANDLE hSyncEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (hSyncEvent == NULL) {
		spdlog::warn("CreateSF4Process: CreateEventW() could not create game sync handle, game may be unable to access Steam: err {}", GetLastError());
	}

	SetLastError(0);

	if (
		!DetourCreateProcessWithDllsW(
			szExePath,
			NULL,
			NULL,
			NULL,
			TRUE,
			CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
			NULL,
			szGameDirectory,
			&si,
			&pi,
			nDlls,
			rlpDlls,
			NULL
		)) {
		dwError = GetLastError();
		StringCchPrintf(szErrorString, 1024, L"DetourCreateProcessWithDllEx failed: %d", dwError);
        spdlog::error("Could not start the game with Sidecar (Win32 {})", dwError);
        if (hSyncEvent) CloseHandle(hSyncEvent);
        return nullptr;
	}

	sf4e::Payload p = payload;
    wchar_t discordPath[32768] = {};
    if (sf4e::install::ResolveInstallFile(L"ember-discord.exe",discordPath,32768) &&
        discord.Start(discordPath,pi.dwProcessId)) p.discord=discord.Bootstrap();
	if (helper.Start(helperPath, pi.dwProcessId)) {
		p.helper = helper.Bootstrap();
	}
	else {
		p.helperError = helper.LastError();
		spdlog::warn("Networking helper unavailable (Win32 {}). Offline remains available.", p.helperError);
	}
	if (hSyncEvent != NULL) {
		if (!DuplicateHandle(GetCurrentProcess(), hSyncEvent, pi.hProcess, &p.hSyncEvent, 0, false, DUPLICATE_SAME_ACCESS)) {
			spdlog::warn("CreateSF4Process: DuplicateHandle() could not duplicate game sync handle, game may be unable to access Steam: err {}", GetLastError());
		}
	}
	if (!DetourCopyPayloadToProcess(pi.hProcess, sf4eSidecar::s_guidSidecarPayload, &p, sizeof(sf4e::Payload))) {
		StringCchPrintf(szErrorString, 1024, L"DetourCopyPayloadToProcess failed: %d", GetLastError());
		SecureZeroMemory(p.helper.nonce, sizeof(p.helper.nonce));
        SecureZeroMemory(p.discord.nonce, sizeof(p.discord.nonce));
		helper.Stop(0); discord.Stop(0);
		TerminateProcess(pi.hProcess, 9008);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (hSyncEvent) CloseHandle(hSyncEvent);
		return nullptr;
	}
	SecureZeroMemory(p.helper.nonce, sizeof(p.helper.nonce));
        SecureZeroMemory(p.discord.nonce, sizeof(p.discord.nonce));

	if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
		helper.Stop(0); discord.Stop(0);
		TerminateProcess(pi.hProcess, 9007);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (hSyncEvent) CloseHandle(hSyncEvent);
		return NULL;
	}
	spdlog::info("CreateSF4Process resumed pid={}", pi.dwProcessId);
	if (hSyncEvent != NULL) {
		HANDLE startupHandles[] = { hSyncEvent, pi.hProcess };
		DWORD lockWaitResult = WaitForMultipleObjects(2, startupHandles, FALSE, 60 * 1000);
		if (lockWaitResult == WAIT_OBJECT_0 + 1) {
			DWORD exitCode = 0;
			GetExitCodeProcess(pi.hProcess, &exitCode);
			spdlog::warn("Game exited before Sidecar startup completed (exit code {})", exitCode);
		}
		else if (lockWaitResult == WAIT_TIMEOUT) {
			spdlog::warn("Sidecar startup did not signal within 60 seconds");
		}
		else if (lockWaitResult == WAIT_FAILED) {
			spdlog::warn("Could not wait for Sidecar startup (Win32 {})", GetLastError());
		}
		else {
			spdlog::info("CreateSF4Process received Sidecar sync signal");
		}
		CloseHandle(hSyncEvent);
	}

	CloseHandle(pi.hThread);
	return pi.hProcess;
}

int UpdatePath(const wchar_t* const szLauncherDirW, wchar_t* const szErrorStringW, const int nErrorStringLen) {
	// Modify PATH to contain only the runtime DLL directory required by Sidecar.
	// Child processes inherit this environment, so keep the prefix narrow.
	// PATH can exceed 2K on developer machines; Windows allows up to 32767 chars.
	const DWORD kMaxEnv = 32767;
	DWORD nPathChars = GetEnvironmentVariableW(L"PATH", NULL, 0);
	DWORD res;

	if (nPathChars == 0) {
		DWORD err = GetLastError();
		if (err != ERROR_ENVVAR_NOT_FOUND) {
			spdlog::warn(L"UpdatePath: GetEnvironmentVariable(\"PATH\", ...) failed: {}", err);
			StringCchPrintfW(szErrorStringW, nErrorStringLen,
				L"Could not read PATH environment variable (error %lu).", err);
		}
		return 0;
	}

	wchar_t* szPathW = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nPathChars * sizeof(wchar_t));
	if (!szPathW) {
		StringCchPrintfW(szErrorStringW, nErrorStringLen, L"Out of memory while updating PATH.");
		return 0;
	}

	if (GetEnvironmentVariableW(L"PATH", szPathW, nPathChars) != nPathChars - 1) {
		DWORD err = GetLastError();
		HeapFree(GetProcessHeap(), 0, szPathW);
		StringCchPrintfW(szErrorStringW, nErrorStringLen,
			L"Could not read PATH environment variable (error %lu).", err);
		return 0;
	}

	size_t launcherLen = wcslen(szLauncherDirW);
	size_t newLen = (size_t)nPathChars + launcherLen + 2;
	if (newLen > kMaxEnv) {
		HeapFree(GetProcessHeap(), 0, szPathW);
		StringCchPrintfW(szErrorStringW, nErrorStringLen,
			L"PATH is too long to prepend the sf4e folder. Shorten your system PATH or launch from a shorter directory.");
		return 0;
	}

	wchar_t* szNewPathW = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newLen * sizeof(wchar_t));
	if (!szNewPathW) {
		HeapFree(GetProcessHeap(), 0, szPathW);
		StringCchPrintfW(szErrorStringW, nErrorStringLen, L"Out of memory while updating PATH.");
		return 0;
	}

	if ((res = StringCchPrintf(szNewPathW, newLen, TEXT("%s;%s"), szLauncherDirW, szPathW)) != S_OK) {
		StringCchPrintfW(szErrorStringW, nErrorStringLen,
			L"Could not create new PATH (error %lu).", res);
		HeapFree(GetProcessHeap(), 0, szPathW);
		HeapFree(GetProcessHeap(), 0, szNewPathW);
		return 0;
	}

	SetEnvironmentVariableW(L"PATH", szNewPathW);
	HeapFree(GetProcessHeap(), 0, szPathW);
	HeapFree(GetProcessHeap(), 0, szNewPathW);
	return 1;
}


// Sidecar's dependencies resolve from System32 before the package folder, so the
// installed VC++ runtime must be at least the toolset that built us. Older
// runtimes (below 14.40) crash on the first std::mutex lock inside the game.
bool RuntimeIsCurrent() {
	wchar_t path[MAX_PATH] = {};
	const UINT length = GetSystemDirectoryW(path, MAX_PATH);
	// An unreadable version is not evidence of an old runtime; do not block on it.
	if (!length || length >= MAX_PATH || FAILED(PathCchAppend(path, MAX_PATH, L"msvcp140.dll"))) return true;
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
	// Wine's own msvcp140 reports an older Microsoft number (14.42 in Wine 11)
	// but is a separate implementation: v0.9.7, built with 14.51, ran on it.
	// A Microsoft runtime installed into a Wine prefix is still checked.
	if (sf4e::platform::IsWineBuiltinDll(path)) return true;
	DWORD handle = 0;
	const DWORD size = GetFileVersionInfoSizeW(path, &handle);
	if (!size) return true;
	std::vector<BYTE> data(size);
	VS_FIXEDFILEINFO* info = nullptr;
	UINT infoSize = 0;
	if (!GetFileVersionInfoW(path, 0, size, data.data()) ||
		!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || !info) return true;
	const DWORD major = HIWORD(info->dwFileVersionMS), minor = LOWORD(info->dwFileVersionMS);
	const DWORD required = _MSC_VER - 1900;
	return major > 14 || (major == 14 && minor >= required);
}

// Shows a localized launcher message and returns the button pressed.
int ShowLauncherMessage(const char* key, UINT flags) {
	return MessageBoxW(nullptr, sf4e::platform::Utf8ToWide(sf4e::loc::T(key)).c_str(), L"SF4 Ember Netplay", flags);
}

// The recovery screen, with its selection art reporting to launcher.log.
bool ShowRecovery(std::string message, std::wstring& gameDirectory, bool updates = false) {
	return sf4e::ui::RunRecovery(std::move(message), gameDirectory, updates,
		[](const std::string& line) { spdlog::warn("{}", line); });
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    sf4e::install::ConfigureDllSearch();
    const auto languagePreference = sf4e::platform::LoadLanguagePreference();
    sf4e::loc::SetActive(sf4e::loc::ResolveLocale(languagePreference, sf4e::platform::WindowsUiLanguages()));
    // Before logging: spdlog, updates and recovery all lock a std::mutex, which
    // an old runtime crashes on, so nothing else can run until this passes.
    if (!RuntimeIsCurrent()) {
        if (ShowLauncherMessage("launcher.runtime_outdated", MB_YESNO | MB_ICONERROR) == IDYES)
            ShellExecuteW(nullptr, L"open", L"https://aka.ms/vc14/vc_redist.x86.exe", nullptr, nullptr, SW_SHOWNORMAL);
        return 1;
    }
    ConfigureLauncherLogging();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    sf4e::Payload payload{};
    bool offline = false, updates = false, recovery = false, updateError = false, discordLaunch = false;
    DWORD waitPid = 0;
    std::string localeOverride;
    CLI::App app("SF4 Ember Netplay for Ultra Street Fighter IV", "Launcher");
    app.add_flag("--discord-launch", discordLaunch, "Start Ember for an accepted Discord invitation.");
    app.add_flag("--console", payload.args.bShowConsole, "Show diagnostic logging.");
    app.add_flag("--offline", offline, "Start at the native game menu without networking.");
    app.add_flag("--updates", updates, "Open update and recovery controls.");
    app.add_flag("--recovery", recovery, "Open launch recovery controls.");
    app.add_flag("--update-error", updateError, "Show updater recovery after an installation failure.");
    app.add_option("--wait-pid", waitPid, "Wait for the current game to exit before opening update controls.");
    app.add_option("--locale", localeOverride, "Use a language for this launcher run only.");
    int argc = 0; auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try { app.parse(argc, argv); } catch (const CLI::ParseError& e) { LocalFree(argv); return app.exit(e); }
    LocalFree(argv);
    if (!localeOverride.empty() && sf4e::loc::ValidPreference(localeOverride))
        sf4e::loc::SetActive(sf4e::loc::ResolveLocale(localeOverride, sf4e::platform::WindowsUiLanguages()));
    sf4e::platform::LauncherInstance instance;
    std::wstring chosenDirectory;
    if (waitPid) {
        HANDLE oldGame = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
        if (oldGame) { WaitForSingleObject(oldGame, 30000); CloseHandle(oldGame); }
    }
    if (updates) { ShowRecovery(updateError ? sf4e::loc::T("launcher.update_failed") : "", chosenDirectory, true); return 0; }
    if (recovery && !ShowRecovery(sf4e::loc::T("launcher.recovery_title"), chosenDirectory)) return 0;
    if (!instance.Acquire()) {
        // A Discord invite reaches the running copy, so a second start for it
        // stays quiet. Otherwise a leftover launcher (or one still waiting on
        // a game that never closed) made every start do nothing at all.
        spdlog::warn("Another Ember launcher is already running; this start was not continued");
        if (!discordLaunch) ShowLauncherMessage("launcher.already_running", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    // An update interrupted mid-install must be restored before the game runs
    // on a half-replaced install. The Updater restarts the Launcher after.
    switch (sf4e::launcher::StartPendingUpdateRecovery(GetCurrentProcessId())) {
    case sf4e::launcher::PendingRecovery::Started: return 0;
    case sf4e::launcher::PendingRecovery::Failed:
        ShowRecovery(sf4e::loc::T("launcher.update_failed"), chosenDirectory, true);
        return 1;
    case sf4e::launcher::PendingRecovery::None: break;
    }
    wchar_t installRoot[MAX_PATH] = {}, dllDirectory[MAX_PATH] = {}, pathError[1024] = {};
    if (!sf4e::install::GetInstallRoot(installRoot, MAX_PATH) || !sf4e::install::GetPackageDllDirectory(dllDirectory, MAX_PATH) ||
        !UpdatePath(dllDirectory, pathError, 1024)) {
        ShowRecovery(sf4e::loc::T("launcher.path_failed"), chosenDirectory);
        return 1;
    }
    sf4e::launcher::PersistedSettings settings;
    sf4e::launcher::LoadPersistedSettings(settings);
    sf4e::launcher::EnsureUniqueDisplayName(settings);
    payload.netplay.mode = static_cast<int>(sf4e::NetplayMode::Idle);
    payload.netplay.version = sf4e::SF4E_NETPLAY_CONFIG_VERSION;
    strncpy_s(payload.netplay.displayName, settings.displayName, _TRUNCATE);
    payload.netplay.inputDelay = settings.inputDelay;
    payload.netplay.editionSelect = settings.editionSelect;
    payload.netplay.roundCount = settings.roundCount;
    payload.netplay.roundTimeIntegral = settings.roundTimeIntegral;
    payload.netplay.deviceIdx = payload.netplay.deviceType = 0xff;
    payload.netplay.useRelay = 0;
    SetEnvironmentVariableW(L"SF4E_START_OFFLINE", offline ? L"1" : nullptr);
    // A folder picked in recovery on an earlier launch comes before the search.
    sf4e::launcher::RememberedFolder remembered{sf4e::platform::Utf8ToWide(settings.gameDirectory.c_str())};
    const auto exists = [](const std::wstring& path) { return PathFileExistsW(path.c_str()) != FALSE; };
    for (;;) {
        const bool chosen = !chosenDirectory.empty();
        const std::wstring saved = remembered.Persisted();
        auto location = sf4e::launcher::LocateGame(chosenDirectory, saved, exists, [] {
            wchar_t directory[1024] = {}, executable[1024] = {};
            return FindSF4(directory,1024,executable,1024) != 0 ? std::wstring(directory) : std::wstring();
        });
        if (!chosen && !saved.empty()) {
            if (location.directory == saved) spdlog::info(L"Game directory from settings: {}", saved.c_str());
            else spdlog::warn(L"Game directory from settings has no SSFIV.exe, searching instead: {}", saved.c_str());
        }
        if (location.fromRecovery && !location.executable.empty()) {
            // Remember the picked folder so later launches do not ask again.
            const auto save = [](const std::wstring& folder) { const auto utf8 = sf4e::platform::WideToUtf8(folder); return !utf8.empty() && sf4e::launcher::SaveGameDirectory(utf8); };
            if (!remembered.Remember(location.directory, save)) spdlog::warn(L"Could not save game directory to settings: {}", location.directory.c_str());
            else if (remembered.Persisted() != saved) spdlog::info(L"Saved game directory to settings: {}", location.directory.c_str());
        }
        if (location.executable.empty()) {
            if (!ShowRecovery(sf4e::loc::T("launcher.game_not_found"),chosenDirectory)) return 0;
            continue;
        }
        wchar_t sidecar[MAX_PATH] = {};
        char sidecarAnsi[1024] = {};
        BOOL substituted = FALSE;
        if (!sf4e::install::ResolveInstallFile(L"Sidecar.dll",sidecar,MAX_PATH) ||
            !WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,sidecar,-1,sidecarAnsi,1024,nullptr,&substituted) || substituted) {
            if (!ShowRecovery(sf4e::loc::T("launcher.sidecar_missing"),chosenDirectory)) return 0;
            continue;
        }
        const char* dlls[] = {sidecarAnsi};
        CreateAppIDFile(location.directory.data());
        sf4e::platform::HelperProcess helper, discord;
        const auto helperPath = std::filesystem::path(installRoot)/L"sf4-net.exe";
        HANDLE game = CreateSF4Process(payload,helper,discord,helperPath.wstring(),location.directory.data(),location.executable.data(),1,dlls);
        if (!game) {
            if (!ShowRecovery(sf4e::loc::T("launcher.start_failed"),chosenDirectory)) return 0;
            continue;
        }
        WaitForSingleObject(game,INFINITE);
        DWORD exitCode = 0; GetExitCodeProcess(game,&exitCode);
        spdlog::info("Game exited with code {:#010x} ({})", exitCode, sf4e::crash::ExitCodeName(exitCode));
        discord.Stop(); helper.Stop(); CloseHandle(game);
        if (exitCode != 0 && ShowRecovery(sf4e::loc::T("launcher.game_error"),chosenDirectory)) continue;
        return 0;
    }
}
