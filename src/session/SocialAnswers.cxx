#include "SocialAnswers.hxx"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "../common/SkillBadgeJson.hxx"
#include "../netplay/Levels.hxx"
#include "../netplay/Rankings.hxx"

namespace sf4e { namespace session {
namespace {
using nlohmann::json;
namespace friends = netplay::friends;

// The bridge's id shape: a prefix, "_", and a 36-character uuid.
constexpr std::size_t IdBytes = 3 + 36;
constexpr std::size_t MaxLabelBytes = 64;

[[noreturn]] void Bad(const char* what) { throw std::invalid_argument(what); }

const json& Object(const json& value) {
	if (!value.is_object()) Bad("object");
	return value;
}

const json& Array(const json& object, const char* key, std::size_t maximum) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_array() || found->size() > maximum) Bad(key);
	return *found;
}

std::string Text(const json& object, const char* key, std::size_t maximum) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) Bad(key);
	auto text = found->get<std::string>();
	if (text.empty() || text.size() > maximum) Bad(key);
	return text;
}

// Another player's text is one line.
std::string SingleLine(std::string text) {
	for (auto& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
	return text;
}

std::uint64_t Unsigned(const json& object, const char* key) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_number_unsigned()) Bad(key);
	return found->get<std::uint64_t>();
}

// A time or count that may be absent: absent is 0.
std::uint64_t OptionalUnsigned(const json& object, const char* key) {
	return object.find(key) == object.end() ? 0 : Unsigned(object, key);
}

bool Flag(const json& object, const char* key) {
	const auto found = object.find(key);
	if (found == object.end()) return false;
	if (!found->is_boolean()) Bad(key);
	return found->get<bool>();
}

std::uint64_t Bounded(const json& object, const char* key, std::uint64_t maximum) {
	const auto value = Unsigned(object, key);
	if (value > maximum) Bad(key);
	return value;
}

