#ifndef UNICODE
#define UNICODE
#endif

#define DIRECTINPUT_VERSION 0x0800

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>
#include <strsafe.h>
#include <d3d9.h>
#include <dinput.h>
#include <tchar.h>

#include <detours/detours.h>

#include "../Dimps/Dimps.hxx"
#include "../sf4e/sf4e.hxx"

#include "sidecar.hxx"

static void BootstrapLog(const char* msg) {
	wchar_t appData[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0) {
		return;
	}

	wchar_t dir[MAX_PATH] = { 0 };
	wchar_t logsDir[MAX_PATH] = { 0 };
	wchar_t logPath[MAX_PATH] = { 0 };
	if (FAILED(StringCchPrintfW(dir, MAX_PATH, L"%s\\sf4e", appData))) {
		return;
	}
	CreateDirectoryW(dir, NULL);
	if (FAILED(StringCchPrintfW(logsDir, MAX_PATH, L"%s\\logs", dir))) {
		return;
	}
	CreateDirectoryW(logsDir, NULL);
	if (FAILED(StringCchPrintfW(logPath, MAX_PATH, L"%s\\sidecar_bootstrap.log", logsDir))) {
		return;
	}
	WIN32_FILE_ATTRIBUTE_DATA attrs = { 0 };
	if (GetFileAttributesExW(logPath, GetFileExInfoStandard, &attrs)) {
		const ULONGLONG size =
			((ULONGLONG)attrs.nFileSizeHigh << 32) | (ULONGLONG)attrs.nFileSizeLow;
		if (size > 256 * 1024) {
			DeleteFileW(logPath);
		}
	}

	HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD written = 0;
	WriteFile(file, msg, (DWORD)strlen(msg), &written, NULL);
	WriteFile(file, "\r\n", 2, &written, NULL);
	CloseHandle(file);
}

static HMODULE LocatePERoot() {
	return DetourGetContainingModule(DetourGetEntryPoint(NULL));
}

sf4e::Payload* FindPayload() {
	HMODULE hMod;
	sf4e::Payload* payload;
	DWORD payloadLength;

	for (hMod = DetourEnumerateModules(NULL); hMod != NULL; hMod = DetourEnumerateModules(hMod)) {
		payload = (sf4e::Payload*)DetourFindPayload(hMod, sf4eSidecar::s_guidSidecarPayload, &payloadLength);
		if (GetLastError() == 0 && payload != NULL) {
			return payload;
		}
	}
	MessageBoxA(NULL, "Could not find launcher payload!", NULL, MB_OK);
	return NULL;
}

static LPCWSTR DETOUR_FAILED_MESSAGE = TEXT("Could not detour targets!");

__declspec(dllexport) BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,
	DWORD dwReason,
	LPVOID reserved
) {
	LONG error;
	sf4e::Payload* payload = NULL;

	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		DetourRestoreAfterWith();
		BootstrapLog("Sidecar DLL_PROCESS_ATTACH");
		payload = FindPayload();
		if (payload) {
			char msg[256] = { 0 };
			sprintf_s(
				msg,
				"Sidecar payload received version=%d mode=%d devOverlay=%d",
				payload->netplay.version,
				payload->netplay.mode,
				(int)payload->netplay.devOverlay
			);
			BootstrapLog(msg);
		}
		Dimps::Locate(LocatePERoot());
		DetourTransactionBegin();
		sf4e::Install(hinstDLL, payload);
		payload = NULL;
		error = DetourTransactionCommit();
		if (error != NO_ERROR) {
			char msg[128] = { 0 };
			sprintf_s(msg, "Sidecar DetourTransactionCommit failed error=%ld", error);
			BootstrapLog(msg);
			MessageBox(NULL, DETOUR_FAILED_MESSAGE, NULL, MB_OK);
		}
		else {
			BootstrapLog("Sidecar install committed");
		}
		break;

	case DLL_THREAD_ATTACH:
		break;

	case DLL_THREAD_DETACH:
		break;

	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
