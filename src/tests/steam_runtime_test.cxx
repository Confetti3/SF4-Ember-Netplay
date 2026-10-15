// Wrapper tests for SteamRuntime.hxx using a fake ISteamUser017/
// ISteamFriends014 instead of the real steam_api.dll (see
// docs/design/STEAM_IDENTITY_AND_INVITES.md, "Wrapper tests with a fake
// interface"). Vtable/ABI probing against the real DLL is explicitly kept
// out of this pure unit test and belongs in a separate live test executable.

#include "../platform/SteamRuntime.hxx"

#include <cstring>
#include <map>
#include <string>

#include "test_support.hxx"

using namespace sf4e;
using namespace sf4e::steam_abi;

namespace {

std::uint64_t ValidIndividualSteamId(std::uint32_t accountId) {
	return (std::uint64_t(1) << 56) | (std::uint64_t(1) << 52) | accountId;
}

class FakeUser : public ISteamUser017 {
public:
	bool loggedOn = true;
	std::uint64_t steamId = ValidIndividualSteamId(12345);
	void Unused00() override {}
	bool BLoggedOn() override { return loggedOn; }
	CSteamID GetSteamID() override { return CSteamID(steamId); }
};

class FakeFriends : public ISteamFriends014 {
public:
	std::string persona = "Ryu";
	int friendCount = 0;
	std::map<int, std::pair<std::uint64_t, std::string>> explicitFriends;
	int setRichPresenceCalls = 0;
	std::vector<std::pair<std::string, std::string>> richPresenceLog;
	bool setRichPresenceReturn = true;
	bool inviteReturn = true;
	std::uint64_t lastInviteTarget = 0;
	std::string lastInviteConnect;
	mutable std::string lastQueriedName;

	const char* GetPersonaName() override { return persona.c_str(); }
	void Unused01() override {} void Unused02() override {}
	int GetFriendCount(int) override { return friendCount; }
	CSteamID GetFriendByIndex(int i, int) override {
		const auto it = explicitFriends.find(i);
		if (it != explicitFriends.end()) return CSteamID(it->second.first);
		return CSteamID(ValidIndividualSteamId(1000000 + static_cast<std::uint32_t>(i))); // synthesized, no backing storage
	}
	void Unused05() override {} void Unused06() override {}
	const char* GetFriendPersonaName(CSteamID id) override {
		for (const auto& kv : explicitFriends)
			if (kv.second.first == id.ConvertToUint64()) { lastQueriedName = kv.second.second; return lastQueriedName.c_str(); }
		lastQueriedName = "Friend";
		return lastQueriedName.c_str();
	}
	void Unused08() override {} void Unused09() override {} void Unused10() override {} void Unused11() override {}
	void Unused12() override {} void Unused13() override {} void Unused14() override {} void Unused15() override {}
	void Unused16() override {} void Unused17() override {} void Unused18() override {} void Unused19() override {}
	void Unused20() override {} void Unused21() override {}
	bool ActivateGameOverlay(const char*) override { return true; }
	void Unused23() override {} void Unused24() override {} void Unused25() override {} void Unused26() override {}
	void Unused27() override {} void Unused28() override {} void Unused29() override {} void Unused30() override {}
	bool RequestUserInformation(CSteamID, bool) override { return true; }
	void Unused32() override {} void Unused33() override {} void Unused34() override {} void Unused35() override {}
	void Unused36() override {}
	bool SetRichPresence(const char* key, const char* value) override {
		++setRichPresenceCalls;
		richPresenceLog.emplace_back(key, value);
		return setRichPresenceReturn;
	}
	void ClearRichPresence() override {}
	void Unused39() override {} void Unused40() override {} void Unused41() override {} void Unused42() override {}
	bool InviteUserToGame(CSteamID steamIDFriend, const char* pchConnectString) override {

		lastInviteTarget = steamIDFriend.ConvertToUint64();
		lastInviteConnect = pchConnectString;
		return inviteReturn;
	}
};

} // namespace

static void TestAttachRequiresLoggedOnAndAValidId() {
	FakeUser user; FakeFriends friends;
	steam::Runtime runtime;
	CHECK(!runtime.AttachWithInterfaces(nullptr, &friends));
	CHECK(!runtime.AttachWithInterfaces(&user, nullptr));
	user.loggedOn = false;
	CHECK(!runtime.AttachWithInterfaces(&user, &friends) && !runtime.Attached());
	user.loggedOn = true; user.steamId = 0;
	CHECK(!runtime.AttachWithInterfaces(&user, &friends));
	user.steamId = ValidIndividualSteamId(12345);
	CHECK(runtime.AttachWithInterfaces(&user, &friends) && runtime.Attached());
}

static void TestLocalUserCopiesIdentityImmediately() {
	FakeUser user; FakeFriends friends;
	friends.persona = "Ry\x01u\x7F!"; // a control character and DEL Steam itself would never remove
	steam::Runtime runtime;
	CHECK(runtime.AttachWithInterfaces(&user, &friends));
	const auto identity = runtime.LocalUser();
	CHECK(identity.available && identity.loggedOn);
	CHECK(identity.steamId == user.steamId);
	CHECK(identity.personaName == "Ryu!"); // sanitized, never the raw persona text
	user.loggedOn = false;
	CHECK(!runtime.LocalUser().available);
}

