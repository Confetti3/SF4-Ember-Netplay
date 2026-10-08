#include "update/PackageInstaller.hxx"
#include "update/ProgressWindow.hxx"
#include "../common/PackageInventory.hxx"
#include "../platform/Elevation.hxx"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <commctrl.h>
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

static bool WideToUtf8(const wchar_t* wide, char* out, int outLen) {
	if (!wide || !out || outLen <= 0) {
		return false;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outLen, NULL, NULL);
	return n > 0;
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

static bool InstallFiles(const wchar_t* staging, const wchar_t* install, HWND bar) {
    std::string error;
    // A package is thousands of file steps; the bar is told only when it
    // would move, so the window's message queue never fills.
    if (sf4e::launcher::InstallPackage(staging, install, error, [bar, shown = -1](std::uint64_t done, std::uint64_t total) mutable {
        const int position = total ? static_cast<int>(done * sf4e::launcher::ProgressWindow::Range / total) : 0;
        if (bar && position != shown) PostMessageW(bar, PBM_SETPOS, shown = position, 0);
        return true;
    })) return true;
    AppendLog(error.c_str()); return false;
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

static bool ParseArgs(int argc, wchar_t** argv, wchar_t* installDir, int installDirChars, wchar_t* stagingDir, int stagingDirChars, DWORD* waitPid, bool* recoverOnly, bool* uninstall, const wchar_t** status) {
	installDir[0] = L'\0';
	stagingDir[0] = L'\0';
	*waitPid = 0;
	*recoverOnly = false;
	*uninstall = false;

	for (int i = 1; i < argc; i++) {
		if (_wcsicmp(argv[i], L"-InstallDir") == 0 && i + 1 < argc) {
			wcsncpy_s(installDir, installDirChars, argv[++i], _TRUNCATE);
		}
		else if (_wcsicmp(argv[i], L"-StagingDir") == 0 && i + 1 < argc) {
			wcsncpy_s(stagingDir, stagingDirChars, argv[++i], _TRUNCATE);
		}
		else if (_wcsicmp(argv[i], L"-WaitPid") == 0 && i + 1 < argc) {
			*waitPid = (DWORD)_wtoi(argv[++i]);
		}
		else if (_wcsicmp(argv[i], L"-Status") == 0 && i + 1 < argc) { if (argv[++i][0]) *status = argv[i]; }
		else if (_wcsicmp(argv[i], L"-RecoverOnly") == 0) { *recoverOnly = true; }
		else if (_wcsicmp(argv[i], L"-Uninstall") == 0) { *uninstall = true; }
	}

	return installDir[0] != L'\0' && (*recoverOnly || *uninstall || stagingDir[0] != L'\0');
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
	wchar_t installDir[MAX_PATH] = { 0 };
	wchar_t stagingDir[MAX_PATH] = { 0 };
	DWORD waitPid = 0;
	bool recoverOnly = false, uninstall = false;
	// The launcher passes this in the player's language; one from before
	// -Status does not.
	const wchar_t* status = L"Installing the update...";

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

	if (!ParseArgs(argc, argv, installDir, MAX_PATH, stagingDir, MAX_PATH, &waitPid, &recoverOnly, &uninstall, &status)) {
		AppendLog("ERROR: missing -InstallDir or -StagingDir");
		return 1;
	}
	if (uninstall) return Uninstall(installDir);
	if (recoverOnly) {
		// With -WaitPid the Launcher asked for this at startup: wait for it to
		// exit, then start it again, showing update recovery on failure.
		if (waitPid && !WaitForProcessExit(waitPid, 30000)) {
			AppendLog("ERROR: launcher is still running; recovery cancelled");
			return 1;
		}
		std::string recoveryError;
		if (!sf4e::launcher::RecoverPackage(installDir,recoveryError,false)) {
			AppendLog(recoveryError.c_str());
			if (waitPid) StartLauncher(installDir, L"--updates --update-error");
			return 1;
		}
		AppendLog("Update recovery complete");
		if (waitPid) StartLauncher(installDir);
		return 0;
	}

	char startLine[1024] = { 0 };
	char installUtf8[MAX_PATH * 2] = { 0 };
	char stagingUtf8[MAX_PATH * 2] = { 0 };
	WideToUtf8(installDir, installUtf8, sizeof(installUtf8));
	WideToUtf8(stagingDir, stagingUtf8, sizeof(stagingUtf8));
	snprintf(
		startLine,
		sizeof(startLine),
		"Updater start InstallDir=%s StagingDir=%s WaitPid=%lu",
		installUtf8,
		stagingUtf8,
		waitPid
	);
	AppendLog(startLine);

	if (GetFileAttributesW(installDir) == INVALID_FILE_ATTRIBUTES) {
		AppendLog("ERROR: install directory not found");
		return 1;
	}
	if (GetFileAttributesW(stagingDir) == INVALID_FILE_ATTRIBUTES) {
		AppendLog("ERROR: staging directory not found");
		return 1;
	}

	wchar_t title[512] = { 0 };
	_snwprintf_s(title, _TRUNCATE, L"SF4 Ember Netplay - %s", status);
	sf4e::launcher::ProgressWindow progress(title);
	if (!WaitForProcessExit(waitPid, 30000)) {
		AppendLog("ERROR: launcher is still running; update cancelled");
        progress.Close();
        StartLauncher(installDir, L"--updates --update-error");
        return 1;
	}


	Sleep(500);

	const bool installed = InstallFiles(stagingDir, installDir, progress.Window());
	progress.Close();
	if (!installed) {
        StartLauncher(installDir, L"--updates --update-error");
		return 1;
	}

	AppendLog("starting Launcher.exe");
	if (!StartLauncher(installDir)) {
		return 1;
	}

	AppendLog("Updater complete");
	return 0;
}
