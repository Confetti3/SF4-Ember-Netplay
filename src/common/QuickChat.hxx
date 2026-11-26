#pragma once
// Quick lines (docs/design/FIGHT_CHAT.md): the few room chat messages a fighter
// sends mid-fight without typing, with a pad's Back and a direction or F1 to F4.
// A line travels as its id (room::Action::quick, room::ChatMessage::quick) and
// with its English words as the message text, so each reader shows it in their
// own language and anything that does not know the id still reads the text.
// Kept to C++14: PlayerPreferences.hxx includes it.
#include <cstdint>

namespace sf4e {

enum class QuickLine : std::uint8_t { None, GoodGame, OneMore, Lag, Sorry, Nice, Thanks, GoodLuck, CloseOne };

namespace quickchat {
// Each line in id order (Table[i].line is QuickLine(i + 1)): its saved code, its
// catalog key and the English text the room carries.
struct Row { QuickLine line; const char* code; const char* key; const char* english; };
constexpr int Count = 8;
constexpr Row Table[Count] = {
	{QuickLine::GoodGame, "gg", "chat.quick.gg", "GG"},
	{QuickLine::OneMore, "one_more", "chat.quick.one_more", "One more?"},
	{QuickLine::Lag, "lag", "chat.quick.lag", "Lag?"},
	{QuickLine::Sorry, "sorry", "chat.quick.sorry", "Sorry!"},
	{QuickLine::Nice, "nice", "chat.quick.nice", "Nice!"},
	{QuickLine::Thanks, "thanks", "chat.quick.thanks", "Thanks!"},
	{QuickLine::GoodLuck, "good_luck", "chat.quick.good_luck", "Good luck!"},
	{QuickLine::CloseOne, "close_one", "chat.quick.close_one", "Close one!"},
};
// The quick slots: Up, Right, Down and Left with Back held, and F1 to F4.
constexpr int Slots = 4;

// The row of a line, or null for None and anything past the table: for a line not yet checked.
inline const Row* FindRow(QuickLine line) {
	const int index = static_cast<int>(line) - 1;
	return index >= 0 && index < Count ? &Table[index] : nullptr;
}
// The table is in the enum's order, so a line from it is its own index.
constexpr bool InEnumOrder(int index = 0) {
	return index == Count || (static_cast<int>(Table[index].line) == index + 1 && InEnumOrder(index + 1));
}
static_assert(InEnumOrder(), "the quick line table is in QuickLine's order");
// The row of a line from the table: a saved slot (PlayerPreferences::Valid keeps None
// and anything past the table out of them), a slot Settings stepped to, or a message
// the room took. FindRow is for any other value.
inline const Row& RowOf(QuickLine line) { return Table[static_cast<int>(line) - 1]; }
// The line a wire id names; None for 0 and anything past the table.
inline QuickLine FromWire(long long id) {
	return id >= 1 && id <= Count ? static_cast<QuickLine>(id) : QuickLine::None;
}
}

}
