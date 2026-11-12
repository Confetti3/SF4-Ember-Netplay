// A room's Skip winner scenes rule: on by default in every room, public ones
// included. The host of a private room turns it off (SetSkipScenes) and on
// again; nobody else can, and a server-owned room keeps it on. It gates no seat
// and moves nobody, and it travels on the wire only when off, so a room
// without the key reads as on.
#include "../session/RoomModel.hxx"

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using sf4e::NetworkLink;
using namespace sf4e::room;

namespace {
struct Room {
	RoomAuthority authority;
	MemberId host = 0, guest = 0;
	std::uint64_t nextAction = 1;
	int nextConnection = 0;

	explicit Room(bool skipScenes = true)
		: authority("Scenes room", 16, 53, Rules(), false, DelayPolicy::Shared, ClassicRoom,
			static_cast<std::uint8_t>(ClassicTableCount), false, false, skipScenes) {
		authority.AdvanceTime(1000);
		host = Join("Host", true);
		guest = Join("Guest", false);
	}
	MemberId Join(const char* name, bool isHost) {
		MemberProfile profile;
		profile.link = NetworkLink::Wired;
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(nextConnection++)}, isHost, profile);
		EXPECT(result.accepted);
		return result.accepted ? result.snapshot.members.back().id : 0;
	}
	const Snapshot& View() const { return authority.SnapshotView(); }
	Action Make(ActionKind kind) {
		Action action;
		action.kind = kind;
		action.roomEpoch = View().roomEpoch;
		action.revision = View().revision;
		action.tableRevision = View().tables[0].revision;
		action.actionId = nextAction++;
		return action;
	}
	Result Set(MemberId member, bool on) {
		auto action = Make(ActionKind::SetSkipScenes);
		action.skipScenes = on;
		return authority.Apply(member, action);
	}
};
}

int main() {
	{
		// On by default; the host turns it off and on, a member cannot.
		Room room;
		EXPECT(room.View().skipScenes);
		const auto refused = room.Set(room.guest, false);
		EXPECT(!refused.accepted && refused.reason == RejectReason::NotHost && room.View().skipScenes);
		const auto before = room.View().revision;
		EXPECT(room.Set(room.host, false).accepted && !room.View().skipScenes);
		EXPECT(room.View().revision > before);
		// The same value again is accepted and changes nothing.
		const auto after = room.View().revision;
		EXPECT(room.Set(room.host, false).accepted && room.View().revision == after);
		EXPECT(room.Set(room.host, true).accepted && room.View().skipScenes);
	}
	{
		// Every member's view carries it, with or without the chat.
		Room on;
		EXPECT(on.authority.SnapshotFor(on.guest).skipScenes);
		EXPECT(on.authority.SnapshotFor(on.guest, false).skipScenes);
		Room off(false);
		EXPECT(!off.View().skipScenes);
		EXPECT(!off.authority.SnapshotFor(off.guest).skipScenes);
		EXPECT(!off.authority.SnapshotFor(off.guest, false).skipScenes);
	}
	{
		// A stale revision is refused, as with the other room settings.
		Room room;
		auto action = room.Make(ActionKind::SetSkipScenes);
		action.skipScenes = false;
		action.revision = room.View().revision + 7;
		const auto result = room.authority.Apply(room.host, action);
		EXPECT(!result.accepted && result.reason == RejectReason::StaleRoom && room.View().skipScenes);
	}
	{
		// A public (server-owned) room keeps the default: its rule is not set by any member.
		RoomAuthority authority("Open room", 8, 9, PublicRoomRules());
		EXPECT(authority.SetServerOwned());
		EXPECT(authority.SnapshotView().skipScenes);
		MemberProfile profile;
		profile.link = NetworkLink::Wired;
		// A public room admits an account; its first member is not the host (SetServerOwned).
		profile.admission.account = "emb1-member";
		const auto joined = authority.Join("Member", ConnectionRef{"host", "1"}, false, profile);
		EXPECT(joined.accepted && !joined.snapshot.members.empty());
		if (!joined.accepted || joined.snapshot.members.empty()) return TEST_FAILURES() ? 1 : 0;
		Action action;
		action.kind = ActionKind::SetSkipScenes;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.actionId = 1;
		action.skipScenes = false;
		const auto result = authority.Apply(joined.snapshot.members.back().id, action);
		EXPECT(!result.accepted && authority.SnapshotView().skipScenes);
		const nlohmann::json json = authority.SnapshotView();
		EXPECT(!json.contains("skip_scenes") && json.get<Snapshot>().skipScenes);
	}
	{
		// It gates no seat: a member sits whether it is on or off.
		Room room(false);
		EXPECT(room.authority.Apply(room.guest, room.Make(ActionKind::Queue)).accepted);
	}
	{
		// The key is written only when off, and a missing key reads as on.
		Room room;
		const nlohmann::json on = room.View();
		EXPECT(!on.contains("skip_scenes") && on.get<Snapshot>().skipScenes);
		EXPECT(room.Set(room.host, false).accepted);
		const nlohmann::json off = room.View();
		EXPECT(off.at("skip_scenes") == false && !off.get<Snapshot>().skipScenes);
		auto bad = off;
		bad["skip_scenes"] = "no";
		bool threw = false;
		try { (void)bad.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// A checkpoint keeps the rule.
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && !restored.SnapshotView().skipScenes);
	}
	{
		// The action: the key only when off, an unknown kind past the last is refused.
		Room room;
		auto action = room.Make(ActionKind::SetSkipScenes);
		action.skipScenes = false;
		const nlohmann::json sent = action;
		EXPECT(sent.at("skip_scenes") == false && !sent.get<Action>().skipScenes && sent.get<Action>().kind == ActionKind::SetSkipScenes);
		action.skipScenes = true;
		const nlohmann::json on = action;
		EXPECT(!on.contains("skip_scenes") && on.get<Action>().skipScenes);
		// Any other action carries no key and reads as the default.
		const nlohmann::json other = room.Make(ActionKind::Lock);
		EXPECT(!other.contains("skip_scenes"));
		auto unknown = sent;
		unknown["kind"] = static_cast<int>(LastActionKind) + 1;
		bool threw = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		auto bad = sent;
		bad["skip_scenes"] = 0;
		threw = false;
		try { (void)bad.get<Action>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// SetSkipScenes is the last kind: SetTinyFighters stays where it was.
		EXPECT(static_cast<int>(ActionKind::SetSkipScenes) == static_cast<int>(ActionKind::SetTinyFighters) + 1);
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room skip scenes rules passed.\n");
	return 0;
}