bool IsHexId(const std::string& text) {
	return text.size() == 32 && std::all_of(text.begin(), text.end(),
		[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// "fr_", "rq_", "bl_" or "iv_" and the uuid.
std::string Id(const json& object, const char* key, const char* prefix) {
	auto id = Text(object, key, IdBytes);
	if (id.size() != IdBytes || id.compare(0, 3, prefix) != 0) Bad(key);
	return id;
}

std::string Name(const json& object, const char* key) {
	return SingleLine(Text(object, key, netplay::rankings::MaxNameBytes));
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

// XXXX-XXXX in Crockford base32.
bool IsFriendCode(const std::string& code) {
	static const std::string symbols = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
	if (code.size() != 9 || code[4] != '-') return false;
	for (std::size_t i = 0; i < code.size(); ++i)
		if (i != 4 && symbols.find(code[i]) == std::string::npos) return false;
	return true;
}

friends::Status ReadStatus(const json& object) {
	const auto status = Text(object, "status", 16);
	if (status == "idle") return friends::Status::Idle;
	if (status == "queued") return friends::Status::Queued;
	if (status == "ready") return friends::Status::Ready;
	if (status == "fighting") return friends::Status::Fighting;
	if (status == "watching") return friends::Status::Watching;
	if (status == "training") return friends::Status::Training;
	Bad("status");
}

int ReadTable(const json& object) {
	if (object.find("table") == object.end()) return -1;
	return static_cast<int>(Bounded(object, "table", friends::MaxTable));
}

friends::Room DecodeRoom(const json& value) {
	const auto& object = Object(value);
	friends::Room room;
	room.id = Text(object, "room_id", 32);
	room.name = SingleLine(Text(object, "name", MaxLabelBytes));
	room.members = static_cast<std::uint8_t>(Bounded(object, "members", 16));
	room.capacity = static_cast<std::uint8_t>(Bounded(object, "capacity", 16));
	room.joinable = Flag(object, "joinable");
	if (!IsHexId(room.id) || room.capacity < 2 || room.members > room.capacity) Bad("room");
	return room;
}

friends::Presence DecodePresence(const json& value) {
	const auto& object = Object(value);
	friends::Presence presence;
	const auto state = Text(object, "state", 16);
	if (state == "offline") {
		presence.kind = friends::Presence::Kind::Offline;
		presence.seenAt = OptionalUnsigned(object, "seen_at");
	} else if (state == "online") {
		presence.kind = friends::Presence::Kind::Online;
		const auto activity = Text(object, "activity", 16);
		if (activity == "menus") presence.activity = friends::Activity::Menus;
		else if (activity == "offline_play") presence.activity = friends::Activity::OfflinePlay;
		else Bad("activity");
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
		Bad("state");
	}
	return presence;
}

friends::Main DecodeMain(const json& value) {
	const auto& object = Object(value);
	friends::Main main;
	main.fighter = static_cast<std::uint8_t>(Bounded(object, "fighter", selection::FighterCount - 1));
	const bool points = object.find("points") != object.end(), tier = object.find("tier") != object.end();
	if (points != tier) Bad("tier");
	if (points) {
		main.showsPoints = true;
		main.points = Unsigned(object, "points");
		main.tier = static_cast<std::uint8_t>(Bounded(object, "tier", netplay::levels::TierCount - 1));
		if (main.points == 0) Bad("points");
	}
	return main;
}

friends::Friend DecodeFriend(const json& value) {
	const auto& object = Object(value);
	friends::Friend entry;
	entry.id = Id(object, "id", "fr_");
	entry.name = Name(object, "name");
	entry.country = Country(object);
	entry.since = Unsigned(object, "since");
	if (const auto main = object.find("main"); main != object.end()) {
		entry.hasMain = true;
		entry.main = DecodeMain(*main);
	}
	if (const auto account = object.find("account"); account != object.end()) {
		const auto& fields = Object(*account);
		entry.hasAccount = true;
		entry.account.points = Unsigned(fields, "points");
		entry.account.tier = static_cast<std::uint8_t>(Bounded(fields, "tier", netplay::levels::TierCount - 1));
	}
	if (const auto badge = object.find("badge"); badge != object.end() && !ParseSkillBadge(*badge, entry.badge)) Bad("badge");
	const auto presence = object.find("presence");
	if (presence == object.end()) Bad("presence");
	entry.presence = DecodePresence(*presence);
	if (const auto record = object.find("record"); record != object.end()) {
		const auto& fields = Object(*record);
		entry.hasRecord = true;
		entry.record.won = Unsigned(fields, "won");
		entry.record.lost = Unsigned(fields, "lost");
		entry.record.lastAt = Unsigned(fields, "last_at");
		if (entry.record.won == 0 && entry.record.lost == 0) Bad("record");
	}
	return entry;
}

friends::Incoming DecodeIncoming(const json& value) {
	const auto& object = Object(value);
	friends::Incoming entry;
	entry.id = Id(object, "id", "rq_");
	entry.name = Name(object, "name");
	entry.country = Country(object);
	entry.sentAt = Unsigned(object, "sent_at");
	if (const auto played = object.find("played"); played != object.end()) {
		const auto& fields = Object(*played);
		entry.games = Unsigned(fields, "games");
		entry.lastGameAt = Unsigned(fields, "last_at");
		if (entry.games == 0) Bad("played");
	}
	return entry;
}

friends::Outgoing DecodeOutgoing(const json& value) {
	const auto& object = Object(value);
	friends::Outgoing entry;
	entry.id = Id(object, "id", "rq_");
	entry.label = SingleLine(Text(object, "label", MaxLabelBytes));
	entry.sentAt = Unsigned(object, "sent_at");
	return entry;
}

friends::Invite DecodeInvite(const json& value) {
	const auto& object = Object(value);
	friends::Invite entry;
	entry.id = Id(object, "id", "iv_");
	entry.friendId = Id(object, "friend", "fr_");
	entry.expiresAt = Unsigned(object, "expires_at");
	const auto& room = Object(object.at("room"));
	// Exactly one of the two, as the bridge sends them.
	const bool id = room.find("room_id") != room.end(), secret = room.find("secret") != room.end();
	if (id == secret) Bad("room");
	if (id) {
		entry.room.kind = friends::InviteRoom::Kind::RoomId;
		entry.room.value = Text(room, "room_id", 32);
		if (!IsHexId(entry.room.value)) Bad("room_id");
	} else {
		entry.room.kind = friends::InviteRoom::Kind::Secret;
		entry.room.value = Text(room, "secret", 127);
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
	entry.at = Unsigned(object, "at");
	return entry;
}

template <class Entry, class Decode>
std::vector<Entry> DecodeList(const json& object, const char* key, std::size_t maximum, Decode decode) {
	std::vector<Entry> entries;
	const auto& rows = Array(object, key, maximum);
	entries.reserve(rows.size());
	for (const auto& row : rows) entries.push_back(decode(row));
	return entries;
}
}

bool IsSocialOp(const std::string& op) { return op == "friends_sync" || op == "friends_act"; }

bool ReadSocialAnswer(const json& event, TournamentAnswer& answer) {
	try {
		TournamentAnswer read;
		read.op = Text(event, "op", 64);
		if (!IsSocialOp(read.op)) return false;
		const auto& id = event.at("request_id");
		const auto& ok = event.at("ok");
		if (!id.is_number_unsigned() || !ok.is_boolean()) return false;
		read.requestId = id.get<std::uint64_t>();
		read.ok = ok.get<bool>();
		if (const auto reason = event.find("reason"); reason != event.end() && !reason->is_null()) read.reason = Text(event, "reason", 64);
		if (const auto data = event.find("data"); data != event.end()) read.data = *data;
		answer = std::move(read);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool DecodeFriendsState(const json& data, friends::State& state) {
	try {
		const auto& object = Object(data);
		friends::State decoded;
		decoded.kind = friends::State::Kind::Listed;
		decoded.code = Text(object, "code", 9);
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
