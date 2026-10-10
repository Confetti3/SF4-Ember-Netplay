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
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include "../ui/RecoverySurface.hxx"
#include "../platform/JoinLinkMailbox.hxx"
#include "../common/WipeText.hxx"
#include "../platform/Elevation.hxx"
#include "../platform/LauncherInstance.hxx"
#include "../platform/Utf8.hxx"
#include "../platform/VideoLinkServe.hxx"
#include "../platform/WineBuiltin.hxx"

#include <CLI/CLI.hpp>
#include <detours/detours.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "../sf4e/sf4e.hxx"
#include "../sidecar/sidecar.hxx"
#include "../common/CrashDump.hxx"
#include "../common/CrashReport.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../common/ReplayLink.hxx"
#include "../platform/ReplayPath.hxx"
#include "../common/sf4e__NetplayConfig.hxx"
#include "../common/install_paths.hxx"
#include "../common/Localization.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include "GameLocator.hxx"
#include "netplay/netplay_persist.hxx"
#include "update/github_release_client.hxx"
#include "BuildIdentity.hxx"


// Where the launcher and the game both log; the crash dump goes here too.
wchar_t g_logsDir[MAX_PATH] = { 0 };

void ConfigureLauncherLogging() {
	PWSTR appData = NULL;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appData) != S_OK) {
		return;
	}

	wchar_t sf4eDir[MAX_PATH] = { 0 };
	wchar_t* logsDir = g_logsDir;
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
					// A field log names the build it came from.
					spdlog::info("Launcher build: version={} revision={}", SF4E_APP_VERSION, SF4E_SOURCE_REVISION);
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

		// Steam's manifest names the install folder, which need not be the
		// usual one; the usual folder is still tried after it.
		std::string manifest;
		std::ifstream manifestFile(std::filesystem::path(library) / L"steamapps" / L"appmanifest_45760.acf", std::ios::binary);
		if (manifestFile.is_open()) manifest.assign(std::istreambuf_iterator<char>(manifestFile), std::istreambuf_iterator<char>());
		for (const std::wstring& folder : sf4e::launcher::GameFoldersInLibrary(library, manifest)) {
			// Most libraries do not hold SF4, so a missing folder is not logged.
			if (!PathIsDirectoryW(folder.c_str())) continue;
			if (wcscpy_s(szGameDirectory, nGameDirSize, folder.c_str()) != 0) {
				spdlog::warn(L"FindSF4ByEstimatedSteamPath: game folder {} is too long", folder);
				continue;
			}
			if ((res = PathCchCombine(szExePath, nExeSize, szGameDirectory, sf4e::launcher::kGameExecutableName)) != S_OK) {
				spdlog::warn(L"FindSF4ByEstimatedSteamPath: szExePath PathCchCombine failed: {}", res);
				continue;
			}
			if (PathFileExistsW(szExePath)) {
				spdlog::info(L"FindSF4ByEstimatedSteamPath: found the game in {}", folder);
				return 1;
			}
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

// A fail-fast (0xC0000409) skips every handler in the game. Windows Error
// Reporting still writes a dump when LocalDumps is set up for SSFIV.exe
// (docs/guides/USER_NETPLAY.md); name it so it reaches the report.
void NoteWindowsCrashDump(DWORD processId) {
	PWSTR localAppData = NULL;
	if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData) != S_OK) return;
	wchar_t name[64] = {}, path[MAX_PATH] = {};
	StringCchPrintfW(name, 64, L"CrashDumps\\SSFIV.exe.%lu.dmp", processId);
	const bool named = SUCCEEDED(PathCchCombine(path, MAX_PATH, localAppData, name));
	CoTaskMemFree(localAppData);
	if (named && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) spdlog::info(L"Windows wrote a crash dump to {}", path);
	else spdlog::info("No crash dump from Sidecar or Windows Error Reporting; see USER_NETPLAY.md to turn on LocalDumps");
}

// Copies every match replay into Ember's archive (platform/ReplayFiles.hxx)
// so the game's slots are not the only copy. Never blocks a start.
void ArchiveReplays() {
	static bool noted = false;
	if (sf4e::platform::replays::Archive() < 0 && !noted) { noted = true; spdlog::info("Replays: no Steam path or settings folder, nothing archived"); }
}

