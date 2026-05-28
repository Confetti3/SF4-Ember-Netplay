#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

static const char kMagicReg[] = "SF4G";
static const char kMagicHealth[] = "SF4H";
static const char kRespHealth[] = "SF4OK";
static const char kRespRegistered[] = "SF4R";
static const char kRespWaiting[] = "SF4W";
static const int kRegPacketLenLegacy = 4 + 32;
static const int kRegPacketLenV2 = 4 + 32 + 2;
static const int kMaxPacket = 2048;

struct Endpoint {
	sockaddr_in addr = {};
	bool active = false;
};

static bool IsHexToken(const char* token, int len) {
	if (len != 32) {
		return false;
	}
	for (int i = 0; i < len; i++) {
		if (!std::isxdigit((unsigned char)token[i])) {
			return false;
		}
	}
	return true;
}

static bool TokenMatches(const char* expected, const char* received) {
	char a[33] = { 0 };
	char b[33] = { 0 };
	for (int i = 0; i < 32; i++) {
		a[i] = (char)std::tolower((unsigned char)expected[i]);
		b[i] = (char)std::tolower((unsigned char)received[i]);
	}
	return strncmp(a, b, 32) == 0;
}

static bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static bool SameIp(const sockaddr_in& a, const sockaddr_in& b) {
	return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

static void CopyEndpoint(sockaddr_in& dst, const sockaddr_in& src) {
	dst = src;
}

static int FindSlotByEndpoint(Endpoint* slots, const sockaddr_in& from) {
	for (int i = 0; i < 2; i++) {
		if (slots[i].active && SameEndpoint(slots[i].addr, from)) {
			return i;
		}
	}
	return -1;
}

static int FindSlotByIp(Endpoint* slots, const sockaddr_in& from) {
	int found = -1;
	for (int i = 0; i < 2; i++) {
		if (slots[i].active && SameIp(slots[i].addr, from)) {
			if (found >= 0) {
				return -2;
			}
			found = i;
		}
	}
	return found;
}

// Rematch registration uses a new ephemeral probe port; rebind an existing slot by declared GGPO port or IP.
static int FindSlotForRegistration(Endpoint* slots, const sockaddr_in& from, uint16_t declaredPort) {
	int slotIdx = FindSlotByEndpoint(slots, from);
	if (slotIdx >= 0) {
		return slotIdx;
	}

	for (int i = 0; i < 2; i++) {
		if (!slots[i].active) {
			return i;
		}
	}

	if (declaredPort != 0) {
		const uint16_t declaredNetwork = htons(declaredPort);
		for (int i = 0; i < 2; i++) {
			if (slots[i].active && slots[i].addr.sin_port == declaredNetwork) {
				return i;
			}
		}
	}

	const int ipSlot = FindSlotByIp(slots, from);
	if (ipSlot >= 0) {
		return ipSlot;
	}

	return -1;
}

static uint64_t MonotonicMs() {
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
	spdlog::set_default_logger(spdlog::stdout_color_mt("GgpoUdpRelay"));
	spdlog::set_level(spdlog::level::info);

	CLI::App app("SF4e thin UDP GGPO relay");
	uint16_t port = 24456;
	std::string roomToken;
	int idleExitSec = 900;
	app.add_option("--port", port, "UDP listen port")->check(CLI::Range(1, 65535));
	app.add_option("--room-token", roomToken, "32-char hex room token (required)")->required();
	app.add_option("--idle-exit-sec", idleExitSec, "Exit after idle seconds with no traffic (0 disables)");
	CLI11_PARSE(app, argc, argv);

	if (roomToken.size() != 32 || !IsHexToken(roomToken.c_str(), 32)) {
		spdlog::error("--room-token must be 32 hex characters");
		return 1;
	}

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		spdlog::error("socket() failed: {}", strerror(errno));
		return 1;
	}

	int reuse = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in bindAddr = {};
	bindAddr.sin_family = AF_INET;
	bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	bindAddr.sin_port = htons(port);
	if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
		spdlog::error("bind({}) failed: {}", port, strerror(errno));
		close(sock);
		return 1;
	}

	spdlog::info("GgpoUdpRelay listening on UDP {} token={}...", port, roomToken.substr(0, 8));

	Endpoint slots[2];
	uint64_t lastTrafficMs = MonotonicMs();

	for (;;) {
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(sock, &readfds);
		timeval tv = { 0, 200000 };
		int ready = select(sock + 1, &readfds, nullptr, nullptr, &tv);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			spdlog::error("select() failed: {}", strerror(errno));
			break;
		}

		const uint64_t nowMs = MonotonicMs();
		if (idleExitSec > 0 && nowMs - lastTrafficMs > (uint64_t)idleExitSec * 1000ULL) {
			spdlog::info("Idle timeout ({}s), exiting", idleExitSec);
			break;
		}

		if (ready == 0) {
			continue;
		}

		char buf[kMaxPacket];
		sockaddr_in from = {};
		socklen_t fromLen = sizeof(from);
		ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) {
			continue;
		}
		lastTrafficMs = nowMs;

		if (n == 4 && memcmp(buf, kMagicHealth, 4) == 0) {
			sendto(sock, kRespHealth, 5, 0, (sockaddr*)&from, fromLen);
			continue;
		}

		const bool isRegLegacy = n == kRegPacketLenLegacy && memcmp(buf, kMagicReg, 4) == 0;
		const bool isRegV2 = n == kRegPacketLenV2 && memcmp(buf, kMagicReg, 4) == 0;
		if (isRegLegacy || isRegV2) {
			if (!TokenMatches(roomToken.c_str(), buf + 4)) {
				continue;
			}

			uint16_t declaredPort = 0;
			if (isRegV2) {
				declaredPort =
					(uint16_t)(((unsigned char)buf[36] << 8) | (unsigned char)buf[37]);
			}

			const int slotIdx = FindSlotForRegistration(slots, from, declaredPort);
			if (slotIdx < 0) {
				char ipStr[INET_ADDRSTRLEN] = { 0 };
				inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
				spdlog::warn(
					"Registration rejected: no slot for {}:{} declaredGgpoPort={}",
					ipStr,
					ntohs(from.sin_port),
					declaredPort
				);
				continue;
			}

			const bool rematchRebind = slots[slotIdx].active;
			sockaddr_in registered = from;
			if (declaredPort != 0) {
				registered.sin_port = htons(declaredPort);
			}

			CopyEndpoint(slots[slotIdx].addr, registered);
			slots[slotIdx].active = true;

			char ipStr[INET_ADDRSTRLEN] = { 0 };
			inet_ntop(AF_INET, &registered.sin_addr, ipStr, sizeof(ipStr));
			if (rematchRebind) {
				spdlog::info(
					"Rematch rebind slot {} at {}:{} (probe from {}:{})",
					slotIdx,
					ipStr,
					ntohs(registered.sin_port),
					ipStr,
					ntohs(from.sin_port)
				);
			}
			else {
				spdlog::info(
					"Registered slot {} at {}:{} (probe from {}:{})",
					slotIdx,
					ipStr,
					ntohs(registered.sin_port),
					ipStr,
					ntohs(from.sin_port)
				);
			}

			const char* resp = kRespRegistered;
			if (!slots[0].active || !slots[1].active) {
				resp = kRespWaiting;
			}
			sendto(sock, resp, 4, 0, (sockaddr*)&from, fromLen);
			continue;
		}

		int srcSlot = FindSlotByEndpoint(slots, from);
		if (srcSlot < 0) {
			const int ipSlot = FindSlotByIp(slots, from);
			if (ipSlot >= 0) {
				char ipStr[INET_ADDRSTRLEN] = { 0 };
				inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
				spdlog::info(
					"Rebound slot {} to {}:{} (was port {})",
					ipSlot,
					ipStr,
					ntohs(from.sin_port),
					ntohs(slots[ipSlot].addr.sin_port)
				);
				CopyEndpoint(slots[ipSlot].addr, from);
				srcSlot = ipSlot;
			}
		}
		if (srcSlot < 0) {
			continue;
		}
		int dstSlot = srcSlot == 0 ? 1 : 0;
		if (!slots[dstSlot].active) {
			continue;
		}
		sendto(sock, buf, n, 0, (sockaddr*)&slots[dstSlot].addr, sizeof(slots[dstSlot].addr));
	}

	close(sock);
	return 0;
}
