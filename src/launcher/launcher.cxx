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
#include "netplay/netplay_wizard.hxx"
#include "netplay/netplay_launch_controller.hxx"
#include "relay/relay_host_spawn.hxx"
#include "update/github_release_client.hxx"
#include "ipc/electron_ipc.hxx"

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
		MessageBox(NULL, szErrorString, NULL, MB_OK);
		MessageBox(NULL, szGameDirectory, NULL, MB_OK);
		MessageBox(NULL, szExePath, NULL, MB_OK);
		for (int i = 0; i < nDlls; i++) {
			MessageBoxA(NULL, rlpDlls[i], NULL, MB_OK);
		}
		if (dwError == ERROR_INVALID_HANDLE) {
			MessageBox(NULL, L"Can't detour a 64-bit target process from a 32-bit parent process or vice versa.", NULL, MB_OK);
		}
		ExitProcess(9009);
	}

	sf4e::Payload p = payload;
	if (hSyncEvent != NULL) {
		if (!DuplicateHandle(GetCurrentProcess(), hSyncEvent, pi.hProcess, &p.hSyncEvent, 0, false, DUPLICATE_SAME_ACCESS)) {
			spdlog::warn("CreateSF4Process: DuplicateHandle() could not duplicate game sync handle, game may be unable to access Steam: err {}", GetLastError());
		}
	}
	if (!DetourCopyPayloadToProcess(pi.hProcess, sf4eSidecar::s_guidSidecarPayload, &p, sizeof(sf4e::Payload))) {
		StringCchPrintf(szErrorString, 1024, L"DetourCopyPayloadToProcess failed: %d", GetLastError());
		MessageBox(NULL, szErrorString, NULL, MB_OK);
		ExitProcess(9008);
	}

	ResumeThread(pi.hThread);
	spdlog::info("CreateSF4Process resumed pid={}", pi.dwProcessId);
	if (hSyncEvent != NULL) {
		DWORD lockWaitResult = WaitForSingleObject(hSyncEvent, 60 * 1000);
		if (lockWaitResult != 0) {
			spdlog::warn("CreateSF4Process: WaitForSingleObject() could not wait for game sync handle, game may be unable to access Steam: err {}", GetLastError());
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
			MessageBoxW(NULL, szErrorStringW, L"sf4e", MB_OK | MB_ICONERROR);
		}
		return 0;
	}

	wchar_t* szPathW = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nPathChars * sizeof(wchar_t));
	if (!szPathW) {
		StringCchPrintfW(szErrorStringW, nErrorStringLen, L"Out of memory while updating PATH.");
		MessageBoxW(NULL, szErrorStringW, L"sf4e", MB_OK | MB_ICONERROR);
		return 0;
	}

	if (GetEnvironmentVariableW(L"PATH", szPathW, nPathChars) != nPathChars - 1) {
		DWORD err = GetLastError();
		HeapFree(GetProcessHeap(), 0, szPathW);
		StringCchPrintfW(szErrorStringW, nErrorStringLen,
			L"Could not read PATH environment variable (error %lu).", err);
		MessageBoxW(NULL, szErrorStringW, L"sf4e", MB_OK | MB_ICONERROR);
		return 0;
	}

	size_t launcherLen = wcslen(szLauncherDirW);
	size_t newLen = (size_t)nPathChars + launcherLen + 2;
	if (newLen > kMaxEnv) {
		HeapFree(GetProcessHeap(), 0, szPathW);
		StringCchPrintfW(szErrorStringW, nErrorStringLen,
			L"PATH is too long to prepend the sf4e folder. Shorten your system PATH or launch from a shorter directory.");
		MessageBoxW(NULL, szErrorStringW, L"sf4e", MB_OK | MB_ICONERROR);
		return 0;
	}

	wchar_t* szNewPathW = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newLen * sizeof(wchar_t));
	if (!szNewPathW) {
		HeapFree(GetProcessHeap(), 0, szPathW);
		StringCchPrintfW(szErrorStringW, nErrorStringLen, L"Out of memory while updating PATH.");
		MessageBoxW(NULL, szErrorStringW, L"sf4e", MB_OK | MB_ICONERROR);
		return 0;
	}

	if ((res = StringCchPrintf(szNewPathW, newLen, TEXT("%s;%s"), szLauncherDirW, szPathW)) != S_OK) {
		StringCchPrintfW(szErrorStringW, nErrorStringLen,
			L"Could not create new PATH (error %lu).", res);
		MessageBoxW(NULL, szErrorStringW, L"sf4e", MB_OK | MB_ICONERROR);
		HeapFree(GetProcessHeap(), 0, szPathW);
		HeapFree(GetProcessHeap(), 0, szNewPathW);
		return 0;
	}

	SetEnvironmentVariableW(L"PATH", szNewPathW);
	HeapFree(GetProcessHeap(), 0, szPathW);
	HeapFree(GetProcessHeap(), 0, szNewPathW);
	return 1;
}

