#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <pathcch.h>
#include <shellapi.h>
#include <strsafe.h>
#include <tlhelp32.h>

namespace {

static const wchar_t* kRequiredPackagePaths[] = {
	L"Launcher.exe",
	L"Sidecar.dll",
	L"RelayHost.exe",
	L"Updater.exe",
	L"spdlog.dll",
	L"fmt.dll",
	L"GameNetworkingSockets.dll",
	L"GGPO.dll",
	L"libcrypto-3.dll",
	L"libprotobuf.dll",
	L"abseil_dll.dll",
	L"Qt6Core.dll",
	L"Qt6Gui.dll",
	L"Qt6Widgets.dll",
	L"qt.conf",
	L"plugins\\platforms\\qwindows.dll",
	L"preflight.ps1",
	L"preflight.cmd",
	L"START_HERE.md",
	L"BUILD_INFO.txt",
};

static const wchar_t* kAllowedPackagePaths[] = {
	L"Launcher.exe",
	L"Sidecar.dll",
	L"RelayHost.exe",
	L"Updater.exe",
	L"START_HERE.md",
	L"preflight.cmd",
	L"preflight.ps1",
	L"qt.conf",
	L"MANIFEST.txt",
	L"ATTRIBUTION.md",
	L"BUILD_INFO.txt",
	L"spdlog.dll",
	L"fmt.dll",
	L"GameNetworkingSockets.dll",
	L"GGPO.dll",
	L"libcrypto-3.dll",
	L"libprotobuf.dll",
	L"abseil_dll.dll",
	L"Qt6Core.dll",
	L"Qt6Gui.dll",
	L"Qt6Widgets.dll",
	L"Qt6Network.dll",
	L"icudt78.dll",
	L"icuin78.dll",
	L"icuuc78.dll",
	L"double-conversion.dll",
	L"pcre2-16.dll",
	L"md4c.dll",
	L"zlib1.dll",
	L"plugins\\generic\\qtuiotouchplugin.dll",
	L"plugins\\imageformats\\qgif.dll",
	L"plugins\\imageformats\\qico.dll",
	L"plugins\\imageformats\\qjpeg.dll",
	L"plugins\\networkinformation\\qnetworklistmanager.dll",
	L"plugins\\platforms\\qwindows.dll",
	L"plugins\\styles\\qmodernwindowsstyle.dll",
	L"plugins\\tls\\qcertonlybackend.dll",
	L"plugins\\tls\\qschannelbackend.dll",
	L"docs\\TROUBLESHOOTING.md",
	L"docs\\USER_NETPLAY.md",
	L"docs\\BETA_TESTERS.md",
	L"docs\\SCOPE_AND_LIMITATIONS.md",
	L"docs\\TEAM_QUICKSTART.md",
	L"docs\\SMOKE_TEST.md",
	L"docs\\NETPLAY_INVARIANTS.md",
	L"docs\\WINDOWS_DEFENDER.md",
	L"docs\\CODE_SIGNING.md",
};

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

static bool PathExistsUnderRoot(const wchar_t* root, const wchar_t* relPath) {
	wchar_t path[MAX_PATH * 2] = { 0 };
	if (FAILED(PathCchCombine(path, MAX_PATH * 2, root, relPath))) {
		return false;
	}
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool IsAllowedPackagePath(const wchar_t* relPath) {
	if (!relPath || !relPath[0]) {
		return false;
	}
	if (_wcsicmp(relPath, L"SECURITY.md") == 0) {
		return true;
	}
	if (_wcsnicmp(relPath, L"plugins\\", 8) == 0) {
		const wchar_t* ext = wcsrchr(relPath, L'.');
		if (ext && _wcsicmp(ext, L".pdb") == 0) {
			return true;
		}
	}
	for (const wchar_t* allowed : kAllowedPackagePaths) {
		if (_wcsicmp(relPath, allowed) == 0) {
			return true;
		}
	}
	return false;
}

static bool ValidatePackageTree(const wchar_t* root, const wchar_t* relPrefix) {
	wchar_t dir[MAX_PATH * 2] = { 0 };
	if (relPrefix && relPrefix[0]) {
		if (FAILED(PathCchCombine(dir, MAX_PATH * 2, root, relPrefix))) {
			return false;
		}
	}
	else {
		wcsncpy_s(dir, root, _TRUNCATE);
	}

	wchar_t pattern[MAX_PATH * 2] = { 0 };
	wcsncpy_s(pattern, dir, _TRUNCATE);
	if (FAILED(PathCchAppend(pattern, MAX_PATH * 2, L"*"))) {
		return false;
	}

	WIN32_FIND_DATAW fd = { 0 };
	HANDLE hFind = FindFirstFileW(pattern, &fd);
	if (hFind == INVALID_HANDLE_VALUE) {
		return true;
	}
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
			continue;
		}
		wchar_t rel[MAX_PATH * 2] = { 0 };
		if (relPrefix && relPrefix[0]) {
			if (FAILED(StringCchPrintfW(rel, MAX_PATH * 2, L"%s\\%s", relPrefix, fd.cFileName))) {
				FindClose(hFind);
				return false;
			}
		}
		else if (FAILED(StringCchCopyW(rel, MAX_PATH * 2, fd.cFileName))) {
			FindClose(hFind);
			return false;
		}

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!ValidatePackageTree(root, rel)) {
				FindClose(hFind);
				return false;
			}
		}
		else if (!IsAllowedPackagePath(rel)) {
			char relUtf8[MAX_PATH * 2] = { 0 };
			WideToUtf8(rel, relUtf8, sizeof(relUtf8));
			char buf[MAX_PATH * 2 + 64] = { 0 };
			snprintf(buf, sizeof(buf), "ERROR: unexpected package file: %s", relUtf8);
			AppendLog(buf);
			FindClose(hFind);
			return false;
		}
	} while (FindNextFileW(hFind, &fd));
	FindClose(hFind);
	return true;
}

