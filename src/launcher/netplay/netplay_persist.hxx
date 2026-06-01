#pragma once

#include "../common/sf4e__NetplayConfig.hxx"

namespace sf4e {
namespace launcher {

	struct PersistedSettings {
		char displayName[NETPLAY_DISPLAY_NAME_LEN] = "Player";
		uint8_t inputDelay = 2;
		uint16_t sessionPort = 23456;
		uint16_t ggpoPort = 23457;
		uint8_t editionSelect = 1;
		int roundCount = 3;
		int roundTimeIntegral = 99;
		uint8_t useRelay = 1;
		char lastJoinHost[128] = { 0 };
		char lastAdvertiseHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
		uint8_t simpleUi = 1;
		uint8_t defaultConnectMethod = 1; // 0=relay, 1=direct, 2=autoNat host helper
		uint16_t relaySessionPort = 0;
		char brokerBaseUrl[256] = "https://74-208-200-95.nip.io";
		char relayRoomCode[32] = { 0 };
		char relayHostSecret[128] = { 0 };
	};

	bool LoadPersistedSettings(PersistedSettings& out);
	bool SavePersistedSettings(const PersistedSettings& in);
	bool GetConfigFilePath(wchar_t* outPath, int outPathChars);

} // namespace launcher
} // namespace sf4e