int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nShowCmd
) {
	sf4e::install::ConfigureDllSearch();
	ConfigureLauncherLogging();
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	HRESULT res = S_OK;
	wchar_t szErrorStringW[4096] = { 0 };
	wchar_t szLauncherDirW[1024] = { 0 };
	wchar_t szGameDirectory[1024] = { 0 };
	wchar_t szExePath[1024] = { 0 };
	char szLauncherDirA[1024] = { 0 };
	char szSidecarDllPathA[1024] = { 0 };
	int nDlls = 1;
	const char* dlls[1] = {
		szSidecarDllPathA,
	};

	sf4e::Payload payload = { 0 };
	CLI::App app("A process-inspection and modification tool for the Steam release of Ultra Street Fighter 4.", "sf4e");
	app.add_flag("--console", payload.args.bShowConsole, "Show a console with live logging. The console may interfere with inputs to the main window.");
	bool offlineOnly = false;
	app.add_flag("--offline", offlineOnly, "Launch without netplay setup.");
	bool hostOnly = false;
	app.add_flag("--host", hostOnly, "Host a netplay session (skip wizard).");
	std::string joinRoomCode;
	app.add_option("--join", joinRoomCode, "Join host at IP:port (skip wizard).")->expected(1);
	bool devOverlay = false;
	app.add_flag("--dev-overlay", devOverlay, "Enable developer netplay overlay in-game.");
	bool applyUpdateOnly = false;
	app.add_flag("--apply-update", applyUpdateOnly, "Download and install the latest GitHub release, then exit.");
	bool electronIpc = false;
	app.add_flag("--electron-ipc", electronIpc, "JSON-lines UI bridge on stdin/stdout (automation / legacy Electron shell).");
	bool headlessHandshake = false;
	app.add_flag("--headless-test-handshake", headlessHandshake, "Same as --electron-ipc (CI smoke test).");
	int argc;
	LPWSTR* argv = CommandLineToArgvW(
		// Intentionally do _not_ use lpCmdLine here. Windows removes
		// the path or name of the program from the start of lpCmdLine,
		// so if it were parsed with `CommandLineToArgvW`, argv[0]
		// would be the first argument. This isn't standards-compatible-
		// in pretty much every other context, argv[0] is a path to or
		// name of the program invoked, and CLI11 assumes that standard.
		// Once passed to CLI11, parsing the nonstandard argv array
		// would effectively ignore the first CLI option.
		//
		// Instead, use the raw command line.
		GetCommandLineW(),
		&argc
	);
	CLI11_PARSE(app, argc, argv);

	// sf4e://join/SF4-XXXX from OS URI handler
	for (int i = 1; i < argc; ++i) {
		if (!argv[i]) {
			continue;
		}
		std::wstring arg = argv[i];
		const wchar_t* prefix = L"sf4e://join/";
		size_t plen = wcslen(prefix);
		if (arg.size() > plen && _wcsnicmp(arg.c_str(), prefix, plen) == 0) {
			char narrow[64] = { 0 };
			WideCharToMultiByte(CP_UTF8, 0, arg.c_str() + plen, -1, narrow, sizeof(narrow), NULL, NULL);
			joinRoomCode = narrow;
			break;
		}
	}
	spdlog::info(
		"Launcher args offline={} host={} join={} electronIpc={} headlessHandshake={} devOverlay={} applyUpdate={}",
		offlineOnly,
		hostOnly,
		!joinRoomCode.empty(),
		electronIpc,
		headlessHandshake,
		devOverlay,
		applyUpdateOnly
	);

	// Compute the path to the sidecar DLL based on the launcher's directory.
	// Ideally, this wouldn't have to convert from wide-char to multibyte in
	// the system's codepage, but Detours uses multibyte paths when injecting
	// DLLs.
	GetModuleFileNameW(NULL, szLauncherDirW, 1024);
	PathCchRemoveFileSpec(szLauncherDirW, 1024);
	WideCharToMultiByte(CP_ACP, 0, szLauncherDirW, 1024, szLauncherDirA, 1024, NULL, NULL);
	{
		wchar_t installRoot[MAX_PATH] = { 0 };
		wchar_t dllRoot[MAX_PATH] = { 0 };
		sf4e::install::GetInstallRoot(installRoot, MAX_PATH);
		sf4e::install::GetPackageDllDirectory(dllRoot, MAX_PATH);
		spdlog::info(L"Launcher paths exeDir={} installRoot={} dllRoot={}", szLauncherDirW, installRoot, dllRoot);
	}
	{
		wchar_t sidecarPathW[1024] = { 0 };
		if (!sf4e::install::ResolveInstallFile(L"Sidecar.dll", sidecarPathW, 1024)) {
			PathCombineW(sidecarPathW, szLauncherDirW, L"Sidecar.dll");
			PathCombineA(szSidecarDllPathA, szLauncherDirA, "Sidecar.dll");
		} else {
			WideCharToMultiByte(CP_ACP, 0, sidecarPathW, -1, szSidecarDllPathA, 1024, NULL, NULL);
		}
		spdlog::info(L"Launcher sidecar path {}", sidecarPathW);
	}

	{
		wchar_t pathForGame[MAX_PATH * 2] = { 0 };
		if (sf4e::install::UsesDllSubdirectory()) {
			wchar_t dllDir[MAX_PATH] = { 0 };
			if (sf4e::install::GetPackageDllDirectory(dllDir, MAX_PATH)) {
				if (FAILED(StringCchCopyW(pathForGame, MAX_PATH * 2, dllDir))) {
					pathForGame[0] = 0;
				}
			}
		}
		const wchar_t* pathTarget = pathForGame[0] ? pathForGame : szLauncherDirW;
		if (!UpdatePath(pathTarget, szErrorStringW, 4096)) {
			return 1;
		}
		spdlog::info(L"Launcher updated child PATH prefix {}", pathTarget);
	}

	if (applyUpdateOnly) {
		sf4e::launcher::UpdateCheckResult check = sf4e::launcher::CheckForUpdate();
		if (!check.ok) {
			MessageBoxA(NULL, check.error.c_str(), "sf4e update", MB_OK | MB_ICONERROR);
			return 1;
		}
		if (!check.updateAvailable) {
			MessageBoxA(NULL, "No update available.", "sf4e update", MB_OK | MB_ICONINFORMATION);
			return 0;
		}
		sf4e::launcher::ApplyUpdateResult applied = sf4e::launcher::DownloadAndApplyUpdate(
			check.zipDownloadUrl.c_str(),
			check.zipApiUrl.c_str(),
			check.latestVersion.c_str()
		);
		if (!applied.ok) {
			MessageBoxA(NULL, applied.error.c_str(), "sf4e update", MB_OK | MB_ICONERROR);
			return 1;
		}
		return 0;
	}

	if (!FindSF4(szGameDirectory, 1024, szExePath, 1024)) {
		MessageBoxW(NULL,
			L"Cannot find Ultra Street Fighter IV (SSFIV.exe).\n\n"
			L"Install USF4 on Steam, or set STEAM_APP_PATH to the game folder.\n"
			L"Run Launcher.exe --console for logs (%APPDATA%\\sf4e\\).",
			L"sf4e", MB_OK | MB_ICONWARNING);
		return 1;
	}
	spdlog::info(L"Launcher resolved game dir={} exe={}", szGameDirectory, szExePath);

	sf4e::launcher::PersistedSettings settings;
	sf4e::launcher::LoadPersistedSettings(settings);
	if (devOverlay) {
		payload.netplay.devOverlay = 1;
	}

	if (offlineOnly) {
		payload.netplay.version = sf4e::SF4E_NETPLAY_CONFIG_VERSION;
		payload.netplay.mode = (int)sf4e::NetplayMode::Idle;
		strncpy_s(payload.netplay.displayName, settings.displayName, _TRUNCATE);
		payload.netplay.inputDelay = settings.inputDelay;
		payload.netplay.sessionPort = settings.sessionPort;
		payload.netplay.ggpoPort = settings.ggpoPort;
		payload.netplay.editionSelect = settings.editionSelect;
		payload.netplay.roundCount = settings.roundCount;
		payload.netplay.roundTimeIntegral = settings.roundTimeIntegral;
		payload.netplay.deviceIdx = 0xff;
		payload.netplay.deviceType = 0xff;
		spdlog::info("Launcher selected CLI offline mode");
	}
	else if (hostOnly && joinRoomCode.empty()) {
		if (!sf4e::launcher::ApplyNetplayConfigFromWizard(payload.netplay, settings, (int)sf4e::NetplayMode::Host, nullptr)) {
			MessageBoxW(NULL, L"Failed to configure host mode.", L"sf4e", MB_OK | MB_ICONWARNING);
			return 1;
		}
		sf4e::launcher::SavePersistedSettings(settings);
		spdlog::info("Launcher selected CLI host mode");
	}
	else if (!joinRoomCode.empty()) {
		if (!sf4e::launcher::ApplyNetplayConfigFromWizard(
			payload.netplay,
			settings,
			(int)sf4e::NetplayMode::Join,
			joinRoomCode.c_str()
		)) {
			MessageBoxW(
				NULL,
				L"Invalid --join address. Use SF4-XXXX or IP:port (example 203.0.113.42:23456).",
				L"sf4e",
				MB_OK | MB_ICONWARNING
			);
			return 1;
		}
		sf4e::launcher::SavePersistedSettings(settings);
		spdlog::info("Launcher selected CLI join mode");
	}
	else if (electronIpc || headlessHandshake) {
		sf4e::launcher::NetplayLaunchController controller(settings, payload.netplay);
		spdlog::info("Launcher entering controller IPC mode");
		if (!sf4e::launcher::RunElectronIpcBridge(controller)) {
			return 0;
		}
		sf4e::launcher::SavePersistedSettings(settings);
		spdlog::info("Launcher controller IPC completed mode={} devOverlay={}", payload.netplay.mode, (int)payload.netplay.devOverlay);
	}
	else {
		spdlog::info("Launcher entering Qt/wizard UI mode");
		if (!sf4e::launcher::RunNetplayWizard(NULL, payload.netplay, settings)) {
			return 0;
		}
		sf4e::launcher::SavePersistedSettings(settings);
		spdlog::info("Launcher UI completed mode={} devOverlay={}", payload.netplay.mode, (int)payload.netplay.devOverlay);
	}

	CreateAppIDFile(szGameDirectory);
	spdlog::info("Launcher starting game with injected Sidecar mode={} inputDelay={} devOverlay={}",
		payload.netplay.mode,
		(int)payload.netplay.inputDelay,
		(int)payload.netplay.devOverlay
	);
	HANDLE hGame = CreateSF4Process(payload, szGameDirectory, szExePath, nDlls, dlls);
	if (sf4e::launcher::GetRelayHostPid() != 0) {
		spdlog::info(
			"Launcher supervising game until exit (RelayHost pid {})",
			sf4e::launcher::GetRelayHostPid()
		);
		WaitForSingleObject(hGame, INFINITE);
		sf4e::launcher::StopRelayHost();
		spdlog::info("Launcher stopped RelayHost after game exit");
	}
	CloseHandle(hGame);
	return 0;
}