static bool ValidateStagedPackage(const wchar_t* stagingDir) {
	for (const wchar_t* rel : kRequiredPackagePaths) {
		if (!PathExistsUnderRoot(stagingDir, rel)) {
			char relUtf8[MAX_PATH * 2] = { 0 };
			WideToUtf8(rel, relUtf8, sizeof(relUtf8));
			char buf[MAX_PATH * 2 + 64] = { 0 };
			snprintf(buf, sizeof(buf), "ERROR: missing package file: %s", relUtf8);
			AppendLog(buf);
			return false;
		}
	}
	return ValidatePackageTree(stagingDir, L"");
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
	return waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT;
}

static void StopProcessesByName(const wchar_t* exeName) {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return;
	}

	PROCESSENTRY32W entry = { 0 };
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szExeFile, exeName) == 0) {
				HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
				if (process) {
					TerminateProcess(process, 1);
					CloseHandle(process);
				}
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
}

static bool RunProcessAndWait(const wchar_t* cmdLine, DWORD* outExitCode) {
	STARTUPINFOW si = { 0 };
	PROCESS_INFORMATION pi = { 0 };
	si.cb = sizeof(si);
	wchar_t mutableCmd[4096] = { 0 };
	wcsncpy_s(mutableCmd, cmdLine, _TRUNCATE);
	if (!CreateProcessW(NULL, mutableCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		return false;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (outExitCode) {
		*outExitCode = exitCode;
	}
	return true;
}

static bool RunRobocopyMirror(const wchar_t* stagingDir, const wchar_t* installDir) {
	wchar_t cmdLine[4096] = { 0 };
	swprintf_s(
		cmdLine,
		L"robocopy.exe \"%s\" \"%s\" /MIR /R:2 /W:2 /NFL /NDL /NJH /NJS",
		stagingDir,
		installDir
	);

	char cmdUtf8[4096] = { 0 };
	WideToUtf8(cmdLine, cmdUtf8, sizeof(cmdUtf8));
	AppendLog(cmdUtf8);

	DWORD exitCode = 99;
	if (!RunProcessAndWait(cmdLine, &exitCode)) {
		AppendLog("robocopy spawn failed");
		return false;
	}
	if (exitCode > 7) {
		char buf[64] = { 0 };
		snprintf(buf, sizeof(buf), "robocopy failed with exit code %u", (unsigned)exitCode);
		AppendLog(buf);
		return false;
	}
	return true;
}

static bool StartLauncher(const wchar_t* installDir) {
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
	sei.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&sei)) {
		char buf[128] = { 0 };
		snprintf(buf, sizeof(buf), "failed to start Launcher.exe (Win32 %lu)", GetLastError());
		AppendLog(buf);
		return false;
	}
	return true;
}

static bool ParseArgs(int argc, wchar_t** argv, wchar_t* installDir, int installDirChars, wchar_t* stagingDir, int stagingDirChars, DWORD* waitPid) {
	installDir[0] = L'\0';
	stagingDir[0] = L'\0';
	*waitPid = 0;

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
	}

	return installDir[0] != L'\0' && stagingDir[0] != L'\0';
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	wchar_t installDir[MAX_PATH] = { 0 };
	wchar_t stagingDir[MAX_PATH] = { 0 };
	DWORD waitPid = 0;

	if (!ParseArgs(argc, argv, installDir, MAX_PATH, stagingDir, MAX_PATH, &waitPid)) {
		AppendLog("ERROR: missing -InstallDir or -StagingDir");
		return 1;
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
	if (!ValidateStagedPackage(stagingDir)) {
		AppendLog("ERROR: staging package failed allowlist validation");
		return 1;
	}

	if (!WaitForProcessExit(waitPid, 30000)) {
		AppendLog("WARNING: launcher process did not exit within timeout");
	}

	StopProcessesByName(L"RelayHost.exe");
	Sleep(500);

	if (!RunRobocopyMirror(stagingDir, installDir)) {
		return 1;
	}

	AppendLog("starting Launcher.exe");
	if (!StartLauncher(installDir)) {
		return 1;
	}

	AppendLog("Updater complete");
	return 0;
}
