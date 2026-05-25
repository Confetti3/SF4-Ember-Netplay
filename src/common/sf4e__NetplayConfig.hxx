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
	static const int SF4E_NETPLAY_CONFIG_VERSION = 2;
	static const int NETPLAY_SESSION_HOST_LEN = 64;
	static const int NETPLAY_ROOM_KEY_LEN = 16;
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
		uint8_t useRelay = 1;
		uint8_t devOverlay = 0;
		uint8_t deviceType = 0xff;
		uint8_t deviceIdx = 0xff;
		// When set, host joins a central session relay (no local SessionServer listen).
		uint8_t useCentralSession = 0;
	};

	inline bool NetplayConfigIsActive(const NetplayConfig& cfg) {
		return cfg.mode == (int)NetplayMode::Host || cfg.mode == (int)NetplayMode::Join;
	}

} // namespace sf4e