HANDLE CreateSF4Process(
	const sf4e::Payload& payload,
	sf4e::platform::HelperProcess& helper,
    sf4e::platform::HelperProcess& discord,
	const std::wstring& helperPath,
	LPWSTR szGameDirectory,
	LPWSTR szExePath,
	int nDlls,
	LPCSTR* rlpDlls,
	sf4e::crash::DumpChannel& dumps,
	DWORD& startError
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
		startError = dwError;
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
	// Without the channel the game writes its own crash dump.
	if (dumps.view && !(
		DuplicateHandle(GetCurrentProcess(), dumps.request, pi.hProcess, &p.hDumpRequest, 0, false, DUPLICATE_SAME_ACCESS) &&
		DuplicateHandle(GetCurrentProcess(), dumps.done, pi.hProcess, &p.hDumpDone, 0, false, DUPLICATE_SAME_ACCESS) &&
		DuplicateHandle(GetCurrentProcess(), dumps.mailbox, pi.hProcess, &p.hDumpMailbox, 0, false, DUPLICATE_SAME_ACCESS))) {
		spdlog::warn("CreateSF4Process: could not hand the crash dump channel to the game (Win32 {})", GetLastError());
		p.hDumpRequest = p.hDumpDone = p.hDumpMailbox = NULL;
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

// The recovery screen, with its selection art reporting to launcher.log. The
// updater offers to start the game when canStart says the launch may go on.
// `crash` offers the crash's report from the message (ui/RecoverySurface.hxx).
bool ShowRecovery(std::string message, std::wstring& gameDirectory, bool updates = false,
	sf4e::ui::Tone tone = sf4e::ui::Tone::Error, bool canStart = false,
	const std::optional<sf4e::reports::CrashContext>& crash = std::nullopt) {
	return sf4e::ui::RunRecovery(std::move(message), gameDirectory, updates,
		[](const std::string& line) { spdlog::warn("{}", line); }, tone, canStart, crash);
}

// Lets the /j page on embernetplay.link hand a room link to Ember through
// ember://join/<code>. Per user (HKCU), no elevation; written only when the
// command does not already name this Launcher.exe. A wrong or missing entry
// costs nothing: the page always offers the link to copy and paste instead.
static void RegisterJoinScheme() {
    wchar_t executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    const std::wstring command = L"\"" + std::wstring(executable) + L"\" --join-link \"%1\"";
    const wchar_t* commandKey = L"Software\\Classes\\ember\\shell\\open\\command";
    wchar_t current[2 * MAX_PATH] = {};
    DWORD size = sizeof(current);
    if (RegGetValueW(HKEY_CURRENT_USER, commandKey, nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS &&
        command == current) return;
    const auto set = [](const wchar_t* key, const wchar_t* name, const std::wstring& value) {
        return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value.c_str(),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    const bool registered = set(L"Software\\Classes\\ember", nullptr, L"URL:SF4 Ember Netplay room link") &&
        set(L"Software\\Classes\\ember", L"URL Protocol", L"") &&
        set(L"Software\\Classes\\ember\\DefaultIcon", nullptr, L"\"" + std::wstring(executable) + L"\",0") &&
        set(commandKey, nullptr, command);
    if (registered) spdlog::info("Registered the ember: room link handler");
    else spdlog::warn("Could not register the ember: room link handler");
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    sf4e::install::ConfigureDllSearch();
    auto languagePreference = sf4e::platform::LoadLanguagePreference();
    // Until the game folder is found, "auto" can only follow Windows.
    sf4e::loc::SetActive(sf4e::platform::ResolveUiLocale(languagePreference));
    // Before logging: spdlog, updates and recovery all lock a std::mutex, which
    // an old runtime crashes on, so nothing else can run until this passes.
    if (!RuntimeIsCurrent()) {
        if (ShowLauncherMessage("launcher.runtime_outdated", MB_YESNO | MB_ICONERROR) == IDYES)
            ShellExecuteW(nullptr, L"open", L"https://aka.ms/vc14/vc_redist.x86.exe", nullptr, nullptr, SW_SHOWNORMAL);
        return 1;
    }
    {
        // Not a launcher start: the game's video export runs its encoder in
        // this executable (platform/VideoLinkServe.hxx). Before the log, which rotates.
        int count = 0; auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        const std::wstring link = count == 3 && !wcscmp(arguments[1], L"--encode-video") ? arguments[2] : L"";
        LocalFree(arguments);
        if (!link.empty()) return sf4e::platform::videolink::Serve(link);
    }
    ConfigureLauncherLogging();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    sf4e::Payload payload{};
    bool offline = false, updates = false, recovery = false, updateError = false, discordLaunch = false;
    DWORD waitPid = 0;
    std::string localeOverride, joinUri;
    CLI::App app("SF4 Ember Netplay for Ultra Street Fighter IV", "Launcher");
    app.add_option("--join-link", joinUri, "Open a room, tournament match, public room or Discord connect link from the browser (ember://join/..., ember://tournament/open?..., ember://room/open?... or ember://discord/connect?...).");
    app.add_flag("--discord-launch", discordLaunch, "Start Ember for an accepted Discord invitation.");
    app.add_flag("--console", payload.args.bShowConsole, "Show diagnostic logging.");
    app.add_flag("--offline", offline, "Start at the native game menu without networking.");
    app.add_flag("--updates", updates, "Open update and recovery controls.");
    app.add_flag("--recovery", recovery, "Open launch recovery controls.");
    app.add_flag("--update-error", updateError, "Show updater recovery after an installation failure.");
    app.add_option("--wait-pid", waitPid, "Wait for the current game to exit before opening update controls.");
    app.add_option("--locale", localeOverride, "Use a language for this launcher run only.");
    // Steam's launch options put the game's own command after the launcher
    // ("Launcher.exe" %command%), arguments included; Ember starts the game
    // itself, so all of it is unused. An unknown option before it is still an error.
    app.prefix_command();
    int argc = 0; auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try { app.parse(argc, argv); } catch (const CLI::ParseError& e) { LocalFree(argv); return app.exit(e); }
    LocalFree(argv);
    const auto steamCommand = app.remaining();
    if (!steamCommand.empty() && steamCommand.front().rfind("-", 0) == 0) return app.exit(CLI::ExtrasError(steamCommand));
    // Steam Input applies only when Steam starts the launcher, which then has
    // Steam's overlay module; sf4e.log's HID list shows whether it took effect.
    spdlog::info("Started {} Steam{}", GetModuleHandleW(L"GameOverlayRenderer.dll") ? "by" : "outside",
        steamCommand.empty() ? "" : " with the game command");
    if (!localeOverride.empty() && sf4e::loc::ValidPreference(localeOverride)) {
        languagePreference = localeOverride;
        sf4e::loc::SetActive(sf4e::platform::ResolveUiLocale(languagePreference));
    }
    // The link is a room's short code, never logged, a tournament match, a
    // public room of a service (its IDs are not logged either), or a
    // tournament service asking the player to connect Discord.
    const std::string joinCode = joinUri.empty() ? std::string() : sf4e::join_link::ParseUri(joinUri);
    const auto matchLink = joinUri.empty() || !joinCode.empty() ? sf4e::tournament_link::MatchLink() :
        sf4e::tournament_link::ParseLink(joinUri);
    const auto publicRoomLink = joinUri.empty() || !joinCode.empty() || matchLink.Valid() ? sf4e::tournament_link::RoomLink() :
        sf4e::tournament_link::ParseRoomLink(joinUri);
    const std::string connectBridge = joinUri.empty() || !joinCode.empty() || matchLink.Valid() || publicRoomLink.Valid() ? std::string() :
        sf4e::tournament_link::ParseConnectLink(joinUri);
    // A replay link names an archived replay to play (common/ReplayLink.hxx).
    const std::string replayLink = joinUri.empty() || !joinCode.empty() || matchLink.Valid() || publicRoomLink.Valid() || !connectBridge.empty() ? std::string() :
        sf4e::platform::ResolveReplayFile(sf4e::replay_link::ParseReplayLink(joinUri));
    if (!joinCode.empty()) spdlog::info("Started with a room link");
    else if (matchLink.Valid()) spdlog::info("Started with a link to tournament match {}", matchLink.matchId);
    else if (publicRoomLink.Valid()) spdlog::info("Started with a public room link");
    else if (!connectBridge.empty()) spdlog::info("Started with a link to connect Discord on service {}", connectBridge);
    else if (!replayLink.empty()) spdlog::info("Started with a link to a replay");
    else if (!joinUri.empty()) spdlog::info("Ignored a link that is not an Ember room, tournament match, public room, Discord connect or replay link");
    sf4e::WipeText(joinUri);
    sf4e::platform::LauncherInstance instance;
    std::wstring chosenDirectory;
    if (waitPid) {
        HANDLE oldGame = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
        if (oldGame) { WaitForSingleObject(oldGame, 30000); CloseHandle(oldGame); }
    }
    // Declining an update, or a failed install, leaves the game as it was, so
    // the updater can go on to start it; only Start continues past here.
    if (updates && !ShowRecovery(updateError ? sf4e::loc::T("launcher.update_failed") : "", chosenDirectory, true, sf4e::ui::Tone::Error, true)) return 0;
    if (recovery && !ShowRecovery(sf4e::loc::T("launcher.recovery_opened"), chosenDirectory, false, sf4e::ui::Tone::Neutral)) return 0;
    if (!instance.Acquire()) {
        // A room link goes to the running game, which shows it on its Join
        // screen for the player to confirm.
        if (!joinCode.empty() && sf4e::platform::DeliverJoinLink(joinCode)) {
            spdlog::info("Handed the room link to the running game");
            return 0;
        }
        // A match link goes to the running game too, which opens the match's
        // row for the player to press Play; a game in progress is never interrupted.
        if (matchLink.Valid() && sf4e::platform::DeliverMatchLink(matchLink)) {
            spdlog::info("Handed the tournament match link to the running game");
            return 0;
        }
        // A public room link goes to the running game too, which asks for a
        // ticket for that room only when the player is free; a room or game
        // in progress is never interrupted.
        if (publicRoomLink.Valid() && sf4e::platform::DeliverPublicRoomLink(publicRoomLink)) {
            spdlog::info("Handed the public room link to the running game");
            return 0;
        }
        // A connect link opens the running game's Connect Discord screen,
        // where the player decides.
        if (!connectBridge.empty() && sf4e::platform::DeliverConnectLink(connectBridge)) {
            spdlog::info("Handed the Discord connect link to the running game");
            return 0;
        }
        // A replay link goes to the running game, which plays it from its main menu.
        if (!replayLink.empty() && sf4e::platform::DeliverReplayLink(replayLink)) {
            spdlog::info("Handed the replay link to the running game");
            return 0;
        }
        // A Discord invite reaches the running copy, so a second start for it
        // stays quiet. Otherwise a leftover launcher (or one still waiting on
        // a game that never closed) made every start do nothing at all.
        spdlog::warn("Another Ember launcher is already running; this start was not continued");
        if (!discordLaunch) ShowLauncherMessage("launcher.already_running", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    RegisterJoinScheme();
    // An update interrupted mid-install must be restored before the game runs
    // on a half-replaced install. The Updater restarts the Launcher after.
    switch (sf4e::launcher::StartPendingUpdateRecovery(GetCurrentProcessId())) {
    case sf4e::launcher::PendingRecovery::Started: return 0;
    case sf4e::launcher::PendingRecovery::Failed:
        ShowRecovery(sf4e::loc::T("launcher.update_failed"), chosenDirectory, true);
        return 1;
    case sf4e::launcher::PendingRecovery::NotNormalUser:
        ShowRecovery(sf4e::loc::T("update.elevated"), chosenDirectory, true);
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
    // The game reads the room link once at start and clears it (NetplayRuntime).
    SetEnvironmentVariableW(L"SF4E_JOIN_LINK", joinCode.empty() ? nullptr : sf4e::platform::Utf8ToWide(joinCode.c_str()).c_str());
    // A match link likewise, as "<bridge id> <match id>".
    SetEnvironmentVariableW(L"SF4E_MATCH_LINK", !matchLink.Valid() ? nullptr :
        sf4e::platform::Utf8ToWide((matchLink.bridgeId + " " + matchLink.matchId).c_str()).c_str());
    // A public room link likewise, as "<bridge id> <room id>".
    SetEnvironmentVariableW(L"SF4E_PUBLIC_ROOM_LINK", !publicRoomLink.Valid() ? nullptr :
        sf4e::platform::Utf8ToWide((publicRoomLink.bridgeId + " " + publicRoomLink.roomId).c_str()).c_str());
    // A connect link as the service's ID.
    SetEnvironmentVariableW(L"SF4E_CONNECT_LINK", connectBridge.empty() ? nullptr :
        sf4e::platform::Utf8ToWide(connectBridge.c_str()).c_str());
    // A replay link as the file's path.
    SetEnvironmentVariableW(L"SF4E_REPLAY_LINK", replayLink.empty() ? nullptr :
        sf4e::platform::Utf8ToWide(replayLink.c_str()).c_str());
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
        // "auto" follows USF4's own language from here on.
        sf4e::platform::SetGameDirectory(location.directory);
        sf4e::loc::SetActive(sf4e::platform::ResolveUiLocale(languagePreference));
        spdlog::info("Interface language {} (preference {}, game {})", sf4e::loc::Tag(sf4e::loc::Active()),
            languagePreference, sf4e::platform::GameLanguage().empty() ? "unknown" : sf4e::platform::GameLanguage());
        wchar_t sidecar[MAX_PATH] = {};
        char sidecarAnsi[1024] = {};
        BOOL substituted = FALSE;
        if (!sf4e::install::ResolveInstallFile(L"Sidecar.dll",sidecar,MAX_PATH) ||
            !WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,sidecar,-1,sidecarAnsi,1024,nullptr,&substituted) || substituted) {
            if (!ShowRecovery(sf4e::loc::T("launcher.sidecar_missing"),chosenDirectory)) return 0;
            continue;
        }
        // Sidecar's own imports resolve inside the game process at its start.
        // Windows searches the game folder first, then the folder this
        // launcher gave SetDllDirectory in ConfigureDllSearch (a parent's DLL
        // directory is handed to the process it starts), then the system
        // folder, the 16-bit system folder, the Windows folder and PATH. So a
        // GGPO.dll left beside SSFIV.exe by another mod or an older install
        // loads instead of ours, lacks our exports, and Windows stops the game
        // with "entry point ggpo_get_last_confirmed_frame not found" before
        // Sidecar can log anything, while a copy in a system folder is never
        // reached. Name the file rather than let that happen. The list keeps
        // the game's order so the scan stops at the package. The 32-bit game
        // reads System32 as SysWOW64, so that folder is named as the player
        // sees it.
        wchar_t systemDirectory[MAX_PATH] = {}, windowsDirectory[MAX_PATH] = {}, legacySystemDirectory[MAX_PATH] = {};
        if (!GetSystemWow64DirectoryW(systemDirectory, MAX_PATH) && !GetSystemDirectoryW(systemDirectory, MAX_PATH)) systemDirectory[0] = L'\0';
        if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH)) windowsDirectory[0] = L'\0';
        else if (FAILED(PathCchCombine(legacySystemDirectory, MAX_PATH, windowsDirectory, L"System"))) legacySystemDirectory[0] = L'\0';
        const auto shadowing = sf4e::launcher::ShadowingRuntimeLibraries(dllDirectory,
            {location.directory, dllDirectory, systemDirectory, legacySystemDirectory, windowsDirectory}, exists);
        if (!shadowing.empty()) {
            std::wstring listed;
            for (const auto& path : shadowing) {
                spdlog::error(L"Runtime library outside the package would load instead of ours: {}", path.c_str());
                listed += (listed.empty() ? L"" : L"\n") + path;
            }
            if (!ShowRecovery(sf4e::loc::Tf("launcher.runtime_shadowed", sf4e::platform::WideToUtf8(listed)), chosenDirectory)) return 0;
            continue;
        }
        ArchiveReplays();
        const char* dlls[] = {sidecarAnsi};
        CreateAppIDFile(location.directory.data());
        sf4e::platform::HelperProcess helper, discord;
        const auto helperPath = std::filesystem::path(installRoot)/L"sf4-net.exe";
        sf4e::crash::DumpChannel dumps;
        sf4e::reports::CrashContext reportCrash;
        reportCrash.started = std::filesystem::file_time_type::clock::now();
        // The small dump a report may carry. It stays on this PC unless the
        // player turns it on in a report's preview.
        dumps.reportEnabled = true;
        if (!dumps.Create()) spdlog::warn("Could not create the crash dump channel (Win32 {})", GetLastError());
        // A game started by a normal process cannot reach a Steam that runs as
        // administrator: its SteamAPI_Init fails and it exits.
        const bool steamAbove = sf4e::platform::ProcessElevation() == sf4e::platform::Elevation::Normal &&
            sf4e::platform::SteamElevation() == sf4e::platform::Elevation::Elevated;
        if (steamAbove) spdlog::warn("Steam runs as administrator and the launcher does not; the game may not reach Steam");
        DWORD startError = 0;
        HANDLE game = CreateSF4Process(payload,helper,discord,helperPath.wstring(),location.directory.data(),location.executable.data(),1,dlls,dumps,startError);
        if (!game) {
            dumps.Close();
            // ERROR_ELEVATION_REQUIRED: SSFIV.exe is marked to run as administrator.
            const char* startMessage = startError == ERROR_ELEVATION_REQUIRED ? "launcher.game_runs_as_admin" : "launcher.start_failed";
            if (!ShowRecovery(sf4e::loc::T(startMessage),chosenDirectory)) return 0;
            continue;
        }
        // The game's crash handler asks for its dump here and waits for it.
        // Without a logs folder there is nowhere to put one; the game then
        // writes its own after the request fails.
        if (!g_logsDir[0]) dumps.Close();
        // More than thirty matches in one sitting would push replays out of the
        // game's slots before the copy at exit, so copy while it runs too.
        std::thread archiver([game] { while (WaitForSingleObject(game, 30000) == WAIT_TIMEOUT) ArchiveReplays(); });
        bool dumped = false;
        dumps.ServeUntilExit(game, g_logsDir, [&](bool written) {
            if (written) { dumped = true; spdlog::info(L"Wrote the game's crash dump to {}", dumps.written); }
            else spdlog::warn("Could not write the game's crash dump (Win32 {})", GetLastError());
        });
        dumps.Close();
        archiver.join();
        if (g_logsDir[0]) sf4e::crash::PruneDumps(g_logsDir, 5, dumps.reportWritten.c_str());
        DWORD exitCode = 0; GetExitCodeProcess(game,&exitCode);
        spdlog::info("Game exited with code {:#010x} ({})", exitCode, sf4e::crash::ExitCodeName(exitCode));
        const bool crashed = sf4e::crash::IsCrashExit(exitCode);
        if (crashed) {
            reportCrash.exitCode = exitCode; reportCrash.smallDump = dumps.reportWritten;
            if (dumps.exceptionCode) reportCrash.exceptionCode = dumps.exceptionCode;
            if (dumps.crashAddress) reportCrash.address = dumps.crashAddress;
        }
        if (crashed && !dumped) NoteWindowsCrashDump(GetProcessId(game));
        discord.Stop(); helper.Stop(); CloseHandle(game);
        ArchiveReplays();
        // A loader failure never reaches Sidecar's crash record, so the exit
        // code is the only thing that tells a missing export from a crash.
        // With Steam above the launcher the game cannot run at all, and how it
        // exits after SteamAPI_Init fails may look like a crash, so that comes first.
        // A crash's report reads the logs folder; without one the message
        // only names the files.
        const bool reportable = crashed && exitCode != 0xC0000139u && !steamAbove && g_logsDir[0];
        const char* exitMessage = exitCode == 0xC0000139u ? "launcher.game_wrong_dll" :
            steamAbove ? "launcher.steam_runs_as_admin" : crashed ? (reportable ? "launcher.game_crashed" : "launcher.game_crashed_files") : "launcher.game_error";
        if (exitCode != 0 && ShowRecovery(sf4e::loc::T(exitMessage),chosenDirectory,false,sf4e::ui::Tone::Error,false,
            reportable ? std::optional<sf4e::reports::CrashContext>(reportCrash) : std::nullopt)) continue;
        return 0;
    }
}
