#pragma once
// The parts a card is drawn from (docs/design/PROFILE.md, "Rules for every
// card size"): the two chip groups, a family on its tier plate, a recent set's
// row, the presence mark. They draw what CardModel.hxx made and decide nothing;
// each takes its sizes from player_card::Metrics, never a literal of its own.
#include "CardModel.hxx"
#include "V2.hxx"
#include <imgui.h>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace card_parts {

// The width of a text at `size` in `font` (the current one when none).
float Across(const std::string& text, float size, ImFont* font = nullptr);

// A chip group: its label (muted, `labelSize`), then its chips, `row` tall.
// Plays chips are the muted v2::Chip; Looking for chips have no ground, a 1 px
// accent outline and `c.text`: thin, rounded, with words, never a frame.
// GroupWidth is what it takes; DrawGroup draws it at `at` and returns that.
float GroupWidth(const std::string& label, const std::vector<player_card::Chip>& chips, float labelSize);
float DrawGroup(ImDrawList* d, ImVec2 at, float row, float labelSize, const std::string& label, const std::vector<player_card::Chip>& chips);
// Both groups on one line `gap` apart, Looking for on a line of its own when
// they do not fit `width`. A group with no chips is left out with its label.
// Returns the height (`row` or twice it with `lineGap`).
float DrawGroupsTogether(ImDrawList* d, ImVec2 at, float width, float row, float labelSize, float gap, float lineGap, const std::vector<player_card::Chip>& plays,
    const std::vector<player_card::Chip>& looking);
// Each group on its own line, Plays giving way (Region, then Connection) until
// it fits `width`; Looking for is never shortened, and wraps to a second line
// only when even it does not fit. Returns the height.
float DrawGroupsStacked(ImDrawList* d, ImVec2 at, float width, float row, float labelSize, float lineGap, const std::vector<player_card::Chip>& plays,
    const std::vector<player_card::Chip>& looking);
// What the stacked groups take, for a Panel sized before it is drawn.
float GroupsStackedHeight(float width, float row, float labelSize, float lineGap, const std::vector<player_card::Chip>& plays, const std::vector<player_card::Chip>& looking);

// A family's icon on a square of its tier's plate, in the tier's stripe colour,
// no frame: the worn icon, the Panel's icons. `icon` is the glyph's size in it.
void DrawIconPlate(ImDrawList* d, ImVec2 at, float side, float icon, int family, v2::Tier tier);
// The level pips: `count` squares `pip` wide, `gap` apart, the first `level` in `tier`'s stripe colour, the rest the border.
// Returns the width. `centre` is the x they are centred on.
float DrawPips(ImDrawList* d, float centre, float y, int count, int level, float pip, float gap, v2::Tier tier);
// One family of the showcase in `box`: the icon on its plate, the pips, the title under them (two lines at most).
void DrawFamilyTile(ImDrawList* d, v2::Box box, const player_card::FamilyTile& tile, const player_card::Metrics& m, bool focused, bool hovered);
// The glyph to draw for a button the card names by its pad button ("X", "Y"): the one the hint bar shows for the device in use (a
// key on the keyboard), null where the device has no such button. Other names come back as they are.
const char* DeviceGlyph(const char* padButton);
// Presence as a mark that differs in shape: a disc in a room, a ring online, a dashed ring offline.
void DrawPresence(ImDrawList* d, ImVec2 centre, float radius, player_card::PresenceMark mark);
// The player's flag, "vs" or nothing before it, name and class on a line of `size` at (x, y), cut to `room`.
void DrawPlayerLine(ImDrawList* d, float x, float y, float room, float size, float emblem, const std::string& lead, const std::string& name, CountryCode country, const SkillBadge& badge);
// A recent set's row in `box`: the two fighters, "vs" and the opponent, the line under it.
void DrawSetRow(ImDrawList* d, v2::Box box, const player_card::SetRow& row, FighterArt art, const player_card::Metrics& m, bool focused, bool hovered);
// The most played opponent's row, and a plain link row ("All sets") with its chevron.
void DrawRivalRow(ImDrawList* d, v2::Box box, const player_card::Rival& rival, const player_card::Metrics& m, bool focused, bool hovered);
void DrawLinkRow(ImDrawList* d, v2::Box box, const std::string& label, float text, bool focused, bool hovered);
// A set row's empty ground while the sets are read, with `words` in the first.
void DrawReadingRow(ImDrawList* d, v2::Box box, const std::string& words, float text);

} } }