static void TestFriendsCopiesSkipsInvalidAndBoundsRows() {
	FakeUser user; FakeFriends friends;
	steam::Runtime runtime;
	CHECK(runtime.AttachWithInterfaces(&user, &friends));
	friends.friendCount = 3;
	friends.explicitFriends[0] = { ValidIndividualSteamId(1), "Chun-Li" };
	friends.explicitFriends[1] = { 0, "Invalid" }; // not a valid individual SteamID: skipped
	friends.explicitFriends[2] = { ValidIndividualSteamId(2), "Chun-Li" }; // duplicate names are safe
	const auto rows = runtime.Friends();
	CHECK(rows.size() == 2);
	CHECK(rows[0].personaName == "Chun-Li" && rows[1].personaName == "Chun-Li");
	CHECK(rows[0].steamId != rows[1].steamId);
	// Steam reporting far more friends than expected never grows unbounded.
	friends.friendCount = 100000;
	friends.explicitFriends.clear();
	CHECK(runtime.Friends().size() == static_cast<std::size_t>(steam::MaximumFriendRows));
}

static void TestPublishCoalescesAndEnforcesRichPresenceLimits() {
	FakeUser user; FakeFriends friends;
	steam::Runtime runtime;
	CHECK(runtime.AttachWithInterfaces(&user, &friends));
	CHECK(runtime.Publish("In an Ember room (1/16)", "ember://join/7K3M-0X1R-T9PZ"));
	CHECK(friends.setRichPresenceCalls == 2); // "status" then "connect"
	CHECK(runtime.Publish("In an Ember room (1/16)", "ember://join/7K3M-0X1R-T9PZ"));
	CHECK(friends.setRichPresenceCalls == 2); // unchanged: no Steam call when nothing differs
	CHECK(runtime.Publish("In an Ember room (2/16)", "ember://join/7K3M-0X1R-T9PZ"));
	CHECK(friends.setRichPresenceCalls == 4);
	// Over Rich Presence's documented 256-byte value limit: refused outright.
	CHECK(!runtime.Publish(std::string(256, 'a'), ""));
	CHECK(friends.setRichPresenceCalls == 4);
}

static void TestClearPresencePublishesTargetedEmptyValues() {
	FakeUser user; FakeFriends friends;
	steam::Runtime runtime;
	CHECK(runtime.AttachWithInterfaces(&user, &friends));
	CHECK(runtime.Publish("In Ember menus", ""));
	runtime.ClearPresence();
	CHECK(friends.richPresenceLog.size() >= 2);
	const auto& last = friends.richPresenceLog.back();
	const auto& secondLast = friends.richPresenceLog[friends.richPresenceLog.size() - 2];
	CHECK(secondLast.first == "connect" && secondLast.second.empty());
	CHECK(last.first == "status" && last.second.empty());
	// Publishing the same status again after a clear is not coalesced away.
	const int before = friends.setRichPresenceCalls;
	CHECK(runtime.Publish("In Ember menus", ""));
	CHECK(friends.setRichPresenceCalls == before + 2);
}

static void TestInviteValidatesTargetAndConnectString() {
	FakeUser user; FakeFriends friends;
	steam::Runtime runtime;
	CHECK(runtime.AttachWithInterfaces(&user, &friends));
	CHECK(!runtime.Invite(0, "ember://join/7K3M-0X1R-T9PZ"));
	CHECK(!runtime.Invite(ValidIndividualSteamId(1), ""));
	CHECK(!runtime.Invite(ValidIndividualSteamId(1), std::string(256, 'a')));
	CHECK(runtime.Invite(ValidIndividualSteamId(1), "ember://join/7K3M-0X1R-T9PZ"));
	CHECK(friends.lastInviteTarget == ValidIndividualSteamId(1));
	CHECK(friends.lastInviteConnect == "ember://join/7K3M-0X1R-T9PZ");
	friends.inviteReturn = false;
	CHECK(!runtime.Invite(ValidIndividualSteamId(1), "ember://join/7K3M-0X1R-T9PZ"));
}

static void TestJoinRequestMailboxIsNewestWinsAndStrict() {
	steam::Runtime runtime;
	steam::JoinRequest request;
	CHECK(!runtime.TakeJoinRequest(request));
	const char good[] = "ember://join/7K3M-0X1R-T9PZ";
	runtime.OnJoinRequested(ValidIndividualSteamId(9), good, sizeof(good));
	// A second, malformed callback payload does not erase the pending one.
	char noNul[8] = { 'e', 'm', 'b', 'e', 'r', ':', '/', '/' };
	runtime.OnJoinRequested(ValidIndividualSteamId(9), noNul, sizeof(noNul));
	CHECK(runtime.TakeJoinRequest(request));
	CHECK(request.code == "7K3M0X1RT9PZ" && request.friendSteamId == ValidIndividualSteamId(9));
	CHECK(!runtime.TakeJoinRequest(request)); // drained
	// Newest of two valid requests wins.
	const char first[] = "ember://join/7K3M-0X1R-T9PZ";
	const char second[] = "ember://join/ABCD-EFGH-JKMN";
	runtime.OnJoinRequested(ValidIndividualSteamId(1), first, sizeof(first));
	runtime.OnJoinRequested(ValidIndividualSteamId(2), second, sizeof(second));
	CHECK(runtime.TakeJoinRequest(request));
	CHECK(request.code == "ABCDEFGHJKMN" && request.friendSteamId == ValidIndividualSteamId(2));
}

int main() {
	TestAttachRequiresLoggedOnAndAValidId();
	TestLocalUserCopiesIdentityImmediately();
	TestFriendsCopiesSkipsInvalidAndBoundsRows();
	TestPublishCoalescesAndEnforcesRichPresenceLimits();
	TestClearPresencePublishesTargetedEmptyValues();
	TestInviteValidatesTargetAndConnectString();
	TestJoinRequestMailboxIsNewestWinsAndStrict();
	std::printf("steam_runtime_test: all tests passed\n");
	return 0;
}
