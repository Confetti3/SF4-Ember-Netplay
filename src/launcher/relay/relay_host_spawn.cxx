#include "relay_host_spawn.hxx"

#include "../../common/install_paths.hxx"

#include <stdio.h>

#include <windows.h>
#include <bcrypt.h>
#include <pathcch.h>

#include "../common/sf4e__NetUtil.hxx"

#include <spdlog/spdlog.h>

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

namespace sf4e {
namespace launcher {

	static const int kHashBlockSize = 1024;

	static unsigned long g_relayHostPid = 0;

	static void StopRelayHostInternal() {
		if (g_relayHostPid == 0) {
			return;
		}

		HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, g_relayHostPid);
		if (process) {
			TerminateProcess(process, 0);
			WaitForSingleObject(process, 5000);
			CloseHandle(process);
		}

		g_relayHostPid = 0;
	}

	unsigned long GetRelayHostPid() {
		return g_relayHostPid;
	}

	void StopRelayHost() {
		StopRelayHostInternal();
	}

	bool HashSidecarDllNextToLauncher(char* outHash, int outHashLen) {
		if (!outHash || outHashLen <= 0) {
			return false;
		}
		outHash[0] = 0;

		wchar_t modulePath[MAX_PATH] = { 0 };
		wchar_t sidecarPath[MAX_PATH] = { 0 };
		if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) == 0) {
			return false;
		}
		if (FAILED(PathCchRemoveFileSpec(modulePath, MAX_PATH))) {
			return false;
		}
		if (sf4e::install::ResolveInstallFile(L"Sidecar.dll", sidecarPath, MAX_PATH)) {
			// resolved (dll\ or root)
		} else if (FAILED(PathCchCombine(sidecarPath, MAX_PATH, modulePath, L"Sidecar.dll"))) {
			return false;
		}

		BCRYPT_ALG_HANDLE hAlg = NULL;
		BCRYPT_HASH_HANDLE hHash = NULL;
		NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
		if (!NT_SUCCESS(status)) {
			return false;
		}

		DWORD cbHashObject = 0;
		DWORD cbData = 0;
		if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbHashObject, sizeof(DWORD), &cbData, 0))) {
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}
		DWORD cbHash = 0;
		if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash, sizeof(DWORD), &cbData, 0))) {
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}

		PUCHAR pbHashObject = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbHashObject);
		PUCHAR pbHash = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbHash);
		if (!pbHashObject || !pbHash) {
			if (pbHashObject) {
				HeapFree(GetProcessHeap(), 0, pbHashObject);
			}
			if (pbHash) {
				HeapFree(GetProcessHeap(), 0, pbHash);
			}
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}

		if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, NULL, 0, 0))) {
			HeapFree(GetProcessHeap(), 0, pbHashObject);
			HeapFree(GetProcessHeap(), 0, pbHash);
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}

		HANDLE hFile = CreateFileW(
			sidecarPath,
			GENERIC_READ,
			FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_SEQUENTIAL_SCAN,
			NULL
		);
		if (hFile == INVALID_HANDLE_VALUE) {
			BCryptDestroyHash(hHash);
			HeapFree(GetProcessHeap(), 0, pbHashObject);
			HeapFree(GetProcessHeap(), 0, pbHash);
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}

		BYTE rgbFile[kHashBlockSize];
		DWORD cbRead = 0;
		for (;;) {
			if (!ReadFile(hFile, rgbFile, kHashBlockSize, &cbRead, NULL) || cbRead == 0) {
				break;
			}
			if (!NT_SUCCESS(BCryptHashData(hHash, rgbFile, cbRead, 0))) {
				CloseHandle(hFile);
				BCryptDestroyHash(hHash);
				HeapFree(GetProcessHeap(), 0, pbHashObject);
				HeapFree(GetProcessHeap(), 0, pbHash);
				BCryptCloseAlgorithmProvider(hAlg, 0);
				return false;
			}
		}
		CloseHandle(hFile);

		if (!NT_SUCCESS(BCryptFinishHash(hHash, pbHash, cbHash, 0))) {
			BCryptDestroyHash(hHash);
			HeapFree(GetProcessHeap(), 0, pbHashObject);
			HeapFree(GetProcessHeap(), 0, pbHash);
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}

		for (DWORD i = 0; i < cbHash; i++) {
			char pair[3];
			snprintf(pair, sizeof(pair), "%02x", pbHash[i]);
			strncat_s(outHash, outHashLen, pair, _TRUNCATE);
		}

		BCryptDestroyHash(hHash);
		HeapFree(GetProcessHeap(), 0, pbHashObject);
		HeapFree(GetProcessHeap(), 0, pbHash);
		BCryptCloseAlgorithmProvider(hAlg, 0);
		return outHash[0] != 0;
	}

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

	bool SpawnRelayHost(uint16_t sessionPort, unsigned long* outPid) {
		StopRelayHostInternal();

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
			CREATE_NEW_CONSOLE,
			NULL,
			moduleDir,
			&si,
			&pi
		)) {
			return false;
		}

		g_relayHostPid = pi.dwProcessId;
		if (outPid) {
			*outPid = g_relayHostPid;
		}

		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);

		if (!sf4e::WaitForLocalTcpPort(sessionPort, 5000, g_relayHostPid)) {
			DWORD exitCode = STILL_ACTIVE;
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_relayHostPid);
			if (process) {
				GetExitCodeProcess(process, &exitCode);
				CloseHandle(process);
			}
			if (exitCode != STILL_ACTIVE) {
				spdlog::error("RelayHost exited early with code {}", exitCode);
			}
			else {
				spdlog::error("RelayHost started but port {} is not listening yet", sessionPort);
			}
			StopRelayHostInternal();
			return false;
		}

		return true;
	}

} // namespace launcher
} // namespace sf4e
