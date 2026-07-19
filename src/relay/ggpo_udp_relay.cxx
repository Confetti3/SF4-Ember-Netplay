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
#include <cstdint>
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
static const int kRegPacketLenV3 = 4 + 32 + 1 + 1 + 8 + 8;
static const unsigned char kRegistrationVersionV3 = 3;
static const int kMaxPacket = 2048;

struct Endpoint {
	sockaddr_in addr = {};
	bool active = false;
};

struct GenerationKey {
	uint64_t readyMessageNum[2] = {};
	bool valid = false;
};

struct GenerationState {
	GenerationKey key = {};
	Endpoint slots[2] = {};
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

static uint64_t ReadU64Be(const unsigned char* data) {
	uint64_t value = 0;
	for (int i = 0; i < 8; i++) {
		value = (value << 8) | data[i];
	}
	return value;
}

static bool SameGeneration(const GenerationKey& a, const GenerationKey& b) {
	return
		a.valid &&
		b.valid &&
		a.readyMessageNum[0] == b.readyMessageNum[0] &&
		a.readyMessageNum[1] == b.readyMessageNum[1];
}

static bool BothSlotsActive(const Endpoint* slots) {
	return slots[0].active && slots[1].active;
}

static void ResetGeneration(GenerationState& state, const GenerationKey& key) {
	state = {};
	state.key = key;
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

// Rematch registration uses a new ephemeral probe port; rebind an existing slot by IP
// (preferred) or a unique declared GGPO port. Never bind both peers to slot 0 just
// because they share the default declared port 23457.
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

	const int ipSlot = FindSlotByIp(slots, from);
	if (ipSlot >= 0) {
		return ipSlot;
	}

	if (declaredPort != 0) {
		const uint16_t declaredNetwork = htons(declaredPort);
		int match = -1;
		int matches = 0;
		for (int i = 0; i < 2; i++) {
			if (slots[i].active && slots[i].addr.sin_port == declaredNetwork) {
				match = i;
				matches++;
			}
		}
		if (matches == 1) {
			return match;
		}
		if (matches > 1) {
			char ipStr[INET_ADDRSTRLEN] = { 0 };
			inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
			spdlog::warn(
				"Registration ambiguous: declaredGgpoPort={} matches {} slots from {}:{}",
				declaredPort,
				matches,
				ipStr,
				ntohs(from.sin_port)
			);
		}
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
	GenerationState activeGeneration;
	GenerationState pendingGeneration;
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
		const bool isRegV3 =
			n == kRegPacketLenV3 &&
			memcmp(buf, kMagicReg, 4) == 0 &&
			(unsigned char)buf[36] == kRegistrationVersionV3;

		if (isRegV3) {
			if (!TokenMatches(roomToken.c_str(), buf + 4)) {
				continue;
			}

			const unsigned char playerSlot = (unsigned char)buf[37];
			if (playerSlot > 1) {
				spdlog::warn("V3 registration rejected: invalid player slot {}", playerSlot);
				continue;
			}

			GenerationKey key;
			key.readyMessageNum[0] = ReadU64Be((const unsigned char*)buf + 38);
			key.readyMessageNum[1] = ReadU64Be((const unsigned char*)buf + 46);
			key.valid = true;

			GenerationState* target = nullptr;
			bool isPending = false;
			if (SameGeneration(activeGeneration.key, key)) {
				target = &activeGeneration;
			}
			else {
				if (!SameGeneration(pendingGeneration.key, key)) {
					ResetGeneration(pendingGeneration, key);
					spdlog::info(
						"V3 pending generation ready=[{},{}]",
						key.readyMessageNum[0],
						key.readyMessageNum[1]
					);
				}
				target = &pendingGeneration;
				isPending = true;
			}

			const bool wasActive = target->slots[playerSlot].active;
			const bool endpointChanged =
				wasActive && !SameEndpoint(target->slots[playerSlot].addr, from);
			CopyEndpoint(target->slots[playerSlot].addr, from);
			target->slots[playerSlot].active = true;

			char ipStr[INET_ADDRSTRLEN] = { 0 };
			inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
			if (!wasActive || endpointChanged) {
				spdlog::info(
					"V3 registered generation=[{},{}] slot={} endpoint={}:{}{}",
					key.readyMessageNum[0],
					key.readyMessageNum[1],
					playerSlot,
					ipStr,
					ntohs(from.sin_port),
					endpointChanged ? " (rebound)" : ""
				);
			}

			if (isPending && BothSlotsActive(pendingGeneration.slots)) {
				activeGeneration = pendingGeneration;
				pendingGeneration = {};
				target = &activeGeneration;
				isPending = false;
				spdlog::info(
					"V3 promoted generation ready=[{},{}]",
					activeGeneration.key.readyMessageNum[0],
					activeGeneration.key.readyMessageNum[1]
				);
			}

			const char* resp =
				!isPending && BothSlotsActive(target->slots)
					? kRespRegistered
					: kRespWaiting;
			sendto(sock, resp, 4, 0, (sockaddr*)&from, fromLen);
			continue;
		}

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

			const bool rematchRebind =
				slots[slotIdx].active && !SameEndpoint(slots[slotIdx].addr, from);
			// Always keep the real NAT 5-tuple from recvfrom. Overwriting with the
			// client's declared GGPO port (usually 23457) black-holes GGPO traffic
			// behind consumer NATs after SF4R succeeds.
			sockaddr_in registered = from;

			CopyEndpoint(slots[slotIdx].addr, registered);
			slots[slotIdx].active = true;

			// Rematch: previous match left both slots active. Invalidate the peer so
			// the first re-registrant gets SF4W until the other side rebinds too —
			// otherwise StartGGPO races against a stale/dead peer endpoint.
			if (rematchRebind) {
				const int peerIdx = slotIdx == 0 ? 1 : 0;
				if (slots[peerIdx].active) {
					slots[peerIdx].active = false;
					spdlog::info(
						"Rematch rebind slot {}; invalidated peer slot {} until they re-register",
						slotIdx,
						peerIdx
					);
				}
			}

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

		Endpoint* forwardingSlots =
			activeGeneration.key.valid ? activeGeneration.slots : slots;
		int srcSlot = FindSlotByEndpoint(forwardingSlots, from);
		if (srcSlot < 0) {
			// Legacy/v2 clients have no explicit player slot, so retain the
			// unambiguous-IP rebind fallback. V3 endpoints must match exactly;
			// this keeps same-public-IP peers from stealing each other's slot.
			const int ipSlot =
				activeGeneration.key.valid ? -1 : FindSlotByIp(forwardingSlots, from);
			if (ipSlot >= 0) {
				char ipStr[INET_ADDRSTRLEN] = { 0 };
				inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
				spdlog::info(
					"Rebound slot {} to {}:{} (was port {})",
					ipSlot,
					ipStr,
					ntohs(from.sin_port),
					ntohs(forwardingSlots[ipSlot].addr.sin_port)
				);
				CopyEndpoint(forwardingSlots[ipSlot].addr, from);
				srcSlot = ipSlot;
			}
		}
		if (srcSlot < 0) {
			continue;
		}
		int dstSlot = srcSlot == 0 ? 1 : 0;
		if (!forwardingSlots[dstSlot].active) {
			continue;
		}
		sendto(
			sock,
			buf,
			n,
			0,
			(sockaddr*)&forwardingSlots[dstSlot].addr,
			sizeof(forwardingSlots[dstSlot].addr)
		);
	}

	close(sock);
	return 0;
}
