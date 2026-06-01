#pragma once

#include <nlohmann/json.hpp>

namespace sf4e {
namespace launcher {
namespace steam_p2p {

	nlohmann::json BuildStatusJson();
	nlohmann::json BuildInfoJson();
	nlohmann::json RefreshFriendsJson(bool onlyInSf4);
	nlohmann::json PrepareHostJson(
		unsigned long long targetSteamId,
		int virtualPort,
		const char* sidecarHash,
		const char* buildGit,
		const char* sessionToken
	);
	nlohmann::json SendInviteJson(unsigned long long targetSteamId, int virtualPort, const char* sidecarHash, const char* buildGit, const char* sessionToken);
	nlohmann::json PollMessagesJson();
	nlohmann::json ListenJson(int virtualPort);
	nlohmann::json ConnectJson(unsigned long long targetSteamId, int virtualPort);
	nlohmann::json CloseJson();

} // namespace steam_p2p
} // namespace launcher
} // namespace sf4e
