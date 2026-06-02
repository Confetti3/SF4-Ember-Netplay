#include "steam_p2p_payload.hxx"

#include <sstream>

namespace sf4e {
namespace steam_experiment {

	nlohmann::json ToJson(const SteamInvitePayload& payload) {
		nlohmann::json j;
		j["cmd"] = "sf4e_steam_p2p_invite";
		j["version"] = payload.version;
		j["senderSteamId"] = std::to_string(payload.senderSteamId);
		j["virtualPort"] = payload.virtualPort;
		j["role"] = payload.role;
		j["sidecarHash"] = payload.sidecarHash;
		j["buildGit"] = payload.buildGit;
		j["sessionToken"] = payload.sessionToken;
		return j;
	}

	std::string EncodeInvite(const SteamInvitePayload& payload) {
		return ToJson(payload).dump();
	}

	bool DecodeInvite(const std::string& text, SteamInvitePayload& outPayload, std::string& outError) {
		try {
			nlohmann::json j = nlohmann::json::parse(text);
			if (j.value("cmd", "") != "sf4e_steam_p2p_invite") {
				outError = "unexpected command";
				return false;
			}

			SteamInvitePayload payload;
			payload.version = j.value("version", 0);
			payload.senderSteamId = std::stoull(j.value("senderSteamId", "0"));
			payload.virtualPort = j.value("virtualPort", 0);
			payload.role = j.value("role", "");
			payload.sidecarHash = j.value("sidecarHash", "");
			payload.buildGit = j.value("buildGit", "");
			payload.sessionToken = j.value("sessionToken", "");

			if (!ValidateInvite(payload, outError)) {
				return false;
			}
			outPayload = payload;
			return true;
		}
		catch (const std::exception& e) {
			outError = e.what();
			return false;
		}
		catch (...) {
			outError = "unknown parse error";
			return false;
		}
	}

	bool ValidateInvite(const SteamInvitePayload& payload, std::string& outError) {
		if (payload.version != STEAM_P2P_INVITE_VERSION) {
			outError = "unsupported invite version";
			return false;
		}
		if (payload.senderSteamId == 0) {
			outError = "missing sender SteamID";
			return false;
		}
		if (payload.virtualPort < 0 || payload.virtualPort >= 1000) {
			outError = "virtual port must be between 0 and 999";
			return false;
		}
		if (payload.role != "host" && payload.role != "join") {
			outError = "role must be host or join";
			return false;
		}
		if (payload.sidecarHash.empty()) {
			outError = "missing sidecar hash";
			return false;
		}
		if (payload.buildGit.empty()) {
			outError = "missing build git";
			return false;
		}
		if (payload.sessionToken.size() < 8) {
			outError = "session token too short";
			return false;
		}
		outError.clear();
		return true;
	}

	bool CompareInviteToLocalBuild(
		const SteamInvitePayload& payload,
		const char* localSidecarHash,
		const char* localBuildGit,
		std::string& outError
	) {
		if (!localSidecarHash || !localSidecarHash[0]) {
			outError = "local sidecar hash unavailable";
			return false;
		}
		if (!localBuildGit || !localBuildGit[0]) {
			outError = "local build version unavailable";
			return false;
		}
		if (payload.sidecarHash != localSidecarHash) {
			outError = "sidecar hash mismatch";
			return false;
		}
		if (payload.buildGit != localBuildGit) {
			outError = "build version mismatch";
			return false;
		}
		outError.clear();
		return true;
	}

	std::string EncodeLaunchReady(const SteamLaunchReadyPayload& payload) {
		nlohmann::json j;
		j["cmd"] = "sf4e_steam_p2p_launch_ready";
		j["version"] = payload.version;
		j["senderSteamId"] = std::to_string(payload.senderSteamId);
		j["sessionToken"] = payload.sessionToken;
		return j.dump();
	}

	bool DecodeLaunchReady(const std::string& text, SteamLaunchReadyPayload& outPayload, std::string& outError) {
		try {
			nlohmann::json j = nlohmann::json::parse(text);
			if (j.value("cmd", "") != "sf4e_steam_p2p_launch_ready") {
				outError = "unexpected command";
				return false;
			}
			SteamLaunchReadyPayload payload;
			payload.version = j.value("version", 0);
			payload.senderSteamId = std::stoull(j.value("senderSteamId", "0"));
			payload.sessionToken = j.value("sessionToken", "");
			if (payload.version != STEAM_P2P_LAUNCH_READY_VERSION) {
				outError = "unsupported launch-ready version";
				return false;
			}
			if (payload.senderSteamId == 0) {
				outError = "missing sender SteamID";
				return false;
			}
			if (payload.sessionToken.size() < 8) {
				outError = "missing session token on launch-ready";
				return false;
			}
			outPayload = payload;
			outError.clear();
			return true;
		}
		catch (const std::exception& e) {
			outError = e.what();
			return false;
		}
		catch (...) {
			outError = "unknown parse error";
			return false;
		}
	}

} // namespace steam_experiment
} // namespace sf4e
