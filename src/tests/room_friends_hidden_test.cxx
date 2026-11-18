// A private room's Friends or Hidden setting: chosen when the room is created,
// changed by its host (SetFriendsHidden), refused in a room its server owns and
// in a match room. It changes what the friends of its members are shown and
// nothing about play, and it travels on the wire only when set.
#include "../session/RoomModel.hxx"

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e::room;

namespace {
Rules FirstToOne() {
	Rules rules;
	rules.format = SetFormat::Ft1;
	rules.rotation = RotationMode::WinnerStays;
	return rules;
}

struct Room {
	RoomAuthority authority;
	MemberId host = 0;
	std::uint64_t nextAction = 1;
	int nextConnection = 0;

	explicit Room(bool friendsHidden = false)
		: authority("Hidden room", 16, 52, FirstToOne(),
			Charter{RoomType::Classic, false, DelayPolicy::Shared, static_cast<std::uint8_t>(ClassicTableCount), false, friendsHidden}) {
		authority.AdvanceTime(1000);
		host = Join("Host", true);
	}
	MemberId Join(const char* name, bool isHost = false) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(nextConnection++)}, isHost, MemberProfile{});
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
	Result SetFriendsHidden(MemberId member, bool on) {
		auto action = Make(ActionKind::SetFriendsHidden);
		action.friendsHidden = on;
		return authority.Apply(member, action);
	}
};

bool Throws(const nlohmann::json& json) {
	try { (void)json.get<Snapshot>(); } catch (const std::exception&) { return true; }
	return false;
}
}

