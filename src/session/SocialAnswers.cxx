#include "SocialAnswers.hxx"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "../common/SkillBadgeJson.hxx"
#include "../netplay/Levels.hxx"
#include "../netplay/Rankings.hxx"
#include "AnswerReading.hxx"

namespace sf4e { namespace session {
using namespace reading;

namespace {
using nlohmann::json;
namespace friends = netplay::friends;

// The bridge's id shape: a prefix, "_", and a 36-character uuid.
constexpr std::size_t IdBytes = 3 + 36;
constexpr std::size_t MaxLabelBytes = 64;

[[noreturn]] void Bad(const char* what) { throw std::invalid_argument(what); }


const json& Array(const json& object, const char* key, std::size_t maximum) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_array() || found->size() > maximum) Bad(key);
	return *found;
}





bool Flag(const json& object, const char* key) {
	const auto found = object.find(key);
	if (found == object.end()) return false;
	if (!found->is_boolean()) Bad(key);
	return found->get<bool>();
}



// "fr_", "rq_", "bl_" or "iv_" and the uuid.
std::string Id(const json& object, const char* key, const char* prefix) {
	auto id = NonEmptyText(object, key, IdBytes);
	if (id.size() != IdBytes || id.compare(0, 3, prefix) != 0) Bad(key);
	return id;
}

std::string Name(const json& object, const char* key) {
	return SingleLine(NonEmptyText(object, key, netplay::rankings::MaxNameBytes));
}

// A country the game's table may lack is shown without a flag; one that is not two upper-case letters is malformed.
CountryCode Country(const json& object) {
	const auto found = object.find("country");
	if (found == object.end()) return CountryCode();
	if (!found->is_string()) Bad("country");
	const auto text = found->get<std::string>();
	if (text.size() != 2 || text[0] < 'A' || text[0] > 'Z' || text[1] < 'A' || text[1] > 'Z') Bad("country");
	return CountryCode::Parsed(text);
}

// XXXX-XXXX: the shape of the code the bridge shows the player. Which symbols it draws them from is the bridge's
// alphabet, which it alone checks (a code typed here goes to it as typed, and is refused there if unknown).
bool IsFriendCode(const std::string& code) {
	if (code.size() != 9 || code[4] != '-') return false;
	for (std::size_t i = 0; i < code.size(); ++i)
		if (i != 4 && !((code[i] >= '0' && code[i] <= '9') || (code[i] >= 'A' && code[i] <= 'Z'))) return false;
	return true;
}

// A status a later bridge adds reads as the neutral one in a room, so the friend is still listed where they are.
friends::Status ReadStatus(const json& object) {
	const auto status = NonEmptyText(object, "status", 16);
	if (status == "queued") return friends::Status::Queued;
	if (status == "ready") return friends::Status::Ready;
	if (status == "fighting") return friends::Status::Fighting;
	if (status == "watching") return friends::Status::Watching;
	if (status == "training") return friends::Status::Training;
	return friends::Status::Idle;
}

int ReadTable(const json& object) {
	if (object.find("table") == object.end()) return -1;
	return static_cast<int>(Small(object, "table", friends::MaxTable));
}

friends::Room DecodeRoom(const json& value) {
	const auto& object = Object(value);
	friends::Room room;
	room.id = NonEmptyText(object, "room_id", 32);
	room.name = SingleLine(NonEmptyText(object, "name", MaxLabelBytes));
	room.members = static_cast<std::uint8_t>(Small(object, "members", 16));
	room.capacity = static_cast<std::uint8_t>(Small(object, "capacity", 16));
	room.joinable = Flag(object, "joinable");
	if (!IsHex(room.id, 32) || room.capacity < 2 || room.members > room.capacity) Bad("room");
	return room;
}

friends::Presence DecodePresence(const json& value) {
	const auto& object = Object(value);
	friends::Presence presence;
	const auto state = NonEmptyText(object, "state", 16);
	if (state == "offline") {
		presence.kind = friends::Presence::Kind::Offline;
		presence.seenAt = OptionalCount(object, "seen_at");
	} else if (state == "room") {
		presence.kind = friends::Presence::Kind::InRoom;
		presence.status = ReadStatus(object);
		presence.table = ReadTable(object);
		presence.privateRoom = Flag(object, "private");
		if (const auto room = object.find("room"); room != object.end()) {
			if (presence.privateRoom) Bad("room");
			presence.hasRoom = true;
			presence.room = DecodeRoom(*room);
		}
	} else {
		// Online, as a state a later bridge adds is: the friend is there, and a state this build cannot name says no more.
		// An activity it does not know is the menus.
		presence.kind = friends::Presence::Kind::Online;
		if (state == "online" && NonEmptyText(object, "activity", 16) == "offline_play") presence.activity = friends::Activity::OfflinePlay;
	}
	return presence;
}


// Points and tier are shown together or not at all. How many points a tier
// takes is the bridge's to say.
friends::Main DecodeMain(const json& value) {
	const auto& object = Object(value);
	friends::Main main;
	main.fighter = static_cast<std::uint8_t>(Small(object, "fighter", selection::FighterCount - 1));
	const bool points = object.find("points") != object.end(), tier = object.find("tier") != object.end();
	if (points != tier) Bad("tier");
	if (points) {
		main.showsPoints = true;
		main.points = Count(object, "points");
		main.tier = Tier(object);
	}
	return main;
}

// Each optional part of a friend stands alone: one this build cannot read is
// left out and the friend is still listed. What names the friend (id, name,
// country, since) and where they are (presence) must read, or the row goes.
friends::Friend DecodeFriend(const json& value) {
	const auto& object = Object(value);
	friends::Friend entry;
	entry.id = Id(object, "id", "fr_");
	entry.name = Name(object, "name");
	entry.country = Country(object);
	entry.since = Count(object, "since");
	const auto part = [](auto read) { try { read(); return true; } catch (const std::exception&) { return false; } };
	if (const auto main = object.find("main"); main != object.end())
		entry.hasMain = part([&] { entry.main = DecodeMain(*main); });
	if (const auto account = object.find("account"); account != object.end()) {
		entry.hasAccount = part([&] {
			const auto& fields = Object(*account);
			entry.account.points = Count(fields, "points");
			entry.account.tier = Tier(fields);
		});
		if (!entry.hasAccount) entry.account = {};
	}
	// A badge this build cannot read is no badge, not a list that cannot be read.
	entry.badge = ReadSkillBadge(object);
	const auto presence = object.find("presence");
	if (presence == object.end()) Bad("presence");
	entry.presence = DecodePresence(*presence);
	if (const auto record = object.find("record"); record != object.end()) {
		entry.hasRecord = part([&] {
			const auto& fields = Object(*record);
			entry.record.won = Count(fields, "won");
			entry.record.lost = Count(fields, "lost");
			entry.record.lastAt = Count(fields, "last_at");
		});
		// A record with no game in it says nothing.
		if (!entry.hasRecord || (entry.record.won == 0 && entry.record.lost == 0)) { entry.hasRecord = false; entry.record = {}; }
	}
	if (!entry.hasMain) entry.main = {};
	return entry;
}

friends::Incoming DecodeIncoming(const json& value) {
	const auto& object = Object(value);
	friends::Incoming entry;
	entry.id = Id(object, "id", "rq_");
	entry.name = Name(object, "name");
	entry.country = Country(object);
	entry.sentAt = Count(object, "sent_at");
	if (const auto played = object.find("played"); played != object.end()) {
		const auto& fields = Object(*played);
		entry.games = Count(fields, "games");
		entry.lastGameAt = Count(fields, "last_at");
	}
	return entry;
}

friends::Outgoing DecodeOutgoing(const json& value) {
	const auto& object = Object(value);
	friends::Outgoing entry;
	entry.id = Id(object, "id", "rq_");
	entry.label = SingleLine(NonEmptyText(object, "label", MaxLabelBytes));
	entry.sentAt = Count(object, "sent_at");
	return entry;
}

// The invite, or nothing for one whose room is of a kind a later bridge adds: that invite is dropped and the rest are kept.
std::optional<friends::Invite> DecodeInvite(const json& value) {
	const auto& object = Object(value);
	friends::Invite entry;
	entry.id = Id(object, "id", "iv_");
	entry.friendId = Id(object, "friend", "fr_");
	entry.expiresAt = Count(object, "expires_at");
	const auto& room = Object(object.at("room"));
	const bool id = room.find("room_id") != room.end(), secret = room.find("secret") != room.end();
	// Both is malformed; neither is a kind this build does not know.
	if (id && secret) Bad("room");
	if (!id && !secret) return std::nullopt;
	if (id) {
		entry.room.kind = friends::InviteRoom::Kind::RoomId;
		entry.room.value = NonEmptyText(room, "room_id", 32);
		if (!IsHex(entry.room.value, 32)) Bad("room_id");
	} else {
		entry.room.kind = friends::InviteRoom::Kind::Secret;
		entry.room.value = NonEmptyText(room, "secret", 127);
		if (entry.room.value.size() != 127 || (entry.room.value.compare(0, 5, "emd1:") != 0 && entry.room.value.compare(0, 5, "emd2:") != 0))
			Bad("secret");
	}
	return entry;
}

friends::Blocked DecodeBlocked(const json& value) {
	const auto& object = Object(value);
	friends::Blocked entry;
	entry.id = Id(object, "id", "bl_");
	entry.name = Name(object, "name");
	entry.at = Count(object, "at");
	return entry;
}

// `decode` returns the entry, or an optional that is empty for a row to leave out. A row it cannot read is left out
// too, and the rest of the list is kept: one damaged row never costs the player the others.
template <class Entry, class Decode>
std::vector<Entry> DecodeList(const json& object, const char* key, std::size_t maximum, Decode decode) {
	std::vector<Entry> entries;
	const auto& rows = Array(object, key, maximum);
	entries.reserve(rows.size());
	for (const auto& row : rows) {
		try {
			auto decoded = decode(row);
			if constexpr (std::is_same_v<decltype(decoded), std::optional<Entry>>) { if (decoded) entries.push_back(std::move(*decoded)); }
			else entries.push_back(std::move(decoded));
		} catch (const std::exception&) {}
	}
	return entries;
}
}

bool DecodeFriendsState(const json& data, friends::State& state) {
	try {
		const auto& object = Object(data);
		friends::State decoded;
		decoded.kind = friends::State::Kind::Listed;
		decoded.code = NonEmptyText(object, "code", 9);
		if (!IsFriendCode(decoded.code)) return false;
		decoded.appearOffline = Flag(object, "appear_offline");
		decoded.friends = DecodeList<friends::Friend>(object, "friends", friends::MaxFriends, DecodeFriend);
		decoded.incoming = DecodeList<friends::Incoming>(object, "incoming", friends::MaxIncoming, DecodeIncoming);
		decoded.outgoing = DecodeList<friends::Outgoing>(object, "outgoing", friends::MaxOutgoing, DecodeOutgoing);
		decoded.invites = DecodeList<friends::Invite>(object, "invites", friends::MaxInvites, DecodeInvite);
		decoded.blocked = DecodeList<friends::Blocked>(object, "blocked", friends::MaxBlocked, DecodeBlocked);
		state = std::move(decoded);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

} }
