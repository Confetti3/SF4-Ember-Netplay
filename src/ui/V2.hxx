#pragma once
// Ember's V2 look (the V2 Overhaul design canvas): one switch, the layout rules
// and the small set of parts every V2 screen is built from. Presentation only:
// nothing here reads or changes navigation, so a screen draws the same entries
// in either look and the player can switch between them at any time.
#include "NameMarks.hxx"
#include "RoomCards.hxx"
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace v2 {
// The player's Settings > Interface > Look. Set once per frame from their preferences.
bool On();
void SetOn(bool on);

// Whose look a screen wears: Ember's, or BluMint's in the rooms of BluMint's
// matches (with BluMint's permission). Set each frame by what is on screen.
enum class Brand { Ember, BluMint };
void SetBrand(Brand brand);
Brand CurrentBrand();
// The palette roles of Theme.cxx by name, in the brand's colours, so V2
// parts never write a literal: grounds from ink to hover, the accent that
// marks focus and the thing to act on, text, and the red of a LIVE mark.
struct Palette { ImU32 ink, frame, panel, button, header, hover, border, accent, accentLight, text, muted, live; };
const Palette& Colors();
// BluMint's wordmark in the font atlas (Theme.cxx), after Ember's mark.
constexpr int WordmarkRect = 1;
namespace color {
// `color` at `alpha` (0 to 1).
constexpr ImU32 Alpha(ImU32 color, float alpha) { return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha * 255) << IM_COL32_A_SHIFT); }
}

// Layout, in physical pixels for an interface of `scale`. The side margin is
// 16 under 900 logical pixels of width, 24 under 1700, 40 above; content is
// capped at 2000 and centred, so a 21:9 or 32:9 screen grows margins, not rows.
constexpr float ContentCap = 2000;
inline float Pad(float width, float scale) {
    const float logical = width / scale;
    return (logical < 900 ? 16 : logical < 1700 ? 24 : 40) * scale;
}
inline bool Narrow(float width, float scale) { return width / scale < 900; }
inline float SideMargin(float width, float scale) { return (std::max)(Pad(width, scale), (width - ContentCap * scale) * .5f); }

// A screen is laid out by cutting boxes off a box, as the design boards stack
// their panes: the hint bar off the bottom, the header off the top, a side
// column off the right, and what is left is the body. Each cut takes `size`
// (clamped to what is there) and leaves the rest, gap included, in `from`.
struct Box {
    ImVec2 min, max;
    float W() const { return max.x - min.x; }
    float H() const { return max.y - min.y; }
};
inline Box CutTop(Box& from, float size, float gap = 0) { size = (std::min)(size, from.H()); Box cut{from.min, {from.max.x, from.min.y + size}}; from.min.y = (std::min)(from.max.y, cut.max.y + gap); return cut; }
inline Box CutBottom(Box& from, float size, float gap = 0) { size = (std::min)(size, from.H()); Box cut{{from.min.x, from.max.y - size}, from.max}; from.max.y = (std::max)(from.min.y, cut.min.y - gap); return cut; }
inline Box CutLeft(Box& from, float size, float gap = 0) { size = (std::min)(size, from.W()); Box cut{from.min, {from.min.x + size, from.max.y}}; from.min.x = (std::min)(from.max.x, cut.max.x + gap); return cut; }
inline Box CutRight(Box& from, float size, float gap = 0) { size = (std::min)(size, from.W()); Box cut{{from.max.x - size, from.min.y}, from.max}; from.max.x = (std::max)(from.min.x, cut.min.x - gap); return cut; }
inline Box Inset(Box b, float x, float y) { return {{b.min.x + x, b.min.y + y}, {b.max.x - x, b.max.y - y}}; }

// A board panel's ground (the room's, Chat's): the brand's panel in V2, else
// BoardPanelColor; lighter over a game the player watches.
ImVec4 BoardGround(bool seeThrough);
// The focused thing's marks, drawn over it: a 2 px Ember edge inside the box
// and a 4 px bar on its left (`bar` false for art, which the bar would cover).
// Its fill, Colors().header, goes under its content.
void Focus(ImDrawList* draw, ImVec2 min, ImVec2 max, bool bar = true);
// A card's ground: Frame at rest, Hover under the pointer, Header when focused
// (its marks then go over the content: Focus), and the 3 px Ember rule on a
// card that asks to be acted on.
void CardGround(ImDrawList* draw, Box card, bool focused, bool hovered, bool act = false);
// The hint bar along the bottom: each hint is a glyph and its words, and also
// a button (touch, mouse). `status` sits at the right end in `tone`. Returns
// the hint pressed, or -1.
struct Hint { const char* glyph; std::string label; };
int HintBar(Box bar, const std::vector<Hint>& hints, const std::string& status, ImU32 tone);
// A state word on its tone at 18 %: "Ready", "Live", "D 42 ms". Returns its width.
float Chip(ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 tone);
float ChipWidth(const std::string& text);
// A fact in a detail pane: `key` muted on the left, `value` on the right, on
// the frame's ground across the pane at the cursor. Takes the cursor's line.
void Fact(const std::string& key, const std::string& value);

// The brand's mark `height` tall at `at`: Ember's emblem, or BluMint's
// wordmark × Ember's emblem. Returns its width.
float BrandMark(ImDrawList* draw, ImVec2 at, float height);
// A screen's header: the brand's mark (BrandMark), an optional muted eyebrow (the path), the
// title, and the rule with its 72 px Ember start. Takes the cursor's line.
// `chips` sit at its right end: a state word each, in its tone.
struct HeaderChip { std::string text; ImU32 tone; };
void Header(const char* title, const char* eyebrow = nullptr, const std::vector<HeaderChip>& chips = {});
// Horizontal tabs under the header; the current one is filled and underlined.
// Returns the tab the pointer pressed, or -1. Moving between tabs is the screen's.
int Tabs(const std::vector<std::string>& labels, int current);

// A small 16:9 screen with the spots something can sit in (centres, 0 to 1 of
// the screen), the thing drawn as an Ember block of `block` (also 0 to 1) at
// `current`. The mouse drags the block, or clicks a spot, and it snaps to the
// nearest spot when let go: returns that spot, or -1 while nothing was picked.
// The pad and keys pick the same values in the row's value panel.
int SpotMap(const char* id, const std::vector<ImVec2>& spots, int current, ImVec2 block);

// The player tag: flag · rank · name · title · indicators, always in that
// order. Space short: status goes first, then the title; flag, rank, name and
// ping always stay.
enum class TagSize { XS, S, M };
struct TagView {
    std::string name, title, status;
    NameMarks marks;
    // The member's ping as the room measures it (LatencyMarkFor); none without text.
    LatencyMark ping;
};
// Draws at `at` within `width`; returns the height it took.
float Tag(ImDrawList* draw, ImVec2 at, float width, const TagView& tag, TagSize size);
} } }
