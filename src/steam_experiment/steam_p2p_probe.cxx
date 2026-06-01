#include "steam_p2p_payload.hxx"

#include <chrono>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace {

	struct ProbeState {
		HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
		bool acceptIncoming = false;
		bool connected = false;
		bool failed = false;
		bool accepted = false;
	};

	static ProbeState g_state;

	const char* ConnStateName(ESteamNetworkingConnectionState state) {
		switch (state) {
		case k_ESteamNetworkingConnectionState_None:
			return "None";
		case k_ESteamNetworkingConnectionState_Connecting:
			return "Connecting";
		case k_ESteamNetworkingConnectionState_FindingRoute:
			return "FindingRoute";
		case k_ESteamNetworkingConnectionState_Connected:
			return "Connected";
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			return "ClosedByPeer";
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			return "ProblemDetectedLocally";
		default:
			return "Unknown";
		}
	}

	void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
		if (!info) {
			return;
		}

		std::cout
			<< "connection event old=" << ConnStateName(info->m_eOldState)
			<< " new=" << ConnStateName(info->m_info.m_eState)
			<< " reason=" << info->m_info.m_szEndDebug
			<< "\n";

		if (g_state.acceptIncoming && info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
			g_state.conn = info->m_hConn;
			const EResult accept = SteamNetworkingSockets()->AcceptConnection(info->m_hConn);
			g_state.accepted = accept == k_EResultOK;
			std::cout << "AcceptConnection result=" << accept << "\n";
		}

		if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
			g_state.conn = info->m_hConn;
			g_state.connected = true;
		}

		if (
			info->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
			info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally
		) {
			g_state.failed = true;
		}
	}

	bool WriteSteamAppId(uint32_t appId) {
		if (appId == 0) {
			return true;
		}
		std::ofstream f("steam_appid.txt", std::ios::trunc);
		if (!f) {
			std::cerr << "Could not write steam_appid.txt\n";
			return false;
		}
		f << appId;
		std::cout << "Wrote steam_appid.txt appid=" << appId << " for this experimental run\n";
		return true;
	}

	bool InitSteam() {
		if (!SteamAPI_Init()) {
			std::cerr << "SteamAPI_Init failed. Launch through Steam or provide a valid steam_appid.txt for testing.\n";
			return false;
		}
		if (!SteamUser() || !SteamUser()->BLoggedOn()) {
			std::cerr << "Steam initialized, but no logged-in Steam user is available.\n";
			return false;
		}
		SteamNetworkingUtils()->InitRelayNetworkAccess();
		std::cout << "Steam initialized\n";
		std::cout << "SteamID: " << SteamUser()->GetSteamID().ConvertToUint64() << "\n";
		std::cout << "Persona: " << SteamFriends()->GetPersonaName() << "\n";
		return true;
	}

	void PumpCallbacks() {
		SteamAPI_RunCallbacks();
		if (SteamNetworkingSockets()) {
			SteamNetworkingSockets()->RunCallbacks();
		}
	}

	void PrintFriends(bool onlyInSf4) {
		const int flags = k_EFriendFlagImmediate;
		const int count = SteamFriends()->GetFriendCount(flags);
		std::cout << "Friends: " << count << "\n";
		for (int i = 0; i < count; ++i) {
			CSteamID friendId = SteamFriends()->GetFriendByIndex(i, flags);
			FriendGameInfo_t gameInfo;
			const bool inGame = SteamFriends()->GetFriendGamePlayed(friendId, &gameInfo);
			const bool inSf4 = inGame && gameInfo.m_gameID.AppID() == 45760;
			if (onlyInSf4 && !inSf4) {
				continue;
			}
			std::cout
				<< friendId.ConvertToUint64()
				<< " | " << SteamFriends()->GetFriendPersonaName(friendId)
				<< " | state=" << SteamFriends()->GetFriendPersonaState(friendId)
				<< " | inUSF4=" << (inSf4 ? "yes" : "no")
				<< "\n";
		}
	}

	bool SendInvite(uint64_t targetSteamId, const sf4e::steam_experiment::SteamInvitePayload& payload) {
		SteamNetworkingIdentity identity;
		identity.SetSteamID64(targetSteamId);
		const std::string encoded = sf4e::steam_experiment::EncodeInvite(payload);
		EResult result = SteamNetworkingMessages()->SendMessageToUser(
			identity,
			encoded.data(),
			(uint32)encoded.size(),
			k_nSteamNetworkingSend_Reliable,
			0
		);
		std::cout << "SendMessageToUser result=" << result << "\n";
		return result == k_EResultOK;
	}

	void PollSteamMessages(int seconds) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		while (std::chrono::steady_clock::now() < deadline) {
			PumpCallbacks();

			SteamNetworkingMessage_t* msgs[16] = { nullptr };
			const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, msgs, 16);
			for (int i = 0; i < count; ++i) {
				std::string body((const char*)msgs[i]->m_pData, (size_t)msgs[i]->m_cbSize);
				std::cout << "message from " << msgs[i]->m_identityPeer.GetSteamID64() << ": " << body << "\n";

				sf4e::steam_experiment::SteamInvitePayload invite;
				std::string err;
				if (sf4e::steam_experiment::DecodeInvite(body, invite, err)) {
					std::cout
						<< "decoded invite role=" << invite.role
						<< " virtualPort=" << invite.virtualPort
						<< " buildGit=" << invite.buildGit
						<< "\n";
				}
				else {
					std::cout << "message was not an sf4e invite: " << err << "\n";
				}
				msgs[i]->Release();
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	int RunSocketProbe(bool host, uint64_t peerSteamId, int virtualPort, int seconds) {
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnConnectionStatusChanged);

		HSteamListenSocket listenSock = k_HSteamListenSocket_Invalid;
		if (host) {
			g_state.acceptIncoming = true;
			listenSock = SteamNetworkingSockets()->CreateListenSocketP2P(virtualPort, 1, &opt);
			if (listenSock == k_HSteamListenSocket_Invalid) {
				std::cerr << "CreateListenSocketP2P failed\n";
				return 1;
			}
			std::cout << "Listening with CreateListenSocketP2P virtualPort=" << virtualPort << "\n";
		}
		else {
			SteamNetworkingIdentity identity;
			identity.SetSteamID64(peerSteamId);
			g_state.conn = SteamNetworkingSockets()->ConnectP2P(identity, virtualPort, 1, &opt);
			if (g_state.conn == k_HSteamNetConnection_Invalid) {
				std::cerr << "ConnectP2P failed to create a connection\n";
				return 1;
			}
			std::cout << "Connecting with ConnectP2P peer=" << peerSteamId << " virtualPort=" << virtualPort << "\n";
		}

		bool sentProbe = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		while (std::chrono::steady_clock::now() < deadline && !g_state.failed) {
			PumpCallbacks();

			if (g_state.connected && g_state.conn != k_HSteamNetConnection_Invalid && !sentProbe) {
				const char* ping = "sf4e-steam-p2p-probe";
				int64 messageNumber = 0;
				EResult send = SteamNetworkingSockets()->SendMessageToConnection(
					g_state.conn,
					ping,
					(uint32)strlen(ping),
					k_nSteamNetworkingSend_Reliable,
					&messageNumber
				);
				std::cout << "SendMessageToConnection result=" << send << " messageNumber=" << messageNumber << "\n";
				sentProbe = true;
			}

			if (g_state.conn != k_HSteamNetConnection_Invalid) {
				ISteamNetworkingMessage* msgs[8] = { nullptr };
				const int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(g_state.conn, msgs, 8);
				for (int i = 0; i < count; ++i) {
					std::string body((const char*)msgs[i]->m_pData, (size_t)msgs[i]->m_cbSize);
					std::cout << "socket message: " << body << "\n";
					msgs[i]->Release();
				}
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		if (g_state.conn != k_HSteamNetConnection_Invalid) {
			SteamNetworkingSockets()->CloseConnection(g_state.conn, 0, "probe complete", false);
		}
		if (listenSock != k_HSteamListenSocket_Invalid) {
			SteamNetworkingSockets()->CloseListenSocket(listenSock);
		}

		if (!g_state.connected) {
			std::cerr << "P2P socket did not reach Connected within " << seconds << "s\n";
			return 2;
		}
		std::cout << "P2P socket reached Connected\n";
		return 0;
	}

} // namespace

int main(int argc, char** argv) {
	CLI::App app("Experimental Steamworks P2P probe for SF4 Netplay Launcher. Does not launch USF4.");
	bool listFriends = false;
	bool onlySf4Friends = false;
	bool listen = false;
	uint64_t connectSteamId = 0;
	uint64_t inviteSteamId = 0;
	int virtualPort = 7;
	int pollSeconds = 30;
	uint32_t writeAppId = 0;
	std::string sidecarHash = "unknown-sidecar-hash";
	std::string buildGit = "unknown-build";
	std::string sessionToken = "manual-test-token";

	app.add_flag("--list-friends", listFriends, "Print Steam friends visible to this account.");
	app.add_flag("--only-sf4", onlySf4Friends, "Only print friends currently reported as running USF4.");
	app.add_flag("--listen", listen, "Create a Steam P2P listen socket.");
	app.add_option("--connect", connectSteamId, "ConnectP2P to a peer SteamID64.")->check(CLI::NonNegativeNumber);
	app.add_option("--send-invite", inviteSteamId, "Send an sf4e Steam P2P invite payload to a SteamID64.")->check(CLI::NonNegativeNumber);
	app.add_option("--virtual-port", virtualPort, "Steam P2P virtual port (0-999).")->check(CLI::Range(0, 999));
	app.add_option("--poll-seconds", pollSeconds, "Seconds to pump Steam callbacks/messages.")->check(CLI::Range(1, 600));
	app.add_option("--write-appid", writeAppId, "Explicitly write steam_appid.txt for a local experiment, e.g. 45760.")->check(CLI::NonNegativeNumber);
	app.add_option("--sidecar-hash", sidecarHash, "Sidecar hash to include in invite payload.");
	app.add_option("--build-git", buildGit, "Build git string to include in invite payload.");
	app.add_option("--session-token", sessionToken, "Session token to include in invite payload.");
	CLI11_PARSE(app, argc, argv);

	if (!WriteSteamAppId(writeAppId)) {
		return 1;
	}
	if (!InitSteam()) {
		return 1;
	}

	if (listFriends) {
		PrintFriends(onlySf4Friends);
	}

	if (inviteSteamId != 0) {
		sf4e::steam_experiment::SteamInvitePayload payload;
		payload.senderSteamId = SteamUser()->GetSteamID().ConvertToUint64();
		payload.virtualPort = virtualPort;
		payload.role = listen ? "host" : "join";
		payload.sidecarHash = sidecarHash;
		payload.buildGit = buildGit;
		payload.sessionToken = sessionToken;
		std::string err;
		if (!sf4e::steam_experiment::ValidateInvite(payload, err)) {
			std::cerr << "Invite payload invalid: " << err << "\n";
			SteamAPI_Shutdown();
			return 1;
		}
		if (!SendInvite(inviteSteamId, payload)) {
			SteamAPI_Shutdown();
			return 1;
		}
	}

	int rc = 0;
	if (listen || connectSteamId != 0) {
		rc = RunSocketProbe(listen, connectSteamId, virtualPort, pollSeconds);
	}
	else {
		PollSteamMessages(pollSeconds);
	}

	SteamAPI_Shutdown();
	return rc;
}
