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

#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include "../ui/RecoverySurface.hxx"
#include "../platform/LauncherInstance.hxx"

#include <CLI/CLI.hpp>
#include <detours/detours.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <vdf_parser.hpp>

#include "../sf4e/sf4e.hxx"
#include "../sidecar/sidecar.hxx"
#include "../common/sf4e__NetplayConfig.hxx"
#include "../common/install_paths.hxx"
#include "netplay/netplay_persist.hxx"
#include "update/github_release_client.hxx"

LPCWCH szGameFilename = L"SSFIV.exe";
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

	if ((res = PathCchCombine(szExePath, nExeSize, szGameDirectory, szGameFilename)) != S_OK) {
		spdlog::warn(L"FindSF4ByCurrentDirectory: PathCchCombine failed: {}", res);
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
	DWORD dwDataRead = 1024;
	LSTATUS lQueryStatus;
	wchar_t szLibraries[8][1024];
	int nLibrariesUsed = 1;
	wchar_t szLibraryFolderVDFPath[1024];
	HRESULT res = S_OK;

	// Capture SteamPath, which always acts as the first library
	lQueryStatus = RegGetValueW(
		HKEY_CURRENT_USER,
		L"Software\\Valve\\Steam",
		L"SteamPath",
		RRF_RT_REG_SZ,
		NULL,
		szLibraries[0],
		&dwDataRead
	);
	if (lQueryStatus != ERROR_SUCCESS) {
		spdlog::warn(L"FindSF4ByEstimatedSteamPath: Could not query registry for SteamPath: {}", lQueryStatus);
		return 0;
	}

	// Read the libary paths from `libraryfolders.vdf` file inside SteamPath
	if ((res = PathCchCombine(szLibraryFolderVDFPath, 1024, szLibraries[0], L"steamapps\\libraryfolders.vdf")) != S_OK) {
		spdlog::warn(L"FindSF4ByEstimatedSteamPath: szLibraryFolderVDFPath PathCchCombine failed: {}", res);
		return 0;
	}
	std::ifstream libraryFoldersFile(szLibraryFolderVDFPath);
	tyti::vdf::object libraryFoldersRoot = tyti::vdf::read(libraryFoldersFile);
	for (auto it = libraryFoldersRoot.childs.begin(); it != libraryFoldersRoot.childs.end(); ++it) {
		if (nLibrariesUsed >= 8) break;
		MultiByteToWideChar(
			CP_ACP,
			0,
			it->second->attribs["path"].c_str(),
			-1,
			szLibraries[nLibrariesUsed],
			1024
		);
		nLibrariesUsed++;
	}

	// Search the discovered libraries
	for (int i = 0; i < nLibrariesUsed; i++) {
		if (!PathIsDirectoryW(szLibraries[i])) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: detected library {} does not exist", szLibraries[i]);
			continue;
		}

		if ((res = PathCchCombine(szGameDirectory, nGameDirSize, szLibraries[i], szLibrarySuffix)) != S_OK) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: szGameDirectory PathCchCombine for {} failed: {}", szLibraries[i], res);
			continue;
		}

		if (!PathIsDirectoryW(szGameDirectory)) {
			// A common case- any given library may not contain SF4, so logging would
			// add more noise than signal.
			continue;
		}

		if ((res = PathCchCombine(szExePath, nExeSize, szGameDirectory, szGameFilename)) != S_OK) {
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


int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    sf4e::install::ConfigureDllSearch(); ConfigureLauncherLogging();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    sf4e::Payload payload{};
    bool offline = false, updates = false, recovery = false, updateError = false, discordLaunch = false;
    DWORD waitPid = 0;
    CLI::App app("SF4 Ember Netplay for Ultra Street Fighter IV", "Launcher");
    app.add_flag("--discord-launch", discordLaunch, "Start Ember for an accepted Discord invitation.");
    app.add_flag("--console", payload.args.bShowConsole, "Show diagnostic logging.");
    app.add_flag("--offline", offline, "Start at the native game menu without networking.");
    app.add_flag("--updates", updates, "Open update and recovery controls.");
    app.add_flag("--recovery", recovery, "Open launch recovery controls.");
    app.add_flag("--update-error", updateError, "Show updater recovery after an installation failure.");
    app.add_option("--wait-pid", waitPid, "Wait for the current game to exit before opening update controls.");
    int argc = 0; auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try { app.parse(argc, argv); } catch (const CLI::ParseError& e) { LocalFree(argv); return app.exit(e); }
    LocalFree(argv);
    sf4e::platform::LauncherInstance instance;
    std::wstring chosenDirectory;
    if (waitPid) {
        HANDLE oldGame = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
        if (oldGame) { WaitForSingleObject(oldGame, 30000); CloseHandle(oldGame); }
    }
    if (updates) { sf4e::ui::RunRecovery(updateError ? "The update could not be installed. Close the game and previous launcher before retrying. See %TEMP%\\sf4-netplay-update.log; preserved product copies are in .ember-update-backups." : "", chosenDirectory, true); return 0; }
    if (recovery && !sf4e::ui::RunRecovery("Launch recovery", chosenDirectory)) return 0;
    if (!instance.Acquire()) return 0;
    wchar_t installRoot[MAX_PATH] = {}, dllDirectory[MAX_PATH] = {}, pathError[1024] = {};
    if (!sf4e::install::GetInstallRoot(installRoot, MAX_PATH) || !sf4e::install::GetPackageDllDirectory(dllDirectory, MAX_PATH) ||
        !UpdatePath(dllDirectory, pathError, 1024)) {
        sf4e::ui::RunRecovery("The runtime search path could not be configured. Extract the complete package to a writable folder.", chosenDirectory);
        return 1;
    }
    sf4e::launcher::PersistedSettings settings;
    sf4e::launcher::LoadPersistedSettings(settings);
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
    for (;;) {
        wchar_t directory[1024] = {}, executable[1024] = {};
        bool found = false;
        if (!chosenDirectory.empty()) {
            StringCchCopyW(directory,1024,chosenDirectory.c_str());
            PathCchCombine(executable,1024,directory,L"SSFIV.exe"); found = PathFileExistsW(executable) != FALSE;
        } else found = FindSF4(directory,1024,executable,1024) != 0;
        if (!found) {
            if (!sf4e::ui::RunRecovery("Ultra Street Fighter IV could not be found.",chosenDirectory)) return 0;
            continue;
        }
        wchar_t sidecar[MAX_PATH] = {};
        char sidecarAnsi[1024] = {};
        BOOL substituted = FALSE;
        if (!sf4e::install::ResolveInstallFile(L"Sidecar.dll",sidecar,MAX_PATH) ||
            !WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,sidecar,-1,sidecarAnsi,1024,nullptr,&substituted) || substituted) {
            if (!sf4e::ui::RunRecovery("Sidecar.dll is missing or its path cannot be used by the injector. Extract the full package to a simple local path.",chosenDirectory)) return 0;
            continue;
        }
        const char* dlls[] = {sidecarAnsi};
        CreateAppIDFile(directory);
        sf4e::platform::HelperProcess helper, discord;
        const auto helperPath = std::filesystem::path(installRoot)/L"sf4-net.exe";
        HANDLE game = CreateSF4Process(payload,helper,discord,helperPath.wstring(),directory,executable,1,dlls);
        if (!game) {
            if (!sf4e::ui::RunRecovery("Game startup or injection failed. Check the launcher log, then retry.",chosenDirectory)) return 0;
            continue;
        }
        WaitForSingleObject(game,INFINITE);
        DWORD exitCode = 0; GetExitCodeProcess(game,&exitCode);
        discord.Stop(); helper.Stop(); CloseHandle(game);
        if (exitCode != 0 && sf4e::ui::RunRecovery("The game exited with an error. Inspect the launcher log, then retry.",chosenDirectory)) continue;
        return 0;
    }
}
