#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <pathcch.h>
#include <shellapi.h>
#include <strsafe.h>

#include "../common/install_paths.hxx"

namespace {

bool LaunchApp(const wchar_t* appPath, const wchar_t* workDir) {
	wchar_t cmdLine[32768] = { 0 };
	const wchar_t* raw = GetCommandLineW();
	if (!raw) {
		return false;
	}
	if (FAILED(StringCchCopyW(cmdLine, 32768, raw))) {
		return false;
	}

	STARTUPINFOW si = { sizeof(si) };
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	PROCESS_INFORMATION pi = { 0 };
	if (!CreateProcessW(
		appPath,
		cmdLine,
		NULL,
		NULL,
		TRUE,
		0,
		NULL,
		workDir,
		&si,
		&pi)) {
		return false;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	ExitProcess(exitCode);
	return true;
}

} // namespace

int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nShowCmd
) {
	(void)hInstance;
	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nShowCmd;

	sf4e::install::ConfigureDllSearch();

	wchar_t root[MAX_PATH] = { 0 };
	wchar_t appPath[MAX_PATH] = { 0 };
	if (!sf4e::install::GetInstallRoot(root, MAX_PATH)) {
		return 1;
	}

	if (sf4e::install::ResolveInstallFile(L"LauncherApp.exe", appPath, MAX_PATH)) {
		if (!LaunchApp(appPath, root)) {
			return 1;
		}
		return 0;
	}

	// Developer flat layout: run LauncherApp.exe beside this stub (same build dir).
	if (FAILED(PathCchCombine(appPath, MAX_PATH, root, L"LauncherApp.exe"))) {
		return 1;
	}
	if (GetFileAttributesW(appPath) == INVALID_FILE_ATTRIBUTES) {
		MessageBoxW(NULL,
			L"Cannot find LauncherApp.exe.\n\n"
			L"Expected dll\\LauncherApp.exe in the package, or LauncherApp.exe next to this exe for dev builds.",
			L"SF4 Netplay Launcher",
			MB_OK | MB_ICONERROR);
		return 1;
	}
	if (!LaunchApp(appPath, root)) {
		return 1;
	}
	return 0;
}
