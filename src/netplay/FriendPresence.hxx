#pragma once
// What the player's friends are told of where this player is, and the room
// this player can offer them (docs/design/FRIENDS.md, "Presence"). Both are
// worked out from the description the Discord companion is sent
// (discord::Describe): "In menus" is `menus`, "Playing offline" `offline_play`,
// every room state `room`, and a member in Training `training`. A public room
// is named by the id the player joined it through the bridge; a private room
// is only "in a private room", and its join secret goes out only in an invite
// the player chooses to send. Pure: tested without a helper
// (friend_presence_test.cxx).
#include "../discord/Presence.hxx"
#include "../session/RoomFollow.hxx"
#include "Friends.hxx"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay { namespace friends {

// Which room the player is in, as the runtime knows it.
struct RoomIdentity {
	// Opened or joined through the bridge's public room list.
	bool isPublic = false;
	// A public room's id, 32 lower-case hex digits.
	std::string publicId;
};

inline bool IsRoomId(const std::string& id) {
	return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The presence a sync carries.
struct Report {
	enum class Kind : std::uint8_t { Menus, OfflinePlay, InRoom, Offline } kind = Kind::Menus;
	// InRoom: a public room's id, or none for a private room.
	std::string roomId;
	Status status = Status::Idle;
	// -1: none.
	int table = -1;

	bool operator==(const Report& other) const {
		return kind == other.kind && roomId == other.roomId && status == other.status && table == other.table;
	}
	bool operator!=(const Report& other) const { return !(*this == other); }

	// {"activity":"menus"}, or {"activity":"room","room":{"room_id":...,"status":...,"table":n}}.
	nlohmann::json ToJson() const {
		using nlohmann::json;
		json presence = json::object();
		switch (kind) {
		case Kind::Menus: presence["activity"] = "menus"; return presence;
		case Kind::OfflinePlay: presence["activity"] = "offline_play"; return presence;
		case Kind::Offline: presence["activity"] = "offline"; return presence;
		case Kind::InRoom: break;
		}
		json room = json::object();
		if (roomId.empty()) room["private"] = true;
		else room["room_id"] = roomId;
		room["status"] = StatusName(status);
		if (table >= 0) room["table"] = table;
		presence["activity"] = "room";
		presence["room"] = std::move(room);
		return presence;
	}

	static const char* StatusName(Status status) {
		switch (status) {
		case Status::Queued: return "queued";
		case Status::Ready: return "ready";
		case Status::Fighting: return "fighting";
		case Status::Watching: return "watching";
		case Status::Training: return "training";
		default: return "idle";
		}
	}
};

// The table the local member sits at or watches, -1 when neither or in Training.
inline int TableOf(const room::Snapshot& room) {
	const auto place = room::PlaceOf(room, room.localMember);
	if (place.kind == room::Place::Kind::Seat) return place.table;
	return room::WatchedTable(room, room.localMember);
}

// The presence for what the player is doing: `described` is discord::Describe's
// description of the same moment, `room` the room snapshot it was made from.
inline Report ReportOf(const discord::Presence& described, const room::Snapshot& room, const RoomIdentity& identity) {
	Report report;
	if (described.where == discord::Presence::Where::Offline) { report.kind = Report::Kind::OfflinePlay; return report; }
	// A game still starting is in no menu yet, but is online.
	if (described.where != discord::Presence::Where::Room) return report;
	report.kind = Report::Kind::InRoom;
	if (identity.isPublic && IsRoomId(identity.publicId)) report.roomId = identity.publicId;
	switch (described.status) {
	case room::MemberStatus::Queued: report.status = Status::Queued; break;
	case room::MemberStatus::Ready: report.status = Status::Ready; break;
	case room::MemberStatus::Playing: report.status = Status::Fighting; break;
	case room::MemberStatus::Watching: report.status = Status::Watching; break;
	default: report.status = Status::Idle; break;
	}
	if (described.training) report.status = Status::Training;
	else report.table = TableOf(room);
	return report;
}

// The room the player can offer a friend now, if any: a public room by its id,
// a private room by its own invitation, under the rule discord::Describe
// offers a room to Discord friends by (not a bound match, healthy, not locked,
// not full). `described.secret` is the invitation when that rule holds and the
// runtime found one (it passes none for a match's room).
struct Offer {
	enum class Kind : std::uint8_t { None, PublicRoom, PrivateRoom } kind = Kind::None;
	// The room id, or the invitation.
	std::string value;
	bool Any() const { return kind != Kind::None; }
};

inline Offer OfferOf(const discord::Presence& described, const RoomIdentity& identity) {
	Offer offer;
	if (described.where != discord::Presence::Where::Room || !described.offerable) return offer;
	if (identity.isPublic) {
		if (IsRoomId(identity.publicId)) { offer.kind = Offer::Kind::PublicRoom; offer.value = identity.publicId; }
	} else if (!described.secret.empty()) {
		offer.kind = Offer::Kind::PrivateRoom;
		offer.value = described.secret;
	}
	return offer;
}

} } }
