#include "relay_host_spawn.hxx"

#include <stdio.h>

#include <windows.h>
#include <pathcch.h>

#include "../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {

	bool FetchAdvertiseRelayHost(char* outHost, int outHostLen) {
		if (!outHost || outHostLen <= 0) {
			return false;
		}
		outHost[0] = 0;
		if (sf4e::FetchPublicIPv4(outHost, outHostLen, 5000)) {
			return true;
		}
		return sf4e::DetectLanIPv4(outHost, outHostLen);
	}

	bool SpawnRelayHost(uint16_t sessionPort) {
		wchar_t moduleDir[MAX_PATH] = { 0 };
		wchar_t relayPath[MAX_PATH] = { 0 };
		if (GetModuleFileNameW(NULL, moduleDir, MAX_PATH) == 0) {
			return false;
		}
		if (FAILED(PathCchRemoveFileSpec(moduleDir, MAX_PATH))) {
			return false;
		}
		if (FAILED(PathCchCombine(relayPath, MAX_PATH, moduleDir, L"RelayHost.exe"))) {
			return false;
		}
		if (GetFileAttributesW(relayPath) == INVALID_FILE_ATTRIBUTES) {
			return false;
		}

		wchar_t cmdLine[512] = { 0 };
		swprintf_s(cmdLine, L"\"%s\" --port %u", relayPath, (unsigned)sessionPort);

		STARTUPINFOW si = { 0 };
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi = { 0 };

		if (!CreateProcessW(
			relayPath,
			cmdLine,
			NULL,
			NULL,
			FALSE,
			CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB,
			NULL,
			moduleDir,
			&si,
			&pi
		)) {
			return false;
		}

		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return true;
	}

} // namespace launcher
} // namespace sf4e
