#pragma once
// Ember's look (the design canvas, lab/v2-mockups): the layout rules and the small
// set of parts every screen is built from. Presentation only: nothing here
// reads or changes navigation.
#include "NameMarks.hxx"
#include "RoomCards.hxx"
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace v2 {
// The palette roles of Theme.cxx by name, so the
// parts never write a literal: grounds from ink to hover, the accent that
// marks focus and the thing to act on, text, the red of a LIVE mark, the
// danger tone of Leave room (Kit: Error) and the green of Ready (Kit: Success).
struct Palette { ImU32 ink, frame, panel, button, header, hover, border, accent, accentLight, text, muted, live, danger, ready; };
const Palette& Colors();
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

// A board panel's ground (the room's, Chat's): the brand's panel, lighter over
// a game the player watches.
ImVec4 BoardGround(bool seeThrough);
// A panel's ground inside a board (Create's and Settings' panels, the room's
// members and chat): the brand's panel at .94, .78 over a game the player watches.
ImU32 PanelGround(bool seeThrough = false);
// A titled panel (Kit: card): its ground and edge, `title` (20 px) in a 40 px
// head with `trailing` muted at its right, and a rule under the head. `open`
// lights the head and the edge (Settings' open panel). Returns the body.
// A title that does not fit beside `trailing` drops it, and one that does not
// fit alone wraps (Heading), the head growing by its second line.
Box Panel(ImDrawList* draw, Box b, const char* title, const char* trailing = nullptr, bool open = false);
// A group's heading (Kit: eyebrow): 14 px in the heading font, muted unless
// `tone` says otherwise. Returns its width.
float SectionHeading(ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 tone = Colors().muted);
// `text` at `size` in `font` (the current one when none), fitted to `width`
// with Fit, so the text probe sees it; `centred` centres it in `width`.
// Returns the width drawn.
float Text(ImDrawList* draw, ImVec2 at, float width, const std::string& text, float size, ImU32 color, ImFont* font = nullptr, bool centred = false, bool probe = true);
// A heading at `size` in the heading font that wraps rather than cut (a
// Settings rail head, a panel's title): one line when it fits `width`, else
// as many whole words as the first line holds (one at least) and the rest on
// a second line HeadingLine apart, its box growing by that line. Each line
// goes through Text, so what still does not fit is cut and the probe reports
// it. HeadingHeight is its height in `width`; Heading draws it and returns it.
// In `font` when given (a row's label in the body font), else the heading font.
constexpr float HeadingLine = 1.2f;
float HeadingHeight(const std::string& text, float width, float size, ImFont* font = nullptr);
float Heading(ImDrawList* draw, ImVec2 at, float width, const std::string& text, float size, ImU32 color, ImFont* font = nullptr);
// A head's state word at its right, in the current font (a panel's Open, a
// rail head's Closed): it stands while the heading fits beside it on one line
// in `room` with `gap` between, and gives way before the heading wraps.
// Returns its width, or 0 when it gives way.
float HeadWord(const std::string& heading, float size, const std::string& word, float room, float gap);
// The chevron of a row whose Select opens something: `size` from its tip to
// each end, its tip pointing right at `at.x + size * .6`.
void Chevron(ImDrawList* draw, ImVec2 at, float size, ImU32 color);
// A board's button (Kit: buttons), 48 px tall by the design, its label in the
// 17 px heading font, fitted and centred. Primary is the screen's one Ember
// button: lighter under the pointer or focus, a 3 px ivory edge when focused.
// Secondary is the button ground with an Ember edge and Focus's marks; Danger
// the same in the danger tone (Leave room). Disabled ones are flat and muted.
// With no label the caller draws its own (Header's Back: a chevron and a word).
enum class ButtonKind { Primary, Secondary, Danger };
void Button(ImDrawList* draw, Box b, const std::string& label, ButtonKind kind, bool focused, bool hovered, bool enabled = true);
// A bar of buttons (the room's actions) in `width`: BarLines is the fewest
// lines over which the places, shared out evenly in order, each get the width
// their labels `need` with `gap` between; BarPlaces lays them out in `bar`,
// each line `height` tall and the next `gap` under it. Along a line the places
// share it evenly, a place that needs more taking that and the rest sharing
// what is left.
int BarLines(float width, const std::vector<float>& need, float gap);
std::vector<Box> BarPlaces(Box bar, const std::vector<float>& need, float gap, int lines, float height);
// The focused row's words under a board: its `label` over its `detail` on the
// frame's ground in `b`, the detail's first line, or all of it wrapped with
// `wrap`. WordsHeight is the height that shows `detail` wrapped in `width`.
void Words(ImDrawList* draw, Box b, const std::string& label, const std::string& detail, bool wrap);
float WordsHeight(float width, const std::string& detail);
// The focused thing's marks, drawn over it: a 2 px Ember edge inside the box
// and a 4 px bar on its left (`bar` false for art, which the bar would cover).
// Its fill, Colors().header, goes under its content.
void Focus(ImDrawList* draw, ImVec2 min, ImVec2 max, bool bar = true);
// A card's ground: Frame at rest, Hover under the pointer, Header when focused
// (its marks then go over the content: Focus), and the 3 px Ember rule on a
// card that asks to be acted on.
void CardGround(ImDrawList* draw, Box card, bool focused, bool hovered, bool act = false);
// A row's ground inside a panel (Create's, Settings', the room's members):
// nothing at rest, Hover under the pointer, Header when focused. Free-standing
// rows and cards are CardGround's.
void RowGround(ImDrawList* draw, Box row, bool focused, bool hovered);
// The hint bar along the bottom: each hint is a glyph and its words, and also
// a button (touch, mouse), unless it is only a label (`label`: a dialog that
// takes the pointer itself is open). `status` sits at the right end in `tone`;
// with `bell` it follows a bell and is a button too, pressed as hints.size().
// Returns the hint pressed, or -1.
struct Hint { const char* glyph; std::string label; bool labelOnly = false; };
int HintBar(Box bar, const std::vector<Hint>& hints, const std::string& status, ImU32 tone, bool bell = false);
// A state word on its tone at 18 %: "Ready", "Live", "D 42 ms". Returns its width.
float Chip(ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 tone);
float ChipWidth(const std::string& text);
float ChipFont();
// How a label and its value share a line (a value row, a card, a fact, a
// panel's title and its count): the label is never cut for its value. Beside
// the label the value takes the `value` it wants; where that leaves the label
// short it gives way down to `least` and takes what is left (cut where it is
// less than it wants); where even that does not fit it goes under the label,
// on a line of its own across `room`, and its row grows by UnderLine. In a
// row that cannot grow (`grows` false) it stays beside the label in what is left.
struct Share { float value = 0; bool under = false; };
Share ShareLine(float label, float value, float least, float room, float gap, bool grows = true);
// What a value under its label adds to its row, in logical pixels.
constexpr float UnderLine = 26;
// `text` cut with "..." to `width` at `size` (the current font when none). The
// text probe is told its full width unless `probe` is false (a line shown whole elsewhere).
std::string Fit(const std::string& text, float width, float size, ImFont* font = nullptr, bool probe = true);
// A fact in a detail pane: `key` muted on the left, `value` on the right, on
// the frame's ground across the pane at the cursor. Takes the cursor's line.
void Fact(const std::string& key, const std::string& value);
// Text wrapped across the cursor's line at `size` (its em) with the boards'
// 1.45 line height, which ImGui's own wrapping (one em) has no room for.
void Paragraph(const std::string& text, float size, ImU32 color, ImFont* font = ImGui::GetFont());
// The same drawn at `at` across `width`, for code without a cursor. Returns its height.
float Paragraph(ImDrawList* draw, ImVec2 at, float width, const std::string& text, float size, ImU32 color, ImFont* font = ImGui::GetFont());
// A Paragraph in the current font, size and colour, at the cursor.
void Wrapped(const std::string& text);

// A soft glow round a box (a seat lit up): rings fading out over `reach`, starting at `strength` alpha.
void Glow(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color, float strength, float reach, float rounding = 0);

// Ember's mark `height` tall at `at`. Returns its width.
float BrandMark(ImDrawList* draw, ImVec2 at, float height);
// A screen's header: the brand's mark (BrandMark), an optional muted eyebrow (the path), the
// title, and the rule with its 72 px Ember start under the title. Takes the cursor's line.
// `chips` sit at its right end: a state word each, in its tone. With `back`
// a button of that word leads the header; returns true when it was pressed.
struct HeaderChip { std::string text; ImU32 tone; };
bool Header(const char* title, const char* eyebrow = nullptr, const std::vector<HeaderChip>& chips = {}, const char* back = nullptr);
// Horizontal tabs under the header; the current one is filled and underlined,
// the device's tab buttons at the ends step them, wrapping (StepTab).
// Returns the tab the pointer pressed, or -1. Moving between tabs is the screen's.
int Tabs(const std::vector<std::string>& labels, int current);

// A small 16:9 screen with the spots something can sit in (centres, 0 to 1 of
// the screen), the thing drawn as an Ember block of `block` (also 0 to 1) at
// `current`. The mouse drags the block, or clicks a spot, and it snaps to the
// nearest spot when let go: returns that spot, or -1 while nothing was picked.
// The pad and keys pick the same values in the row's value panel.
int SpotMap(const char* id, const std::vector<ImVec2>& spots, int current, ImVec2 block);

// A layout grip (Customise.dc.html) on the edge from `from` to `to`, straight
// across or down: a line along the edge and a dot on its middle, which the mouse
// drags across the edge. Returns the pointer's position along that drag (x for
// an edge down the screen, y for one across) while held, else NAN. Not `live`,
// it is drawn muted and takes no pointer (the layout cannot change now). The
// pad and keys set the same layout in Settings > Interface > Room layout. Its
// box is reported (ReportMenuCard) under `id`.
float Grip(const char* id, ImVec2 from, ImVec2 to, bool live);

// A title's tier, from the lowest: its plate and stripe colours, and its icon's ink.
enum class Tier { None, Ash, Copper, Silver, Gold, Ember };
struct TierColors { ImU32 plate, stripe; };
TierColors TierColor(Tier tier);
// The title icons (the Titles & icons board), drawn as lines on a 64 grid
// scaled to `size`; -1 draws nothing.
constexpr int IconCount = 12;
void Icon(ImDrawList* draw, int icon, ImVec2 at, float size, ImU32 ink);
// The awards (the Awards board): a title on its tier's plate and the icon that
// goes with it, lowest tier first. The ids are the identity backend's; the
// rest are the catalog ids of its title, its icon's name and how to earn it.
struct Award { const char* id; int icon; Tier tier; const char *title, *name, *how; };
extern const Award Awards[11];
const Award* FindAward(const std::string& id);
const char* TierName(Tier tier);

// The player tag: flag · rank · name · title · indicators, always in that
// order. Space short: status goes first, then the title; flag, rank, name and
// ping always stay. L is the profile card: the main's portrait beside the
// tag, the title on its plate, a line about the player and their facts in a
// row, which takes two lines under 640 px.
enum class TagSize { XS, S, M, L };
struct TagView {
    std::string name, title, status;
    NameMarks marks;
    // The member's ping as the room measures it (LatencyMarkFor), with its route
    // above XS; none without text.
    LatencyMark ping;
    // L only: the main (-1 for none), the line under the title (country, class) and the facts.
    int fighter = -1;
    // The title's tier colours its plate; the icon (-1 none) sits on the card's right.
    Tier tier = Tier::None;
    int icon = -1;
    std::string about;
    struct Fact { std::string key, value; };
    std::vector<Fact> facts;
};
// Draws at `at` within `width`; returns the height it took.
float Tag(ImDrawList* draw, ImVec2 at, float width, const TagView& tag, TagSize size);
} } }
