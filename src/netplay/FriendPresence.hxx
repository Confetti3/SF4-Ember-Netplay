#pragma once
// What the player's friends are told of where this player is, and the room
// this player can offer them (docs/design/FRIENDS.md, "Presence"). Both are
// worked out from the description the Discord companion is sent
// (discord::Describe): "In menus" is `menus`, "Playing offline" `offline_play`,
// every room state `room`, and a member in Training `training`. A public room
// is named by the id the player joined it through the bridge; a private room
// is "in a private room" to every friend, and offered to them (the room's name,
// its size and its invitation) only while the room is private, open to
// friends (room::Snapshot::friendsHidden is off), and could be offered to
// Discord friends too. The invitation reaches the bridge in the sync but never
// leaves it unless a friend presses Join there, or the player sends it in an
// invite they choose. Pure: tested without a helper (friend_presence_test.cxx).
#include "../common/Utf8Text.hxx"
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
	// InRoom, a private room that friends may join: what the bridge shows of
	// it and keeps the invitation of. Never set for a public room. The
	// invitation is a key to the room: it is sent and compared, never logged.
	bool offered = false;
	std::string name;
	int members = 0, capacity = 0;
	std::string secret;

	bool operator==(const Report& other) const {
		return kind == other.kind && roomId == other.roomId && status == other.status && table == other.table &&
			offered == other.offered && name == other.name && members == other.members && capacity == other.capacity && secret == other.secret;
	}
	bool operator!=(const Report& other) const { return !(*this == other); }

	// {"activity":"menus"}, or {"activity":"room","room":{"room_id":...,"status":...,"table":n}}; a private room that
	// is offered also has "offer":{"secret":...,"name":...,"members":n,"capacity":n}.
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
		if (offered && roomId.empty()) {
			json offer = json::object();
			offer["secret"] = secret;
			offer["name"] = name;
			offer["members"] = members;
			offer["capacity"] = capacity;
			room["offer"] = std::move(offer);
		}
		presence["activity"] = "room";
		presence["room"] = std::move(room);
		return presence;
	}

	// The same presence without what a private room offers: the player appears offline, so nothing of the room is handed on.
	Report WithoutOffer() const {
		Report plain = *this;
		plain.offered = false;
		plain.name.clear();
		plain.members = plain.capacity = 0;
		plain.secret.clear();
		return plain;
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

// A room's name as an offer may carry it, or empty when it may carry none. The
// bridge takes a room name of 1 to 64 bytes of well-formed text with no
// control character and no white space at either end (the protocol's
// check_room_name), and refuses the whole sync for any other. The room itself
// allows more: "Player 1's room " with its trailing space is a name there. So
// the white space at the ends is left off for the offer, and a name that still
// is not one offers no room: the player then shows as in a private room, and
// their friends list keeps refreshing.
inline std::string OfferName(const std::string& roomName) {
	// What the protocol trims by: Unicode's White_Space outside the controls.
	const auto space = [](std::uint32_t c) {
		return c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
			c == 0x205F || c == 0x3000;
	};
	std::size_t begin = std::string::npos, end = 0;
	for (std::size_t at = 0; at < roomName.size();) {
		std::uint32_t code = 0;
		const std::size_t length = Utf8CharacterBytes(roomName, at, &code);
		if (!length || code < 0x20 || (code >= 0x7F && code <= 0x9F)) return {};
		if (!space(code)) {
			if (begin == std::string::npos) begin = at;
			end = at + length;
		}
		at += length;
	}
	if (begin == std::string::npos || end - begin > 64) return {};
	return roomName.substr(begin, end - begin);
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
	// A private room friends may join: offered under the rule Discord's Join follows (described.Offerable()), once it
	// has an invitation, unless its host hid it from friends, and while its name is one the bridge takes (OfferName).
	const std::string offeredName = identity.isPublic ? std::string() : OfferName(room.name);
	if (!identity.isPublic && described.Offerable() && !described.secret.empty() && room.OpenToFriends() && !offeredName.empty()) {
		report.offered = true;
		report.name = offeredName;
		report.members = described.size;
		report.capacity = described.capacity;
		report.secret = described.secret;
	}
	return report;
}

// What a description says keeps its room from being offered, as the one reason carried on: the causes the player can act on
// are named, a bound match or an unhealthy room is Other.
inline OfferBlock BlockOf(discord::Presence::Held held) {
	switch (held) {
	case discord::Presence::Held::None: return OfferBlock::None;
	case discord::Presence::Held::Locked: return OfferBlock::Locked;
	case discord::Presence::Held::Full: return OfferBlock::Full;
	default: return OfferBlock::Other;
	}
}

// The room the player can offer a friend now, if any: a public room by its id,
// a private room by its own invitation, under the rule discord::Describe
// offers a room to Discord friends by (not a bound match, healthy, not locked,
// not full). `described.secret` is the invitation when that rule holds and the
// runtime found one (it passes none for a match's room). `why` is None exactly
// when there is a room to offer, else what keeps it from being (the default: no room).
struct Offer {
	enum class Kind : std::uint8_t { None, PublicRoom, PrivateRoom } kind = Kind::None;
	// The room id, or the invitation.
	std::string value;
	OfferBlock why = OfferBlock::Other;
	bool Any() const { return why == OfferBlock::None; }
};

inline Offer OfferOf(const discord::Presence& described, const RoomIdentity& identity) {
	Offer offer;
	if (described.where != discord::Presence::Where::Room) return offer;
	offer.why = BlockOf(described.held);
	if (offer.why != OfferBlock::None) return offer;
	offer.why = OfferBlock::Other;
	if (identity.isPublic) {
		if (IsRoomId(identity.publicId)) { offer.kind = Offer::Kind::PublicRoom; offer.value = identity.publicId; }
	} else if (!described.secret.empty()) {
		offer.kind = Offer::Kind::PrivateRoom;
		offer.value = described.secret;
	}
	if (offer.kind != Offer::Kind::None) offer.why = OfferBlock::None;
	return offer;
}

} } }
