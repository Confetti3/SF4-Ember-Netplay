// SF4 Ember's launch steps (LaunchSteps.hxx): Sidecar.dll started inside
// SSFIV.exe by Detours with its payload, the networking helper and Discord
// companion it is handed, the replay archive before and after each run,
// USF4's language from its folder and the runtime libraries Sidecar imports.
// Each step is the code launcher.cxx ran before the steps were the product's,
// moved unchanged: the same calls, order and log lines.

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <pathcch.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <detours/detours.h>
#include <spdlog/spdlog.h>

#include "LaunchSteps.hxx"
#include "GameLocator.hxx"
#include "netplay/netplay_persist.hxx"
#include "../sf4e/sf4e.hxx"
#include "../sidecar/sidecar.hxx"
#include "../common/CrashDump.hxx"
#include "../common/GameLanguage.hxx"
#include "../common/ProductIdentity.hxx"
#include "../common/ReplayLink.hxx"
#include "../common/install_paths.hxx"
#include "../common/sf4e__NetplayConfig.hxx"
#include "../platform/HelperPipe.hxx"
#include "../platform/HelperProcess.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/ReplayPath.hxx"

namespace {

sf4e::Payload payload{};

// What one run of the game holds: the helper and the Discord companion
// handed to it, kept for the game's lifetime.
struct Run {
	sf4e::platform::HelperProcess helper, discord;
};
std::unique_ptr<Run> run;

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

// Copies every match replay into Ember's archive (platform/ReplayFiles.hxx)
// so the game's slots are not the only copy: before a game run, and after it
// for what Sidecar could not copy while it ran. Never blocks a start.
void ArchiveReplays() {
	static bool noted = false;
	if (sf4e::platform::replays::Archive() < 0 && !noted) { noted = true; spdlog::info("Replays: no settings folder, nothing archived"); }
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
    if (sf4e::install::ResolveInstallFile(ember::Product().discordExe,discordPath,32768) &&
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

} // namespace

namespace ember { namespace launch {

void AddOptions(CLI::App&) {}

std::wstring LauncherLogName() { return Product().launcherLog; }

std::wstring InstanceScope() { return L""; }

void Prepare(sf4e::launcher::PersistedSettings& settings, bool showConsole) {
    payload.args.bShowConsole = showConsole;
    sf4e::launcher::EnsureUniqueDisplayName(settings);
    payload.netplay.mode = static_cast<int>(sf4e::NetplayMode::Idle);
    payload.netplay.version = sf4e::SF4E_NETPLAY_CONFIG_VERSION;
    strncpy_s(payload.netplay.displayName, settings.displayName, _TRUNCATE);
    payload.netplay.inputDelay = settings.inputDelay;
    payload.netplay.editionSelect = settings.editionSelect;
    payload.netplay.roundCount = settings.roundCount;
    payload.netplay.roundTimeIntegral = settings.roundTimeIntegral;
    payload.netplay.useRelay = 0;
}

std::string GameLanguage(const std::wstring& gameDirectory) {
    return sf4e::loc::DetectGameLanguage(gameDirectory, sf4e::platform::SteamClientLanguage());
}

std::vector<const wchar_t*> RuntimeLibraries() {
    return {std::begin(sf4e::launcher::kSidecarRuntimeLibraries), std::end(sf4e::launcher::kSidecarRuntimeLibraries)};
}

std::string ResolveReplayLink(const std::string& uri) {
    return sf4e::platform::ResolveReplayFile(sf4e::replay_link::ParseReplayLink(uri));
}

void BeforeStart(const std::wstring& gameDirectory) {
    ArchiveReplays();
    std::wstring directory = gameDirectory;
    CreateAppIDFile(directory.data());
}

HANDLE StartGame(const StartRequest& request, DWORD& startError) {
    run = std::make_unique<Run>();
    const auto helperPath = std::filesystem::path(request.installRoot)/sf4e::platform::helper_pipe::Executable;
    std::wstring directory = request.gameDirectory, executable = request.executable;
    const char* dlls[] = {request.moduleAnsi.c_str()};
    return CreateSF4Process(payload,run->helper,run->discord,helperPath.wstring(),directory.data(),executable.data(),1,dlls,*request.dumps,startError);
}

void GameEnded(bool ran) {
    if (run && ran) {
        run->discord.Stop(); run->helper.Stop();
        ArchiveReplays();
    }
    run.reset();
}

} } // namespace ember::launch
