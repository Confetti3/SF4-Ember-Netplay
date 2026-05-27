#include <stdio.h>
#include <memory>
#include <string.h>

#include <windows.h>
#include <bcrypt.h>
#include <pathcch.h>

#include <CLI/CLI.hpp>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "../common/sf4e__NetUtil.hxx"
#include "../session/sf4e__SessionServer.hxx"

using sf4e::SessionServer;
using sf4e::SessionProtocol::FixedPoint;

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

static const int kHashBlockSize = 1024;

static bool HashSidecarDllNextToExe(char* outHash, int outHashLen) {
	wchar_t modulePath[MAX_PATH] = { 0 };
	wchar_t sidecarPath[MAX_PATH] = { 0 };
	if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) == 0) {
		return false;
	}
	if (FAILED(PathCchRemoveFileSpec(modulePath, MAX_PATH))) {
		return false;
	}
	if (FAILED(PathCchCombine(sidecarPath, MAX_PATH, modulePath, L"Sidecar.dll"))) {
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
		spdlog::error("Could not open Sidecar.dll next to RelayHost.exe");
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

	outHash[0] = 0;
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

int main(int argc, char** argv) {
	spdlog::set_default_logger(spdlog::stdout_color_mt("RelayHost"));
	spdlog::set_level(spdlog::level::info);

	CLI::App app("SF4 Netplay Launcher session relay host (headless SessionServer)");
	uint16_t port = 23456;
	std::string identity;
	int idleExitSec = 120;
	app.add_option("--port", port, "Session listen port (TCP/UDP)")->check(CLI::Range(1, 65535));
	app.add_option("--identity", identity, "Session identity string (default: LAN IPv4)");
	app.add_option("--idle-exit-sec", idleExitSec, "Exit after N seconds with no clients (0 disables)");
	CLI11_PARSE(app, argc, argv);

	char sidecarHash[128] = { 0 };
	if (!HashSidecarDllNextToExe(sidecarHash, sizeof(sidecarHash))) {
		spdlog::error("Place Sidecar.dll next to RelayHost.exe (same folder as Launcher).");
		return 1;
	}

	char lanIp[64] = { 0 };
	if (identity.empty()) {
		sf4e::DetectLanIPv4(lanIp, sizeof(lanIp));
		identity = lanIp;
	}

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		spdlog::error("GameNetworkingSockets_Init failed: {}", errMsg);
		return 1;
	}

	FixedPoint roundTime = { 0, 99 };
	std::unique_ptr<SessionServer> server(new SessionServer(identity, sidecarHash, true, 3, roundTime));
	if (server->Listen(port) != 0) {
		spdlog::error("Failed to listen on port {}", port);
		GameNetworkingSockets_Kill();
		return 1;
	}
	server->PrepareForCallbacks();

	spdlog::info("RelayHost listening on port {} identity={}", port, identity);
	spdlog::info("Sidecar hash {} (must match game Sidecar.dll)", sidecarHash);
	if (idleExitSec > 0) {
		spdlog::info("Idle auto-exit enabled: {}s with no clients", idleExitSec);
	}

	const ULONGLONG startMs = GetTickCount64();
	ULONGLONG emptySinceMs = startMs;
	bool hadClients = false;

	for (;;) {
		server->PrepareForCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
		if (server->Step() != 0) {
			spdlog::error("Session server step failed");
			break;
		}

		if (idleExitSec > 0) {
			const ULONGLONG nowMs = GetTickCount64();
			const size_t clientCount = server->ConnectedClientCount();
			if (clientCount > 0) {
				hadClients = true;
				emptySinceMs = nowMs;
			}
			else {
				if (emptySinceMs == 0) {
					emptySinceMs = nowMs;
				}
				const ULONGLONG emptyMs = nowMs - emptySinceMs;
				if (hadClients && emptyMs >= 30000) {
					spdlog::info("RelayHost exiting: no clients for {}s after session ended", emptyMs / 1000);
					break;
				}
				if (emptyMs >= (ULONGLONG)idleExitSec * 1000ULL) {
					spdlog::info("RelayHost exiting: no clients for {}s", emptyMs / 1000);
					break;
				}
			}
		}

		Sleep(1);
	}

	server->Close();
	GameNetworkingSockets_Kill();
	return 0;
}
