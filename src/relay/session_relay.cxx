#include <stdio.h>
#include <memory>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <thread>
#endif

#include <CLI/CLI.hpp>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "../session/sf4e__SessionServer.hxx"

using sf4e::SessionServer;
using sf4e::SessionProtocol::FixedPoint;

static uint64_t MonotonicMs() {
#ifdef _WIN32
	return GetTickCount64();
#else
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

static void SleepMs(int ms) {
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

int main(int argc, char** argv) {
	spdlog::set_default_logger(spdlog::stdout_color_mt("SessionRelay"));
	spdlog::set_level(spdlog::level::info);

	CLI::App app("SF4 Netplay Launcher VPS session relay (headless SessionServer)");
	uint16_t port = 23456;
	std::string identity;
	std::string sidecarHash;
	int idleExitSec = 0;
	app.add_option("--port", port, "Session listen port (TCP/UDP)")->check(CLI::Range(1, 65535));
	app.add_option("--identity", identity, "Session identity string (default: relay-vps)");
	app.add_option("--sidecar-hash", sidecarHash, "Expected Sidecar.dll SHA-256 hex (required)")->required();
	app.add_option("--idle-exit-sec", idleExitSec, "Exit after N seconds with no clients (0 disables)");
	CLI11_PARSE(app, argc, argv);

	if (sidecarHash.empty()) {
		spdlog::error("--sidecar-hash is required");
		return 1;
	}
	if (identity.empty()) {
		identity = "relay-vps";
	}

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		spdlog::error("GameNetworkingSockets_Init failed: {}", errMsg);
		return 1;
	}

	FixedPoint roundTime = { 0, 99 };
	std::unique_ptr<SessionServer> server(
		new SessionServer(identity, sidecarHash, true, 3, roundTime)
	);
	if (server->Listen(port) != 0) {
		spdlog::error("Failed to listen on port {}", port);
		GameNetworkingSockets_Kill();
		return 1;
	}
	server->PrepareForCallbacks();

	spdlog::info("SessionRelay listening on port {} identity={}", port, identity);
	spdlog::info("Sidecar hash {}", sidecarHash);

	const uint64_t startMs = MonotonicMs();
	uint64_t emptySinceMs = startMs;
	bool hadClients = false;

	for (;;) {
		server->PrepareForCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
		if (server->Step() != 0) {
			spdlog::error("Session server step failed");
			break;
		}

		if (idleExitSec > 0) {
			const uint64_t nowMs = MonotonicMs();
			const size_t clientCount = server->ConnectedClientCount();
			if (clientCount > 0) {
				hadClients = true;
				emptySinceMs = nowMs;
			}
			else {
				if (emptySinceMs == 0) {
					emptySinceMs = nowMs;
				}
				const uint64_t emptyMs = nowMs - emptySinceMs;
				if (hadClients && emptyMs >= 30000) {
					spdlog::info("SessionRelay exiting: no clients for {}s after session ended", emptyMs / 1000);
					break;
				}
				if (emptyMs >= (uint64_t)idleExitSec * 1000ULL) {
					spdlog::info("SessionRelay exiting: no clients for {}s", emptyMs / 1000);
					break;
				}
			}
		}

		SleepMs(1);
	}

	server->Close();
	GameNetworkingSockets_Kill();
	return 0;
}
