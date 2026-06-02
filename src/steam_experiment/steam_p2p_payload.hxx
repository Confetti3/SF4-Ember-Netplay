#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace sf4e {
namespace steam_experiment {

	static const int STEAM_P2P_INVITE_VERSION = 1;
	static const int STEAM_P2P_LAUNCH_READY_VERSION = 2;

	struct SteamInvitePayload {
		int version = STEAM_P2P_INVITE_VERSION;
		uint64_t senderSteamId = 0;
		int virtualPort = 0;
		std::string role;
		std::string sidecarHash;
		std::string buildGit;
		std::string sessionToken;
	};

	nlohmann::json ToJson(const SteamInvitePayload& payload);
	std::string EncodeInvite(const SteamInvitePayload& payload);
	bool DecodeInvite(const std::string& text, SteamInvitePayload& outPayload, std::string& outError);
	bool ValidateInvite(const SteamInvitePayload& payload, std::string& outError);
	bool CompareInviteToLocalBuild(
		const SteamInvitePayload& payload,
		const char* localSidecarHash,
		const char* localBuildGit,
		std::string& outError
	);

	struct SteamLaunchReadyPayload {
		int version = STEAM_P2P_LAUNCH_READY_VERSION;
		uint64_t senderSteamId = 0;
		std::string sessionToken;
	};

	std::string EncodeLaunchReady(const SteamLaunchReadyPayload& payload);
	bool DecodeLaunchReady(const std::string& text, SteamLaunchReadyPayload& outPayload, std::string& outError);

	static const int STEAM_P2P_LAUNCH_COMMIT_VERSION = 1;

	struct SteamLaunchCommitPayload {
		int version = STEAM_P2P_LAUNCH_COMMIT_VERSION;
		uint64_t senderSteamId = 0;
		std::string sessionToken;
	};

	std::string EncodeLaunchCommit(const SteamLaunchCommitPayload& payload);
	bool DecodeLaunchCommit(const std::string& text, SteamLaunchCommitPayload& outPayload, std::string& outError);

} // namespace steam_experiment
} // namespace sf4e