int main() {
	{
		// Visible to friends by default; created hidden, it starts hidden.
		Room room;
		EXPECT(!room.View().friendsHidden && !Charter().friendsHidden);
		Room hidden(true);
		EXPECT(hidden.View().friendsHidden);
	}
	{
		// The host hides the room and shows it again; the same value changes
		// nothing, and the room's play is untouched.
		Room room;
		const auto before = room.View().revision;
		EXPECT(room.SetFriendsHidden(room.host, true).accepted && room.View().friendsHidden && room.View().revision > before);
		const auto on = room.View().revision;
		EXPECT(room.SetFriendsHidden(room.host, true).accepted && room.View().revision == on);
		EXPECT(room.SetFriendsHidden(room.host, false).accepted && !room.View().friendsHidden);
		EXPECT(!room.View().wiredOnly && !room.View().locked);
	}
	{
		// Nobody but the host can, and a stale copy is refused.
		Room room;
		const auto other = room.Join("Sam");
		const auto revision = room.View().revision;
		EXPECT(room.SetFriendsHidden(other, true).reason == RejectReason::NotHost && !room.View().friendsHidden);
		EXPECT(room.View().revision == revision);
		auto stale = room.Make(ActionKind::SetFriendsHidden);
		stale.friendsHidden = true; stale.revision -= 1;
		EXPECT(room.authority.Apply(room.host, stale).reason == RejectReason::StaleRoom && !room.View().friendsHidden);
	}
	{
		// A room its server owns has no friends to hide it from: refused, and
		// the room is unchanged.
		RoomAuthority authority("Owned", 8, 53, FirstToOne());
		EXPECT(authority.SetServerOwned());
		MemberProfile profile;
		profile.admission.account = "emb1-host";
		const auto joined = authority.Join("Host", ConnectionRef{"host", "0"}, true, profile);
		EXPECT(joined.accepted);
		const auto host = joined.snapshot.members.back().id;
		Action action;
		action.kind = ActionKind::SetFriendsHidden; action.friendsHidden = true; action.actionId = 1;
		action.roomEpoch = authority.SnapshotView().roomEpoch; action.revision = authority.SnapshotView().revision;
		const auto revision = authority.SnapshotView().revision;
		EXPECT(authority.Apply(host, action).reason == RejectReason::Unauthorized);
		EXPECT(!authority.SnapshotView().friendsHidden && authority.SnapshotView().revision == revision);
	}
	{
		// So does a room bound to a tournament match, whoever holds host powers.
		RoomAuthority authority("Match", 8, 54);
		const std::string a(64, 'a'), b(64, 'b');
		EXPECT(authority.Join("A", ConnectionRef{"room", a}, true).accepted);
		TournamentBinding binding;
		binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.assignmentGeneration = 1; binding.bindingRevision = 1; binding.gamesToWin = 2;
		binding.fighters[0] = {a, "emb1_aaaa"}; binding.fighters[1] = {b, "emb1_bbbb"};
		EXPECT(authority.BindTournament(binding).accepted);
		const auto host = ActingHost(authority.SnapshotView());
		EXPECT(host != 0);
		Action action;
		action.kind = ActionKind::SetFriendsHidden; action.friendsHidden = true; action.actionId = 1;
		action.roomEpoch = authority.SnapshotView().roomEpoch; action.revision = authority.SnapshotView().revision;
		const auto revision = authority.SnapshotView().revision;
		EXPECT(authority.Apply(host, action).reason == RejectReason::Unauthorized);
		EXPECT(!authority.SnapshotView().friendsHidden && authority.SnapshotView().revision == revision);
	}
	{
		// The wire: a room that is visible has no friends_hidden, so an older
		// client reads it as before; hidden, the key travels and reads back.
		Room room;
		const nlohmann::json plain = room.authority.SnapshotFor(room.host);
		EXPECT(!plain.contains("friends_hidden") && !plain.get<Snapshot>().friendsHidden);
		EXPECT(room.SetFriendsHidden(room.host, true).accepted);
		const nlohmann::json hidden = room.authority.SnapshotFor(room.host);
		EXPECT(hidden.at("friends_hidden") == true && hidden.get<Snapshot>().friendsHidden);
		// The chat-free snapshot carries it too.
		EXPECT(room.authority.SnapshotFor(room.host, false).friendsHidden);
		// A state saved before the setting leaves it out: visible.
		auto older = hidden;
		older.erase("friends_hidden");
		EXPECT(!older.get<Snapshot>().friendsHidden);
		auto broken = hidden;
		broken["friends_hidden"] = 1;
		EXPECT(Throws(broken));
		broken["friends_hidden"] = "yes";
		EXPECT(Throws(broken));
		// The action: friends_hidden only when set, and an older authority
		// knows no kind past its last.
		auto action = room.Make(ActionKind::SetFriendsHidden);
		action.friendsHidden = true;
		const nlohmann::json sent = action;
		EXPECT(sent.at("friends_hidden") == true && sent.get<Action>().friendsHidden && sent.get<Action>().kind == ActionKind::SetFriendsHidden);
		EXPECT(static_cast<int>(ActionKind::SetFriendsHidden) <= static_cast<int>(LastActionKind));
		action.friendsHidden = false;
		const nlohmann::json off = action;
		EXPECT(!off.contains("friends_hidden") && !off.get<Action>().friendsHidden);
		auto notBoolean = sent;
		notBoolean["friends_hidden"] = 1;
		bool refused = false;
		try { (void)notBoolean.get<Action>(); } catch (const std::exception&) { refused = true; }
		EXPECT(refused);
		auto unknown = sent;
		unknown["kind"] = static_cast<int>(LastActionKind) + 1;
		bool threw = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// A checkpoint keeps the setting.
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && restored.SnapshotView().friendsHidden);
	}
	{
		// A room replacing a closed one is created as it was.
		Room room;
		EXPECT(room.SetFriendsHidden(room.host, true).accepted);
		const auto charter = CharterOf(room.View());
		EXPECT(charter.friendsHidden);
		RoomAuthority again("Again", 16, 55, FirstToOne(), charter);
		EXPECT(again.SnapshotView().friendsHidden);
		Room visible;
		EXPECT(!CharterOf(visible.View()).friendsHidden);
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room friends hidden setting passed.\n");
	return 0;
}
