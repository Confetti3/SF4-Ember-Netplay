// A room's Wired players only rule: chosen when the room is created, changed
// by its host (SetWiredOnly), it keeps a member on Wi-Fi from the next seat,
// queue turn or cabinet, never one on an Unknown link, and moves nobody who
// already sits. It travels on the wire only when on.
#include "../session/RoomModel.hxx"

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using sf4e::NetworkLink;
using sf4e::NetworkLinkFromWire;
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

	explicit Room(bool wiredOnly, RoomType type = ClassicRoom)
		: authority("Wired room", 16, 51, FirstToOne(), false, DelayPolicy::Shared, type,
			static_cast<std::uint8_t>(ClassicTableCount), wiredOnly) {
		authority.AdvanceTime(1000);
		host = Join("Host", NetworkLink::Wired, true);
	}
	MemberId Join(const char* name, NetworkLink link, bool isHost = false) {
		MemberProfile profile;
		profile.link = link;
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(nextConnection++)}, isHost, profile);
		EXPECT(result.accepted);
		return result.accepted ? result.snapshot.members.back().id : 0;
	}
	const Snapshot& View() const { return authority.SnapshotView(); }
	const Member* Find(MemberId member) const { return FindMember(View(), member); }
	Action Make(ActionKind kind, std::uint8_t table = 0, int seat = -1) {
		Action action;
		action.kind = kind;
		action.roomEpoch = View().roomEpoch;
		action.revision = View().revision;
		action.table = table;
		action.seat = static_cast<std::int8_t>(seat);
		action.tableRevision = View().tables[table].revision;
		action.actionId = nextAction++;
		return action;
	}
	Result At(MemberId member, ActionKind kind, std::uint8_t table = 0, int seat = -1) {
		return authority.Apply(member, Make(kind, table, seat));
	}
	Result SetWiredOnly(MemberId member, bool on) {
		auto action = Make(ActionKind::SetWiredOnly);
		action.wiredOnly = on;
		return authority.Apply(member, action);
	}
	// Both fighters ready and the game runs; it ends with `result`.
	void Play(std::uint8_t table, MatchResult result) {
		const auto& t = View().tables[table];
		const MemberId p1 = t.p1, p2 = t.p2;
		EXPECT(At(p1, ActionKind::Ready, table).accepted && At(p2, ActionKind::Ready, table).accepted);
		EXPECT(authority.BeginMatch(table, p1, p2, sf4e::room::MatchStage::Unsent()).accepted);
		EXPECT(authority.EndMatch(table, View().tables[table].matchGeneration, result).accepted);
	}
};

bool Throws(const nlohmann::json& json) {
	try { (void)json.get<Snapshot>(); } catch (const std::exception&) { return true; }
	return false;
}
}

