// Quick lines in the room (docs/design/FIGHT_CHAT.md): the authority takes one only
// with its own English text, keeps its id, and holds it to every chat rule; the id
// travels in an action and a snapshot, and only a known one reads.
#include "room_authority_support.hxx"

// A quick line travels as its id with its English words: anything else is
// refused, an id outside the table cannot be read, and it keeps the one a second limit.
static void TestQuickChat() {
	RoomAuthority authority("Quick chat", 8, 315);
	authority.AdvanceTime(1000);
	const auto host = Join(authority, 0, true), guest = Join(authority, 1);
	const auto quick = [&](sf4e::QuickLine line, const char* text) {
		auto chat = TableAction(authority, guest, 0, ActionKind::Chat);
		chat.quick = line;
		chat.text = text;
		return authority.Apply(guest, chat);
	};
	EXPECT(quick(sf4e::QuickLine::Nice, "Nice!").accepted);
	const auto& chat = authority.SnapshotView().chat;
	EXPECT(chat.size() == 1 && chat[0].quick == sf4e::QuickLine::Nice && chat[0].text == "Nice!" && chat[0].sender == guest);
	authority.AdvanceTime(3000);
	EXPECT(quick(sf4e::QuickLine::Nice, "Thanks!").reason == RejectReason::InvalidChat);
	EXPECT(quick(sf4e::QuickLine::GoodGame, "gg").reason == RejectReason::InvalidChat);
	EXPECT(chat.size() == 1);
	// The one a second rule covers quick lines, and a quick line counts against typed text.
	EXPECT(quick(sf4e::QuickLine::OneMore, "One more?").accepted);
	EXPECT(quick(sf4e::QuickLine::Lag, "Lag?").reason == RejectReason::InvalidChat);
	// The id is written only when set, and one naming no row cannot be read.
	auto typed = TableAction(authority, host, 0, ActionKind::Chat);
	typed.text = "typed";
	EXPECT(!nlohmann::json(typed).contains("quick"));
	auto sent = TableAction(authority, host, 0, ActionKind::Chat);
	sent.quick = sf4e::QuickLine::Sorry;
	sent.text = "Sorry!";
	EXPECT(nlohmann::json(sent).get<Action>().quick == sf4e::QuickLine::Sorry);
	for (const nlohmann::json bad : {nlohmann::json(0), nlohmann::json(sf4e::quickchat::Count + 1), nlohmann::json(-1), nlohmann::json("nice"), nlohmann::json(1.5)}) {
		auto json = nlohmann::json(sent);
		json["quick"] = bad;
		bool refused = false;
		try { json.get<Action>(); } catch (const std::invalid_argument&) { refused = true; }
		EXPECT(refused);
	}
	// A snapshot round trip keeps the line; an id this build does not know reads as typed text.
	Snapshot parsed;
	auto json = nlohmann::json(authority.SnapshotFor(host));
	json.get_to(parsed);
	EXPECT(parsed.chat.size() == 2 && parsed.chat[0].quick == sf4e::QuickLine::Nice && parsed.chat[1].quick == sf4e::QuickLine::OneMore);
	json["chat"][0]["quick"] = 99;
	json.get_to(parsed);
	EXPECT(parsed.chat[0].quick == sf4e::QuickLine::None && parsed.chat[0].text == "Nice!");
}

int main() {
	TestQuickChat();
	if (TEST_FAILURES()) return 1;
	std::puts("RoomQuickChat test passed");
	return 0;
}
