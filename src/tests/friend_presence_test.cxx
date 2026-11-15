// What friends are told of where the player is, and which room the player can
// offer them (netplay/FriendPresence.hxx), for every state discord::Describe
// tells apart.
#include "../netplay/FriendPresence.hxx"

#include <cstdio>
#include <string>

#include "test_support.hxx"

using namespace sf4e;
namespace friends = netplay::friends;
using friends::Report;
using friends::Status;
using nlohmann::json;

namespace {
const std::string RoomId = "0f0123456789abcdef0123456789abcd";
const std::string Invitation = "emd1:" + std::string(122, 'A');

struct World {
	netplay::Snapshot session;
	room::Snapshot room;
	discord::PresenceInput input{session, room};
	friends::RoomIdentity identity;

	World() {
		input.ready = true;
		input.show = true;
		input.invites = true;
		input.now = 100;
		input.expires = 200;
		input.party = "ember:test";
		input.secret = Invitation;
	}
	void Join(room::MemberStatus status) {
		session.room = netplay::RoomState::Joined;
		session.control = netplay::Health::Healthy;
		room::Member local;
		local.id = 2;
		local.status = status;
		room.members = {local};
		room.localMember = 2;
		room.capacity = 16;
		room.tableCount = 8;
	}
	discord::Presence Described() const { return discord::Describe(input); }
	Report Reported() const { return friends::ReportOf(Described(), room, identity); }
	friends::Offer Offered() const { return friends::OfferOf(Described(), identity); }
};

void OutsideARoom() {
	World world;
	world.input.ready = false;
	EXPECT(world.Reported().kind == Report::Kind::Menus && world.Described().where == discord::Presence::Where::Starting);
	world.input.ready = true;
	EXPECT(world.Reported() == Report() && world.Reported().ToJson() == json({{"activity", "menus"}}));
	world.input.offline = true;
	EXPECT(world.Reported().kind == Report::Kind::OfflinePlay && world.Reported().ToJson() == json({{"activity", "offline_play"}}));
	EXPECT(!world.Offered().Any());
}

// Every status of a member of a public room, and where it sits.
void InAPublicRoom() {
	World world;
	world.Join(room::MemberStatus::Idle);
	world.identity.isPublic = true;
	world.identity.publicId = RoomId;
	auto report = world.Reported();
	EXPECT(report.kind == Report::Kind::InRoom && report.roomId == RoomId && report.status == Status::Idle && report.table == -1);
	EXPECT(report.ToJson() == json({{"activity", "room"}, {"room", {{"room_id", RoomId}, {"status", "idle"}}}}));

	world.room.members[0].status = room::MemberStatus::Queued;
	world.room.tables[2].queue = {2};
	report = world.Reported();
	EXPECT(report.status == Status::Queued && report.table == -1);

	world.room.tables[2].queue.clear();
	world.room.members[0].seat = 0;
	world.room.members[0].table = 1;
	world.room.members[0].status = room::MemberStatus::Seated;
	EXPECT(world.Reported().status == Status::Idle && world.Reported().table == 1);
	world.room.members[0].status = room::MemberStatus::Ready;
	EXPECT(world.Reported().status == Status::Ready && world.Reported().table == 1);
	world.room.members[0].status = room::MemberStatus::Playing;
	report = world.Reported();
	EXPECT(report.status == Status::Fighting && report.table == 1);
	EXPECT(report.ToJson() == json({{"activity", "room"}, {"room", {{"room_id", RoomId}, {"status", "fighting"}, {"table", 1}}}}));

	world.room.members[0].seat = -1;
	world.room.members[0].table = -1;
	world.room.members[0].status = room::MemberStatus::Watching;
	world.room.tables[3].spectators = {2};
	report = world.Reported();
	EXPECT(report.status == Status::Watching && report.table == 3);

	// Training in the room replaces the status, and names no table.
	world.room.members[0].training = true;
	report = world.Reported();
	EXPECT(report.status == Status::Training && report.table == -1);
	EXPECT(report.ToJson() == json({{"activity", "room"}, {"room", {{"room_id", RoomId}, {"status", "training"}}}}));

	// A room while offline play is flagged is still a room.
	world.input.offline = true;
	EXPECT(world.Reported().kind == Report::Kind::InRoom);
}

// A private room its host hid from friends is only "in a private room", whatever the player does there.
void InAPrivateRoom() {
	World world;
	world.Join(room::MemberStatus::Playing);
	world.room.friendsHidden = true;
	world.room.members[0].seat = 1;
	world.room.members[0].table = 0;
	const auto report = world.Reported();
	EXPECT(report.kind == Report::Kind::InRoom && report.roomId.empty() && report.status == Status::Fighting && report.table == 0);
	const auto text = report.ToJson().dump();
	EXPECT(report.ToJson() == json({{"activity", "room"}, {"room", {{"private", true}, {"status", "fighting"}, {"table", 0}}}}));
	// Nothing of the room is offered, so the join secret is not in presence.
	EXPECT(!report.offered && report.secret.empty());
	EXPECT(text.find("emd1") == std::string::npos && text.find("secret") == std::string::npos && text.find("offer") == std::string::npos);
	// A public room the game has no id for is reported as one that is not named.
	world.identity.isPublic = true;
	EXPECT(world.Reported().roomId.empty());
	world.identity.publicId = "not an id";
	EXPECT(world.Reported().roomId.empty());
}

// A private room open to friends reports its name, size and invitation, under the rule discord::Describe offers a room
// to Discord friends by, and never for a public room or a room its host hid.
void AnOfferedPrivateRoom() {
	World world;
	world.Join(room::MemberStatus::Idle);
	world.room.name = "Open Mic";
	auto report = world.Reported();
	EXPECT(report.kind == Report::Kind::InRoom && report.roomId.empty() && report.status == Status::Idle);
	EXPECT(report.offered && report.name == "Open Mic" && report.members == 1 && report.capacity == 16 && report.secret == Invitation);
	EXPECT(report.ToJson() == json({{"activity", "room"}, {"room", {{"private", true}, {"status", "idle"},
		{"offer", {{"secret", Invitation}, {"name", "Open Mic"}, {"members", 1}, {"capacity", 16}}}}}}));
	// The table of a seated fighter goes along as before.
	world.room.members[0].status = room::MemberStatus::Playing;
	world.room.members[0].seat = 0;
	world.room.members[0].table = 2;
	report = world.Reported();
	EXPECT(report.offered && report.table == 2 && report.ToJson().at("room").at("offer").at("name") == "Open Mic");
	world.room.members[0].status = room::MemberStatus::Idle;
	world.room.members[0].seat = -1;
	world.room.members[0].table = -1;

	// Hidden by the host: nothing is offered, and the report is the plain one.
	world.room.friendsHidden = true;
	EXPECT(!world.Reported().offered && world.Reported() == world.Reported().WithoutOffer());
	world.room.friendsHidden = false;
	// A room that is locked, full, no longer healthy, bound to a match or with no invitation offers nothing.
	world.room.locked = true;
	EXPECT(!world.Reported().offered && world.Reported().secret.empty() && world.Reported().kind == Report::Kind::InRoom);
	world.room.locked = false;
	world.room.capacity = 1;
	EXPECT(!world.Reported().offered);
	world.room.capacity = 16;
	world.session.control = netplay::Health::Lost;
	EXPECT(!world.Reported().offered);
	world.session.control = netplay::Health::Healthy;
	world.room.tournament.matchId = "emt_1";
	EXPECT(!world.Reported().offered);
	world.room.tournament = room::TournamentBinding();
	world.input.secret.clear();
	EXPECT(!world.Reported().offered);
	world.input.secret = Invitation;
	EXPECT(world.Reported().offered);
	// A public room is never offered this way, hidden or not.
	world.identity.isPublic = true;
	world.identity.publicId = RoomId;
	report = world.Reported();
	EXPECT(!report.offered && report.roomId == RoomId && report.ToJson().dump().find("offer") == std::string::npos);
}

// What a sync compares: a renewed invitation, a new member, a rename, a leader change and the host's setting all differ.
void TheOfferIsCompared() {
	World world;
	world.Join(room::MemberStatus::Idle);
	world.room.name = "Open Mic";
	const auto base = world.Reported();
	EXPECT(base.offered && base == world.Reported() && !(base != world.Reported()));
	world.input.secret = "emd1:" + std::string(122, 'B');
	EXPECT(world.Reported() != base);
	world.input.secret = Invitation;
	room::Member other;
	other.id = 3;
	world.room.members.push_back(other);
	EXPECT(world.Reported().members == 2 && world.Reported() != base);
	world.room.members.pop_back();
	world.room.name = "Another room";
	EXPECT(world.Reported() != base);
	world.room.name = "Open Mic";
	world.room.capacity = 8;
	EXPECT(world.Reported().capacity == 8 && world.Reported() != base);
	world.room.capacity = 16;
	EXPECT(world.Reported() == base);
	world.room.friendsHidden = true;
	EXPECT(world.Reported() != base);
	// Without its offer a report is the same as the one of a hidden room.
	EXPECT(base.WithoutOffer() == world.Reported() && !base.WithoutOffer().offered && base.WithoutOffer().secret.empty() && base.WithoutOffer().name.empty());
	EXPECT(base.WithoutOffer().ToJson() == json({{"activity", "room"}, {"room", {{"private", true}, {"status", "idle"}}}}));
}

// The room that can be offered, by the rule discord::Describe offers a room to Discord friends by.
void WhatCanBeOffered() {
	World world;
	EXPECT(!world.Offered().Any());
	world.Join(room::MemberStatus::Idle);
	auto offer = world.Offered();
	EXPECT(offer.kind == friends::Offer::Kind::PrivateRoom && offer.value == Invitation);
	// A room with no invitation (a match's room, which the runtime passes none for) offers none.
	world.input.secret.clear();
	EXPECT(!world.Offered().Any());
	world.input.secret = Invitation;
	world.room.locked = true;
	EXPECT(!world.Offered().Any());
	world.room.locked = false;
	world.room.capacity = 1;
	EXPECT(!world.Offered().Any());
	world.room.capacity = 16;
	world.session.control = netplay::Health::Lost;
	EXPECT(!world.Offered().Any());
	world.session.control = netplay::Health::Healthy;
	world.room.tournament.matchId = "emt_1";
	EXPECT(world.room.tournament.Active() && !world.Offered().Any());
	world.room.tournament = room::TournamentBinding();
	EXPECT(world.Offered().Any());

	// A public room is offered by its id; its invitation would admit nobody, so none is sent.
	world.identity.isPublic = true;
	world.identity.publicId = RoomId;
	offer = world.Offered();
	EXPECT(offer.kind == friends::Offer::Kind::PublicRoom && offer.value == RoomId);
	world.room.locked = true;
	EXPECT(!world.Offered().Any());
	world.room.locked = false;
	world.identity.publicId = RoomId.substr(1);
	EXPECT(!world.Offered().Any());
}
}

int main() {
	OutsideARoom();
	InAPublicRoom();
	InAPrivateRoom();
	AnOfferedPrivateRoom();
	TheOfferIsCompared();
	WhatCanBeOffered();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