int main() {
	{
		// The pure rule: only Wi-Fi is kept out, and only while the rule is on.
		Snapshot room;
		for (const auto link : {NetworkLink::Unknown, NetworkLink::Wired, NetworkLink::Wireless}) EXPECT(LinkMaySit(room, link));
		room.wiredOnly = true;
		EXPECT(LinkMaySit(room, NetworkLink::Unknown) && LinkMaySit(room, NetworkLink::Wired) && !LinkMaySit(room, NetworkLink::Wireless));
		// A link value from a newer peer reads as Unknown, so it sits.
		EXPECT(LinkMaySit(room, NetworkLinkFromWire(7)));
	}
	{
		// Off by default: a player on Wi-Fi sits as anyone does.
		Room room(false);
		EXPECT(!room.View().wiredOnly);
		const auto wifi = room.Join("Alex", NetworkLink::Wireless);
		EXPECT(room.At(wifi, ActionKind::Queue, 0, 0).accepted && room.View().tables[0].p1 == wifi);
	}
	{
		// Created wired only: Wi-Fi is refused a named seat, the queue and the
		// first open seat; Wired and Unknown take them. The refusal changes nothing.
		Room room(true);
		EXPECT(room.View().wiredOnly);
		const auto wifi = room.Join("Alex", NetworkLink::Wireless);
		const auto wired = room.Join("Sam", NetworkLink::Wired);
		const auto unknown = room.Join("Deck", NetworkLink::Unknown);
		const auto revision = room.View().revision;
		EXPECT(room.At(wifi, ActionKind::Queue, 0, 0).reason == RejectReason::WiredOnly);
		EXPECT(room.At(wifi, ActionKind::Queue, 0).reason == RejectReason::WiredOnly);
		EXPECT(room.View().revision == revision && room.Find(wifi)->status == MemberStatus::Idle && room.View().tables[0].queue.empty());
		EXPECT(room.At(wired, ActionKind::Queue, 0, 0).accepted && room.View().tables[0].p1 == wired);
		EXPECT(room.At(unknown, ActionKind::Queue, 0).accepted && room.View().tables[0].p2 == unknown);
		// A queue behind a full table is refused to Wi-Fi too.
		EXPECT(room.At(wifi, ActionKind::Queue, 0).reason == RejectReason::WiredOnly);
		// Wi-Fi still watches and chats.
		EXPECT(room.At(wifi, ActionKind::Watch, 0).accepted && room.Find(wifi)->status == MemberStatus::WatchingNext);
		auto chat = room.Make(ActionKind::Chat);
		chat.text = "gg";
		EXPECT(room.authority.Apply(wifi, chat).accepted);
	}
	{
		// The host turns it on and off; nobody else can, and a stale copy is refused.
		Room room(false);
		const auto other = room.Join("Sam", NetworkLink::Wired);
		EXPECT(room.SetWiredOnly(other, true).reason == RejectReason::NotHost && !room.View().wiredOnly);
		auto stale = room.Make(ActionKind::SetWiredOnly);
		stale.wiredOnly = true; stale.revision -= 1;
		EXPECT(room.authority.Apply(room.host, stale).reason == RejectReason::StaleRoom && !room.View().wiredOnly);
		const auto before = room.View().revision;
		EXPECT(room.SetWiredOnly(room.host, true).accepted && room.View().wiredOnly && room.View().revision > before);
		// The same value again changes nothing.
		const auto on = room.View().revision;
		EXPECT(room.SetWiredOnly(room.host, true).accepted && room.View().revision == on);
		EXPECT(room.SetWiredOnly(room.host, false).accepted && !room.View().wiredOnly);
	}
	{
		// Turned on mid-set, it unseats nobody: the Wi-Fi fighter readies, plays
		// and, winning, keeps the seat (Winner stays is no new seat).
		Room room(false);
		const auto wifi = room.Join("Alex", NetworkLink::Wireless);
		const auto wired = room.Join("Sam", NetworkLink::Wired);
		EXPECT(room.At(wifi, ActionKind::Queue, 0, 0).accepted && room.At(wired, ActionKind::Queue, 0, 1).accepted);
		EXPECT(room.At(wifi, ActionKind::Ready, 0).accepted);
		EXPECT(room.SetWiredOnly(room.host, true).accepted);
		EXPECT(room.View().tables[0].p1 == wifi && room.View().tables[0].ready[0] && room.Find(wifi)->status == MemberStatus::Ready);
		EXPECT(room.At(wired, ActionKind::Ready, 0).accepted);
		EXPECT(room.authority.BeginMatch(0, wifi, wired, sf4e::room::MatchStage::Unsent()).accepted && room.View().tables[0].phase == TablePhase::Playing);
		EXPECT(room.SetWiredOnly(room.host, false).accepted && room.SetWiredOnly(room.host, true).accepted);
		EXPECT(room.View().tables[0].phase == TablePhase::Playing && room.View().tables[0].p1 == wifi);
		EXPECT(room.authority.EndMatch(0, room.View().tables[0].matchGeneration, MatchResult::P1Win).accepted);
		EXPECT(room.View().tables[0].p1 == wifi);
	}
	{
		// Rotation: a Wi-Fi member queued before the rule came on is passed over
		// when their turn comes. They leave the queue, the next one sits, and
		// the rotated loser keeps their place behind.
		Room room(false);
		const auto a = room.Join("Player One", NetworkLink::Wired);
		const auto b = room.Join("Player Two", NetworkLink::Wired);
		const auto wifi = room.Join("Alex", NetworkLink::Wireless);
		const auto deck = room.Join("Deck", NetworkLink::Unknown);
		EXPECT(room.At(a, ActionKind::Queue, 0, 0).accepted && room.At(b, ActionKind::Queue, 0, 1).accepted);
		EXPECT(room.At(wifi, ActionKind::Queue, 0).accepted && room.At(deck, ActionKind::Queue, 0).accepted);
		EXPECT(room.Find(wifi)->status == MemberStatus::Queued);
		EXPECT(room.SetWiredOnly(room.host, true).accepted);
		// Queued while seats are full, they keep their place.
		EXPECT(room.View().tables[0].queue.size() == 2 && room.View().tables[0].queue.front() == wifi);
		room.Play(0, MatchResult::P1Win);
		const auto& table = room.View().tables[0];
		EXPECT(table.p1 == a && table.p2 == deck);
		EXPECT(table.queue.size() == 1 && table.queue.front() == b);
		EXPECT(room.Find(wifi)->status == MemberStatus::Idle && room.Find(wifi)->table == -1);
		EXPECT(room.Find(deck)->status == MemberStatus::Seated && room.Find(b)->status == MemberStatus::Queued);
		// An open seat still means an empty queue.
		EXPECT(!SeatOpenNow(table, 0) && !SeatOpenNow(table, 1));
	}
	{
		// The same pass-over when a fighter leaves: only Wi-Fi queued, the seat stays open.
		Room room(false);
		const auto a = room.Join("Player One", NetworkLink::Wired);
		const auto b = room.Join("Player Two", NetworkLink::Wired);
		const auto wifi = room.Join("Alex", NetworkLink::Wireless);
		EXPECT(room.At(a, ActionKind::Queue, 0, 0).accepted && room.At(b, ActionKind::Queue, 0, 1).accepted);
		EXPECT(room.At(wifi, ActionKind::Queue, 0).accepted);
		EXPECT(room.SetWiredOnly(room.host, true).accepted);
		EXPECT(room.At(b, ActionKind::Unqueue, 0).accepted);
		const auto& table = room.View().tables[0];
		EXPECT(table.p1 == a && table.p2 == 0 && table.queue.empty() && room.Find(wifi)->status == MemberStatus::Idle);
		EXPECT(SeatOpenNow(table, 1));
	}
	{
		// Arcade: Wi-Fi does not sit at a cabinet; Unknown and Wired do. One
		// already sitting before the rule moves between cabinets.
		Room room(false, ArcadeRoom);
		const auto wifi = room.Join("Alex", NetworkLink::Wireless);
		EXPECT(room.At(wifi, ActionKind::Queue, 0, 0).accepted && CabinetOf(room.View(), wifi) == 0);
		EXPECT(room.SetWiredOnly(room.host, true).accepted);
		EXPECT(room.At(wifi, ActionKind::Queue, 1, 0).accepted && CabinetOf(room.View(), wifi) == 1);
		const auto late = room.Join("Riley", NetworkLink::Wireless);
		EXPECT(room.At(late, ActionKind::Queue, 2, 0).reason == RejectReason::WiredOnly && CabinetOf(room.View(), late) < 0);
		const auto deck = room.Join("Deck", NetworkLink::Unknown);
		const auto wired = room.Join("Sam", NetworkLink::Wired);
		EXPECT(room.At(deck, ActionKind::Queue, 2, 0).accepted && room.At(wired, ActionKind::Queue, 3, 0).accepted);
	}
	{
		// The wire: a room without the rule has no wired_only, so an older
		// client reads it as before; with it, the key travels and reads back.
		Room room(false);
		const nlohmann::json plain = room.authority.SnapshotFor(room.host);
		EXPECT(!plain.contains("wired_only") && !plain.get<Snapshot>().wiredOnly);
		EXPECT(room.SetWiredOnly(room.host, true).accepted);
		const nlohmann::json wired = room.authority.SnapshotFor(room.host);
		EXPECT(wired.at("wired_only") == true && wired.get<Snapshot>().wiredOnly);
		// The chat-free snapshot carries it too.
		EXPECT(room.authority.SnapshotFor(room.host, false).wiredOnly);
		// A state saved before the rule leaves it out: off.
		auto older = wired;
		older.erase("wired_only");
		EXPECT(!older.get<Snapshot>().wiredOnly);
		auto broken = wired;
		broken["wired_only"] = 1;
		EXPECT(Throws(broken));
		// The action: wired_only only when on, and an older authority knows no
		// kind past its last.
		auto action = room.Make(ActionKind::SetWiredOnly);
		action.wiredOnly = true;
		const nlohmann::json sent = action;
		EXPECT(sent.at("wired_only") == true && sent.get<Action>().wiredOnly && sent.get<Action>().kind == ActionKind::SetWiredOnly);
		action.wiredOnly = false;
		const nlohmann::json off = action;
		EXPECT(!off.contains("wired_only") && !off.get<Action>().wiredOnly);
		auto unknown = sent;
		unknown["kind"] = static_cast<int>(LastActionKind) + 1;
		bool threw = false;
		try { (void)unknown.get<Action>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
		// A checkpoint keeps the rule.
		RoomAuthority restored("Restored");
		EXPECT(restored.RestoreCheckpoint(room.authority.Checkpoint()) && restored.SnapshotView().wiredOnly);
	}
	{
		// The refusal reaches the member as itself; a reason past the last is refused whole.
		Room room(true);
		const auto wifi = room.Join("Alex", NetworkLink::Wireless);
		const nlohmann::json wire = room.At(wifi, ActionKind::Queue, 0);
		EXPECT(wire.at("reason") == static_cast<int>(RejectReason::WiredOnly) && wire.get<Result>().reason == RejectReason::WiredOnly);
		auto later = wire;
		later["reason"] = static_cast<int>(LastRejectReason) + 1;
		bool threw = false;
		try { (void)later.get<Result>(); } catch (const std::exception&) { threw = true; }
		EXPECT(threw);
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room wired only rules passed.\n");
	return 0;
}
