#include "update/PackageInstaller.hxx"
#include "update/UpdaterArguments.hxx"
#include "update/UpdaterRun.hxx"
#include "../common/PackageInventory.hxx"
#include "../platform/Elevation.hxx"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>
#include <pathcch.h>
#include <shellapi.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include "../common/SelectionAssetPath.hxx"

namespace {

static void AppendLog(const char* message) {
	wchar_t tempDir[MAX_PATH] = { 0 };
	if (GetTempPathW(MAX_PATH, tempDir) == 0) {
		return;
	}
	wchar_t logPath[MAX_PATH] = { 0 };
	if (FAILED(PathCchCombine(logPath, MAX_PATH, tempDir, L"sf4-netplay-update.log"))) {
		return;
	}

	SYSTEMTIME st = { 0 };
	GetLocalTime(&st);
	char line[1024] = { 0 };
	snprintf(
		line,
		sizeof(line),
		"%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
		st.wYear,
		st.wMonth,
		st.wDay,
		st.wHour,
		st.wMinute,
		st.wSecond,
		message ? message : ""
	);

	HANDLE hFile = CreateFileW(
		logPath,
		FILE_APPEND_DATA,
		FILE_SHARE_READ,
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD written = 0;
	WriteFile(hFile, line, (DWORD)strlen(line), &written, NULL);
	CloseHandle(hFile);
}

static bool WaitForProcessExit(DWORD pid, DWORD timeoutMs) {
	if (pid == 0) {
		return true;
	}
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!process) {
		return true;
	}
	DWORD waitResult = WaitForSingleObject(process, timeoutMs);
	CloseHandle(process);
	return waitResult == WAIT_OBJECT_0;
}

static bool StartLauncher(const wchar_t* installDir, const wchar_t* arguments = nullptr) {
	wchar_t launcherPath[MAX_PATH] = { 0 };
	if (FAILED(PathCchCombine(launcherPath, MAX_PATH, installDir, L"Launcher.exe"))) {
		return false;
	}
	if (GetFileAttributesW(launcherPath) == INVALID_FILE_ATTRIBUTES) {
		AppendLog("Launcher.exe missing after update");
		return false;
	}

	SHELLEXECUTEINFOW sei = { 0 };
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_FLAG_NO_UI;
	sei.lpVerb = L"open";
	sei.lpFile = launcherPath;
	sei.lpDirectory = installDir;
    sei.lpParameters = arguments;
	sei.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&sei)) {
		char buf[128] = { 0 };
		snprintf(buf, sizeof(buf), "failed to start Launcher.exe (Win32 %lu)", GetLastError());
		AppendLog(buf);
		return false;
	}
	return true;
}

// The installer's uninstaller runs this from a copy outside the folder:
// Windows will not delete a running image, so a copy inside the folder would
// leave itself behind.
static int Uninstall(const wchar_t* installDir) {
	std::string error;
	if (!sf4e::launcher::UninstallPackage(installDir, error)) {
		AppendLog(("ERROR: uninstall failed: " + error).c_str());
		return 1;
	}
	AppendLog("Uninstall complete");
	return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	// Players double-click Updater.exe to update. Its own work needs the
	// launcher's arguments, so a plain start opens the launcher's Updates
	// window, which checks for, downloads and installs the update.
	if (argc == 1) {
		wchar_t ownDir[MAX_PATH] = { 0 };
		const DWORD length = GetModuleFileNameW(nullptr, ownDir, MAX_PATH);
		if (length == 0 || length >= MAX_PATH || FAILED(PathCchRemoveFileSpec(ownDir, MAX_PATH))) return 1;
		AppendLog("Updater started without arguments; opening the Updates window");
		return StartLauncher(ownDir, L"--updates") ? 0 : 1;
	}

	// The Launcher starts this only as the normal user (UpdaterMayRun); a copy
	// started any other way does not install or recover either.
	if (sf4e::platform::ProcessElevation() != sf4e::platform::Elevation::Normal) {
		AppendLog("ERROR: Updater does not run as administrator; start Ember normally");
		return 1;
	}

	sf4e::launcher::UpdaterArguments arguments;
	if (!sf4e::launcher::ParseUpdaterArguments(argc, argv, arguments)) {
		AppendLog("ERROR: missing -InstallDir or -StagingDir");
		return 1;
	}
	if (arguments.uninstall) return Uninstall(arguments.installDir.c_str());

	sf4e::launcher::UpdaterHost host;
	host.waitForLauncher = [](std::uint32_t pid) { return WaitForProcessExit(pid, 30000); };
	host.startLauncher = [](const wchar_t* installDir, const wchar_t* arguments) { return StartLauncher(installDir, arguments); };
	host.log = AppendLog;
	return sf4e::launcher::RunUpdater(arguments, host);
}
