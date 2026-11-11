// A room's Tiny fighters allowed rule: its host sets it (SetTinyFighters) and
// turns it off again; nobody else can. It gates no seat and moves nobody, and
// it travels on the wire only when on, so a room without it reads as before.
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

	explicit Room(bool tinyFighters = false)
		: authority("Tiny room", 16, 52, Rules(), false, DelayPolicy::Shared, ClassicRoom,
			static_cast<std::uint8_t>(ClassicTableCount), false, tinyFighters) {
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
	Result SetTiny(MemberId member, bool on) {
		auto action = Make(ActionKind::SetTinyFighters);
		action.tinyFighters = on;
		return authority.Apply(member, action);
	}
};
}

int main() {
	{
		// Off by default; the host turns it on and off, a member cannot.
		Room room;
		EXPECT(!room.View().tinyFighters);
		const auto refused = room.SetTiny(room.guest, true);
		EXPECT(!refused.accepted && refused.reason == RejectReason::NotHost && !room.View().tinyFighters);
		const auto before = room.View().revision;
		EXPECT(room.SetTiny(room.host, true).accepted && room.View().tinyFighters);
		EXPECT(room.View().revision > before);
		// The same value again is accepted and changes nothing.
		const auto after = room.View().revision;
		EXPECT(room.SetTiny(room.host, true).accepted && room.View().revision == after);
		EXPECT(room.SetTiny(room.host, false).accepted && !room.View().tinyFighters);
	}
	{
		// Chosen when the room is created.
		Room room(true);
		EXPECT(room.View().tinyFighters);
		EXPECT(room.authority.SnapshotFor(room.guest).tinyFighters);
		EXPECT(room.authority.SnapshotFor(room.guest, false).tinyFighters);
	}
	{
		// A stale revision is refused, as with the other room settings.
		Room room;
		auto action = room.Make(ActionKind::SetTinyFighters);
		action.tinyFighters = true;
		action.revision = room.View().revision + 7;
		const auto result = room.authority.Apply(room.host, action);
		EXPECT(!result.accepted && result.reason == RejectReason::StaleRoom && !room.View().tinyFighters);
	}
	{
		// It gates no seat: a member sits whether it is on or off.
		Room room(true);
		EXPECT(room.authority.Apply(room.guest, room.Make(ActionKind::Queue)).accepted);
	}
	{
		// The key is written only when on, and a missing key reads as off.
		Room room;
		const nlohmann::json off = room.View();
		EXPECT(!off.contains("tiny_fighters") && !off.get<Snapshot>().tinyFighters);
		EXPECT(room.SetTiny(room.host, true).accepted);
		const nlohmann::json on = room.View();
		EXPECT(on.at("tiny_fighters") == true && on.get<Snapshot>().tinyFighters);
		auto bad = on;
		bad["tiny_fighters"] = "yes";
		bool threw = false;
		try { (void)bad.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// A checkpoint keeps the rule.
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && restored.SnapshotView().tinyFighters);
	}
	{
		// The action: the key only when on, an unknown kind past the last is refused.
		Room room;
		auto action = room.Make(ActionKind::SetTinyFighters);
		action.tinyFighters = true;
		const nlohmann::json sent = action;
		EXPECT(sent.at("tiny_fighters") == true && sent.get<Action>().tinyFighters && sent.get<Action>().kind == ActionKind::SetTinyFighters);
		action.tinyFighters = false;
		const nlohmann::json off = action;
		EXPECT(!off.contains("tiny_fighters") && !off.get<Action>().tinyFighters);
		auto unknown = sent;
		unknown["kind"] = static_cast<int>(LastActionKind) + 1;
		bool threw = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// SetTinyFighters is the last kind: SetWiredOnly stays where it was.
		EXPECT(static_cast<int>(ActionKind::SetTinyFighters) == static_cast<int>(ActionKind::SetWiredOnly) + 1);
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room tiny fighters rules passed.\n");
	return 0;
}
