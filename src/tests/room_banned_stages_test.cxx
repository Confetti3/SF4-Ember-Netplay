// A room's Banned stages rule: its host replaces the set (SetBannedStages);
// nobody else can, and a server-owned (public) room keeps it empty. It gates no
// seat and moves nobody, and it travels on the wire only when something is
// banned, so a room without it reads as before. And StageUnderBans, the one
// decision of the stage P1's Ready sends under it.
#include "../session/RoomModel.hxx"
#include "../common/StageBans.hxx"
#include "../common/GameCatalog.hxx"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using sf4e::NetworkLink;
using namespace sf4e::room;
using Stages = std::vector<std::int64_t>;

namespace {
// The first valid stage ids of this game's catalog.
Stages ValidStages(std::size_t count) {
	Stages stages;
	for (std::int64_t id = 0; id < 1000 && stages.size() < count; id++)
		if (ember::Catalog().validStage(id)) stages.push_back(id);
	EXPECT(stages.size() == count);
	return stages;
}

struct Room {
	RoomAuthority authority;
	MemberId host = 0, guest = 0;
	std::uint64_t nextAction = 1;
	int nextConnection = 0;

	explicit Room(Stages banned = {})
		: authority("Stages room", 16, 54, Rules(), false, DelayPolicy::Shared, ClassicRoom,
			static_cast<std::uint8_t>(ClassicTableCount), false, false, true, std::move(banned)) {
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
	Result Set(MemberId member, Stages stages) {
		auto action = Make(ActionKind::SetBannedStages);
		action.bannedStages = std::move(stages);
		return authority.Apply(member, action);
	}
};

constexpr int kRandom = -2;   // the test game's Random choice
constexpr int kTraining = 90; // the test game's Training stage
// A game whose stages are 0 to 9 and 90 (Training); Random rolls over those
// not excluded, the roll modulo how many are left.
int Resolve(int choice, std::uint32_t roll, const Stages& excluded) {
	if (choice != kRandom) return choice;
	std::vector<int> left;
	for (int id = 0; id < 10; id++)
		if (!std::binary_search(excluded.begin(), excluded.end(), static_cast<std::int64_t>(id))) left.push_back(id);
	return left.empty() ? kRandom : left[roll % left.size()];
}
int Under(int choice, std::uint32_t roll, const Stages& unavailable, const Stages& banned, std::int64_t training = kTraining) {
	return sf4e::StageUnderBans(choice, kRandom, training, unavailable, banned,
		[&](const Stages& excluded) { return Resolve(choice, roll, excluded); });
}
}

int main() {
	const Stages ids = ValidStages(4);
	{
		// Empty by default; the host sets and clears it, a member cannot.
		Room room;
		EXPECT(room.View().bannedStages.empty());
		const auto refused = room.Set(room.guest, {ids[0]});
		EXPECT(!refused.accepted && refused.reason == RejectReason::NotHost && room.View().bannedStages.empty());
		const auto before = room.View().revision;
		EXPECT(room.Set(room.host, {ids[2], ids[0]}).accepted);
		// Kept ascending.
		EXPECT(room.View().bannedStages == (Stages{ids[0], ids[2]}));
		EXPECT(room.View().revision > before);
		// The same set again is accepted and changes nothing.
		const auto after = room.View().revision;
		EXPECT(room.Set(room.host, {ids[0], ids[2], ids[0]}).accepted && room.View().revision == after);
		EXPECT(room.Set(room.host, {}).accepted && room.View().bannedStages.empty());
	}
	{
		// What is not a stage is dropped, and the list is capped.
		Room room;
		EXPECT(room.Set(room.host, {ids[1], 100000, -5}).accepted);
		EXPECT(room.View().bannedStages == Stages{ids[1]});
		Stages many;
		for (std::int64_t id = 0; id < 5000; id++) many.push_back(id);
		EXPECT(BannedStagesFrom(many).size() <= MaximumBannedStages);
	}
	{
		// Chosen when the room is created (a replacement room keeps it), and in every member's view.
		Room room(Stages{ids[3], ids[1]});
		EXPECT(room.View().bannedStages == (Stages{ids[1], ids[3]}));
		EXPECT(room.authority.SnapshotFor(room.guest).bannedStages == room.View().bannedStages);
		EXPECT(room.authority.SnapshotFor(room.guest, false).bannedStages == room.View().bannedStages);
	}
	{
		// A stale revision is refused, as with the other room settings.
		Room room;
		auto action = room.Make(ActionKind::SetBannedStages);
		action.bannedStages = {ids[0]};
		action.revision = room.View().revision + 7;
		const auto result = room.authority.Apply(room.host, action);
		EXPECT(!result.accepted && result.reason == RejectReason::StaleRoom && room.View().bannedStages.empty());
	}
	{
		// A public (server-owned) room keeps it empty: no member sets it.
		RoomAuthority authority("Open room", 8, 9, PublicRoomRules());
		EXPECT(authority.SetServerOwned());
		MemberProfile profile;
		profile.link = NetworkLink::Wired;
		profile.admission.account = "emb1-member";
		const auto joined = authority.Join("Member", ConnectionRef{"host", "1"}, false, profile);
		EXPECT(joined.accepted && !joined.snapshot.members.empty());
		if (!joined.accepted || joined.snapshot.members.empty()) return 1;
		Action action;
		action.kind = ActionKind::SetBannedStages;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.actionId = 1;
		action.bannedStages = {ids[0]};
		const auto result = authority.Apply(joined.snapshot.members.back().id, action);
		EXPECT(!result.accepted && authority.SnapshotView().bannedStages.empty());
		const nlohmann::json json = authority.SnapshotView();
		EXPECT(!json.contains("banned_stages"));
	}
	{
		// It gates no seat.
		Room room(Stages{ids[0]});
		EXPECT(room.authority.Apply(room.guest, room.Make(ActionKind::Queue)).accepted);
	}
	{
		// The key is written only when something is banned; a bad value bans nothing; a checkpoint keeps the rule.
		Room room;
		const nlohmann::json none = room.View();
		EXPECT(!none.contains("banned_stages") && none.get<Snapshot>().bannedStages.empty());
		EXPECT(room.Set(room.host, {ids[2], ids[0]}).accepted);
		const nlohmann::json some = room.View();
		EXPECT(some.at("banned_stages") == nlohmann::json(Stages{ids[0], ids[2]}));
		EXPECT(some.get<Snapshot>().bannedStages == (Stages{ids[0], ids[2]}));
		auto bad = some;
		bad["banned_stages"] = "all";
		EXPECT(bad.get<Snapshot>().bannedStages.empty());
		bad["banned_stages"] = nlohmann::json::array({ids[1], "x", 99999, ids[1], 1.5});
		EXPECT(bad.get<Snapshot>().bannedStages == Stages{ids[1]});
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && restored.SnapshotView().bannedStages == (Stages{ids[0], ids[2]}));
	}
	{
		// The action: the key only when not empty, an unknown kind past the last is refused.
		Room room;
		auto action = room.Make(ActionKind::SetBannedStages);
		action.bannedStages = {ids[1]};
		const nlohmann::json sent = action;
		EXPECT(sent.get<Action>().bannedStages == Stages{ids[1]} && sent.get<Action>().kind == ActionKind::SetBannedStages);
		action.bannedStages.clear();
		const nlohmann::json empty = action;
		EXPECT(!empty.contains("banned_stages") && empty.get<Action>().bannedStages.empty());
		const nlohmann::json other = room.Make(ActionKind::Lock);
		EXPECT(!other.contains("banned_stages"));
		auto unknown = sent;
		unknown["kind"] = static_cast<int>(LastActionKind) + 1;
		bool threw = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// SetBannedStages is the last kind: SetSkipScenes stays where it was.
		EXPECT(static_cast<int>(ActionKind::SetBannedStages) == static_cast<int>(ActionKind::SetSkipScenes) + 1);
	}
	{
		// Nothing banned: the game's own choice, whatever it is.
		EXPECT(Under(3, 0, {}, {}) == 3);
		EXPECT(Under(kRandom, 7, {}, {}) == 7);
		EXPECT(Under(kRandom, 7, {7}, {}) == 8); // the opponent's lack leaves 0-6, 8, 9: roll 7 of 9 is 8
	}
	{
		// A banned specific stage becomes the Training stage; another stands.
		EXPECT(Under(3, 0, {}, {3, 5}) == kTraining);
		EXPECT(Under(4, 0, {}, {3, 5}) == 4);
		// A banned stage the opponent also lacks is the Training stage too.
		EXPECT(Under(5, 0, {5}, {3, 5}) == kTraining);
		// A game with no Training stage leaves the choice as the game made it.
		EXPECT(Under(3, 0, {}, {3}, -1) == 3);
	}
	{
		// Random rolls over the stages that are neither banned nor missing, never a banned one.
		const Stages banned = {0, 1, 2, 3, 4, 5, 6};
		std::set<int> seen;
		for (std::uint32_t roll = 0; roll < 60; roll++) {
			const int stage = Under(kRandom, roll, {}, banned);
			EXPECT(stage >= 7 && stage <= 9);
			seen.insert(stage);
		}
		EXPECT(seen == (std::set<int>{7, 8, 9}));
		// The opponent's lack and the bans together.
		for (std::uint32_t roll = 0; roll < 60; roll++) EXPECT(Under(kRandom, roll, {8}, banned) == (roll % 2 ? 9 : 7));
		// The same snapshot and roll give the same stage, every time.
		for (std::uint32_t roll = 0; roll < 20; roll++) EXPECT(Under(kRandom, roll, {2}, banned) == Under(kRandom, roll, {2}, banned));
	}
	{
		// Every stage banned: Random and a banned pick both become the Training stage.
		Stages all;
		for (std::int64_t id = 0; id < 10; id++) all.push_back(id);
		EXPECT(Under(kRandom, 3, {}, all) == kTraining);
		EXPECT(Under(4, 3, {}, all) == kTraining);
		// Or all but what the opponent lacks.
		EXPECT(Under(kRandom, 3, {9}, Stages{0, 1, 2, 3, 4, 5, 6, 7, 8}) == kTraining);
		// No Training stage: Random stays unresolved, as the Ready check wants.
		EXPECT(Under(kRandom, 3, {}, all, -1) == kRandom);
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room banned stages rules passed.\n");
	return 0;
}
