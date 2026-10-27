#include "room_authority_support.hxx"

#include <functional>

// The input delay limits. A fighter can now choose up to MaximumInputDelay,
// but snapshots, checkpoints and actions from earlier builds hold up to
// InputDelayWireCeiling and must still load.

using sf4e::InputDelayWireCeiling;
using sf4e::MaximumInputDelay;
using nlohmann::json;

static void Walk(json& value, const std::function<void(json&)>& visit) {
	if (value.is_object()) {
		visit(value);
		for (auto& item : value.items()) Walk(item.value(), visit);
	} else if (value.is_array()) {
		for (auto& item : value) Walk(item, visit);
	}
}

// A room with two fighters seated at table 0, both Ready at `delay`.
static void ReadyBoth(RoomAuthority& authority, MemberId& p1, MemberId& p2, int delay) {
	Join(authority, 0, true);
	p1 = Join(authority, 1);
	p2 = Join(authority, 2);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	for (const auto member : {p1, p2}) {
		Action action = TableAction(authority, member, 0, ActionKind::Ready);
		action.inputDelay = static_cast<std::uint8_t>(delay);
		EXPECT(authority.Apply(member, action).accepted);
	}
}

static void TestReadyAboveTheChoiceLimitIsRecordedAtTheLimit() {
	for (const int asked : {MaximumInputDelay, MaximumInputDelay + 1, 8, InputDelayWireCeiling}) {
		RoomAuthority authority("Limit", 4, 1);
		MemberId p1 = 0, p2 = 0;
		ReadyBoth(authority, p1, p2, asked);
		const int expected = (std::min)(asked, MaximumInputDelay);
		const auto& table = authority.SnapshotView().tables[0];
		EXPECT(table.inputDelay[0] == expected && table.inputDelay[1] == expected && MatchDelay(table) == expected);
		for (const auto member : {p1, p2}) {
			const auto* item = FindMember(authority.SnapshotView(), member);
			EXPECT(item && item->selectedDelay == expected && item->frozenDelay == expected && item->delayLocked);
		}
	}
	// Past the wire ceiling an action is still refused, and changes nothing.
	RoomAuthority authority("Limit", 4, 1);
	Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	Action over = TableAction(authority, p1, 0, ActionKind::Ready);
	over.inputDelay = static_cast<std::uint8_t>(InputDelayWireCeiling + 1);
	const auto refused = authority.Apply(p1, over);
	EXPECT(!refused.accepted && refused.reason == RejectReason::Unauthorized);
	EXPECT(!authority.SnapshotView().tables[0].ready[0] && !authority.SnapshotView().tables[0].ready[1]);
}

static void TestStoredDelaysAboveTheChoiceLimitStillLoad() {
	RoomAuthority authority("Stored", 4, 1);
	MemberId p1 = 0, p2 = 0;
	ReadyBoth(authority, p1, p2, 2);
	// What an earlier build wrote for a fighter who chose 8 and one who chose 10.
	const auto older = [](json& state) {
		Walk(state, [](json& object) {
			if (object.contains("selected_delay")) object["selected_delay"] = 8;
			if (object.contains("frozen_delay")) object["frozen_delay"] = 8;
			if (object.contains("input_delay") && object["input_delay"].is_array()) object["input_delay"] = json::array({8, InputDelayWireCeiling});
		});
	};

	json snapshot = authority.SnapshotView();
	older(snapshot);
	const auto decoded = snapshot.get<Snapshot>();
	EXPECT(decoded.tables[0].inputDelay[0] == 8 && decoded.tables[0].inputDelay[1] == InputDelayWireCeiling);
	EXPECT(MatchDelay(decoded.tables[0]) == InputDelayWireCeiling);
	for (const auto& member : decoded.members)
		if (member.delayLocked) EXPECT(member.selectedDelay == 8 && member.frozenDelay == 8);

	json checkpoint = authority.Checkpoint();
	older(checkpoint);
	RoomAuthority restored("Restored", 4, 1);
	EXPECT(restored.RestoreCheckpoint(json::parse(checkpoint.dump())));
	EXPECT(restored.SnapshotView().tables[0].inputDelay[0] == 8);
	EXPECT(FindMember(restored.SnapshotView(), p1)->frozenDelay == 8);
	// A committed match keeps the delay it recorded.
	EXPECT(MatchDelay(restored.SnapshotView().tables[0]) == InputDelayWireCeiling);

	// An action carries the wire range too.
	Action action = TableAction(authority, p1, 0, ActionKind::Ready);
	for (const int delay : {0, MaximumInputDelay, 8, InputDelayWireCeiling}) {
		json wire = action;
		wire["input_delay"] = delay;
		EXPECT(wire.get<Action>().inputDelay == delay);
	}
}

static void TestMalformedDelaysStayRejected() {
	RoomAuthority authority("Malformed", 4, 1);
	MemberId p1 = 0, p2 = 0;
	ReadyBoth(authority, p1, p2, 2);
	const json bad[] = {json(-1), json(InputDelayWireCeiling + 1), json(255), json(1.5), json("3"), json(nullptr), json(true)};
	for (const auto& value : bad) {
		for (const char* key : {"selected_delay", "frozen_delay"}) {
			json snapshot = authority.SnapshotView();
			bool changed = false;
			Walk(snapshot, [&](json& object) { if (object.contains(key)) { object[key] = value; changed = true; } });
			bool threw = false;
			try { (void)snapshot.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
			EXPECT(changed && threw);
		}
		json table = authority.SnapshotView();
		Walk(table, [&](json& object) { if (object.contains("input_delay") && object["input_delay"].is_array()) object["input_delay"] = json::array({2, value}); });
		bool tableThrew = false;
		try { (void)table.get<Snapshot>(); } catch (const std::exception&) { tableThrew = true; }
		EXPECT(tableThrew);

		json checkpoint = authority.Checkpoint();
		Walk(checkpoint, [&](json& object) { if (object.contains("frozen_delay")) object["frozen_delay"] = value; });
		RoomAuthority restored("Restored", 4, 1);
		EXPECT(!restored.RestoreCheckpoint(json::parse(checkpoint.dump())));

		json action = TableAction(authority, p1, 0, ActionKind::Ready);
		action["input_delay"] = value;
		bool actionThrew = false;
		try { (void)action.get<Action>(); } catch (const std::exception&) { actionThrew = true; }
		EXPECT(actionThrew);
	}
}

int main() {
	TestReadyAboveTheChoiceLimitIsRecordedAtTheLimit();
	TestStoredDelaysAboveTheChoiceLimitStillLoad();
	TestMalformedDelaysStayRejected();
	if (TEST_FAILURES()) return 1;
	std::puts("RoomDelayLimits test passed");
	return 0;
}
