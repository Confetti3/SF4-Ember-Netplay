#pragma once

#include <cstdint>

namespace sf4e {

	enum class NetplayMode : int {
		Idle = 0,
		Host = 1,
		Join = 2,
	};

	// Fixed-size config copied Launcher -> Sidecar via Detours payload.
	// Keep POD and stable layout; bump SF4E_NETPLAY_CONFIG_VERSION if fields change.
	static const int SF4E_NETPLAY_CONFIG_VERSION = 9;
	static const int NETPLAY_SESSION_HOST_LEN = 64;
	static const int NETPLAY_ROOM_KEY_LEN = 32;
	static const int NETPLAY_DISPLAY_NAME_LEN = 32;

	struct NetplayConfig {
		int version = SF4E_NETPLAY_CONFIG_VERSION;
		int mode = (int)NetplayMode::Idle;
		char sessionHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
		uint16_t sessionPort = 23456;
		char roomKey[NETPLAY_ROOM_KEY_LEN] = { 0 };
		char displayName[NETPLAY_DISPLAY_NAME_LEN] = { 0 };
		uint8_t inputDelay = 2;
		uint16_t ggpoPort = 23457;
		uint8_t editionSelect = 1;
		int roundCount = 3;
		int roundTimeIntegral = 99;
		uint8_t useRelay = 0;
		uint8_t devOverlay = 0;
		uint8_t deviceType = 0xff;
		uint8_t deviceIdx = 0xff;
		// Reserved obsolete payload fields; ignored by the Iroh-only application.
		uint8_t useCentralSession = 0;
		// Reserved broker code storage; never imported into active preferences.
		char relayRoomCode[NETPLAY_ROOM_KEY_LEN] = { 0 };
		// Reserved transport selector; the application always uses Iroh.
		uint8_t ggpoTransport = 0;
		uint8_t playerRole = 0; // 1=host, 2=guest
		uint16_t ggpoRemotePort = 0;
		char ggpoRemoteHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
		char matchId[33] = { 0 };
		char ggpoRoomToken[33] = { 0 };
		// Reserved: formerly the GGPO disconnect tolerance, which the sidecar now
		// owns (common/GgpoDisconnectTolerance.hxx). The launcher never set them.
		uint16_t reservedGgpoDisconnectTimeoutMs = 0;
		uint16_t reservedGgpoDisconnectNotifyMs = 0;
		// Vestigial: formerly requested a "training room" with endless
		// sparring settings. Round count and timer are now set directly in
		// the overlay's lobby panel, which offers long values outright, so
		// nothing sets or reads this. Kept only to keep the struct layout
		// stable for the version gate below.
		uint8_t trainingMode = 0;
	};

	inline bool NetplayConfigIsActive(const NetplayConfig& cfg) {
		return cfg.mode == (int)NetplayMode::Host || cfg.mode == (int)NetplayMode::Join;
	}

} // namespace sf4e
