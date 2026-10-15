// Unit tests for SteamPresence.hxx: presence derivation, without
// steam_api.dll or any Steam callback machinery.

#include "../netplay/SteamPresence.hxx"

#include "test_support.hxx"

using namespace sf4e;

static room::Member Local(room::MemberStatus status) {
	room::Member m;
	m.id = 2;
	m.host = false;
	m.status = status;
	return m;
}

static steam_presence::Input Base() {
	steam_presence::Input in;
	in.attached = true;
	in.enabled = true;
	in.invites = true;
	in.session.room = netplay::RoomState::Joined;
	in.session.control = netplay::Health::Healthy;
	in.room.members.push_back(Local(room::MemberStatus::Idle));
	in.room.localMember = 2;
	in.room.capacity = 16;
	in.code = "7K3M0X1RT9PZ";
	in.codeGeneration = in.session.generation.room; // 0 by default: matches
	return in;
}

static void TestNoRuntimeOrDisabledNeverPublishes() {
	steam_presence::Input in = Base();
	in.attached = false;
	CHECK(!steam_presence::Describe(in).publish);
	in = Base();
	in.enabled = false;
	CHECK(!steam_presence::Describe(in).publish);
}

static void TestMainMenuAndOfflinePlay() {
	steam_presence::Input in;
	in.attached = true;
	// Not in a room, no match: the starting state.
	const auto idle = steam_presence::Describe(in);
	CHECK(idle.publish && idle.status == "In Ember menus" && idle.connect.empty());
	// Offline practice/training still reports menus, never a connect value.
	in.session.match = netplay::MatchState::None;
	CHECK(steam_presence::Describe(in).connect.empty());
}

static void TestHealthyJoinableRoomPublishesConnect() {
	const auto out = steam_presence::Describe(Base());
	CHECK(out.publish && out.status == "In an Ember room (1/16)" && out.connect == "7K3M0X1RT9PZ");
}

static void TestShortCodePendingOrFailedWithholdsConnect() {
	steam_presence::Input in = Base();
	in.code.clear(); // short link still pending, or the service failed
	CHECK(steam_presence::Describe(in).connect.empty());
}

static void TestRoomFullOrLockedWithholdsConnect() {
	steam_presence::Input in = Base();
	in.room.capacity = 1;
	auto out = steam_presence::Describe(in);
	CHECK(out.status == "In a private Ember room" && out.connect.empty());
	in = Base();
	in.room.locked = true;
	out = steam_presence::Describe(in);
	CHECK(out.status == "In a private Ember room" && out.connect.empty());
}

static void TestControlDegradedWithholdsConnect() {
	steam_presence::Input in = Base();
	in.session.control = netplay::Health::Lost;
	CHECK(steam_presence::Describe(in).connect.empty());
}

static void TestPreferenceDisabledWithholdsConnect() {
	steam_presence::Input in = Base();
	in.invites = false;
	CHECK(steam_presence::Describe(in).connect.empty());
}

static void TestQueuedPlayingAndSpectatingStatus() {
	steam_presence::Input in = Base();
	in.room.members[0].status = room::MemberStatus::Queued;
	CHECK(steam_presence::Describe(in).status == "Queued in Ember");
	in.room.members[0].status = room::MemberStatus::Playing;
	CHECK(steam_presence::Describe(in).status == "Fighting with Ember rollback");
	in.room.members[0].status = room::MemberStatus::Watching;
	CHECK(steam_presence::Describe(in).status == "Spectating an Ember match");
}

static void TestPlayingOrSpectatingOutsideARoom() {
	steam_presence::Input in;
	in.attached = true;
	in.session.match = netplay::MatchState::Playing;
	CHECK(steam_presence::Describe(in).status == "Fighting with Ember rollback");
	in.room.members.push_back(Local(room::MemberStatus::Watching));
	in.room.localMember = 2;
	CHECK(steam_presence::Describe(in).status == "Spectating an Ember match");
}

static void TestRoomClosingWithholdsConnect() {
	steam_presence::Input in = Base();
	in.room.closed = true;
	const auto out = steam_presence::Describe(in);
	CHECK(out.status == "In Ember menus" && out.connect.empty());
}

static void TestGenerationReplacedWithholdsAStaleCode() {
	steam_presence::Input in = Base();
	in.codeGeneration = 1; // resolved for an older room generation
	in.session.generation.room = 2; // the room has since been replaced
	CHECK(steam_presence::Describe(in).connect.empty());
}

static void TestNotJoinedNeverPublishesAConnectValue() {
	steam_presence::Input in = Base();
	in.session.room = netplay::RoomState::Opening;
	CHECK(steam_presence::Describe(in).connect.empty());
	in.session.room = netplay::RoomState::Idle;
	CHECK(steam_presence::Describe(in).connect.empty());
}

int main() {
	TestNoRuntimeOrDisabledNeverPublishes();
	TestMainMenuAndOfflinePlay();
	TestHealthyJoinableRoomPublishesConnect();
	TestShortCodePendingOrFailedWithholdsConnect();
	TestRoomFullOrLockedWithholdsConnect();
	TestControlDegradedWithholdsConnect();
	TestPreferenceDisabledWithholdsConnect();
	TestQueuedPlayingAndSpectatingStatus();
	TestPlayingOrSpectatingOutsideARoom();
	TestRoomClosingWithholdsConnect();
	TestGenerationReplacedWithholdsAStaleCode();
	TestNotJoinedNeverPublishesAConnectValue();
	std::printf("steam_presence_test: all tests passed\n");
	return 0;
}
