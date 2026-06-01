#include "steam_p2p_payload.hxx"

#include <iostream>
#include <string>

using sf4e::steam_experiment::DecodeInvite;
using sf4e::steam_experiment::EncodeInvite;
using sf4e::steam_experiment::SteamInvitePayload;
using sf4e::steam_experiment::CompareInviteToLocalBuild;
using sf4e::steam_experiment::ValidateInvite;

namespace {

	bool Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

} // namespace

int main() {
	int failures = 0;

	SteamInvitePayload original;
	original.senderSteamId = 76561198000000000ULL;
	original.virtualPort = 7;
	original.role = "host";
	original.sidecarHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	original.buildGit = "abcdef0";
	original.sessionToken = "test-token-1234";

	std::string err;
	failures += Expect(ValidateInvite(original, err), "valid invite should validate") ? 0 : 1;

	SteamInvitePayload decoded;
	const std::string encoded = EncodeInvite(original);
	failures += Expect(DecodeInvite(encoded, decoded, err), "encoded invite should decode") ? 0 : 1;
	failures += Expect(decoded.senderSteamId == original.senderSteamId, "SteamID round trips") ? 0 : 1;
	failures += Expect(decoded.virtualPort == original.virtualPort, "virtual port round trips") ? 0 : 1;
	failures += Expect(decoded.role == original.role, "role round trips") ? 0 : 1;
	failures += Expect(decoded.sidecarHash == original.sidecarHash, "sidecar hash round trips") ? 0 : 1;
	failures += Expect(decoded.buildGit == original.buildGit, "build git round trips") ? 0 : 1;
	failures += Expect(decoded.sessionToken == original.sessionToken, "session token round trips") ? 0 : 1;

	SteamInvitePayload badRole = original;
	badRole.role = "spectator";
	failures += Expect(!ValidateInvite(badRole, err), "invalid role should fail") ? 0 : 1;

	SteamInvitePayload badToken = original;
	badToken.sessionToken = "short";
	failures += Expect(!ValidateInvite(badToken, err), "short token should fail") ? 0 : 1;

	SteamInvitePayload badPort = original;
	badPort.virtualPort = 1000;
	failures += Expect(!ValidateInvite(badPort, err), "out of range virtual port should fail") ? 0 : 1;

	failures += Expect(!DecodeInvite("{\"cmd\":\"other\"}", decoded, err), "unexpected command should fail") ? 0 : 1;

	failures += Expect(
		CompareInviteToLocalBuild(original, original.sidecarHash.c_str(), original.buildGit.c_str(), err),
		"matching local build should pass"
	) ? 0 : 1;
	failures += Expect(
		!CompareInviteToLocalBuild(original, "wrong-hash", original.buildGit.c_str(), err),
		"sidecar mismatch should fail"
	) ? 0 : 1;

	if (failures != 0) {
		std::cerr << failures << " payload test(s) failed\n";
		return 1;
	}

	std::cout << "Steam P2P payload tests passed\n";
	return 0;
}
