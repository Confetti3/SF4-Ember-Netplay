#include "RankingsBoard.hxx"
#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "NameMarks.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
namespace {
using netplay::rankings::BoardEntry;
using netplay::rankings::Listing;
using netplay::rankings::PointsBoard;
using netplay::rankings::PointsLine;

// The boards' measures, in logical pixels.
constexpr float Gap = 8, Strip = 64, Rail = 300, Aside = 28, RailTitle = 40, ClassRungHeight = 54, FighterRungHeight = 52,
    Plate = 112, Head = 96, Place = 72, TableHead = 36, Line = 46, PlayerLine = 52, Side = 400;

bool Focused(const RankingsBoardFrame& f, const MenuEntry& e) { return e.id == f.menu.navigation.Focus(); }

// A row the boards have no look of their own for (loading, a failure, none
// listed): the menu's own card, its bottom returned.
float StateRow(const RankingsBoardFrame& f, const MenuEntry& e, ImVec2 at, float width) {
    return f.menu.DrawBoardRow(f.rows, e, at, width, f.action, BoardRowStyle{{}, true, f.changed, true, true}).box.max.y + Gap * Scale();
}

// Your own strip and banner: the accent's wash inside its edge, as the site marks you.
void Accented(ImDrawList* d, v2::Box b) {
    const auto& c = v2::Colors();
    const float s = Scale();
    d->AddRectFilled(b.min, b.max, v2::color::Alpha(c.accent, .10f), 4 * s);
    d->AddRect(b.min, b.max, v2::color::Alpha(c.accent, .45f), 4 * s, 0, 1 * s);
}

// The ground of a page's panel: the table, the list of a class's players, the fighter's head.
void PanelBox(ImDrawList* d, v2::Box b) {
    const float s = Scale();
    d->AddRectFilled(b.min, b.max, v2::PanelGround(), 4 * s);
    d->AddRect(b.min, b.max, v2::Colors().border, 4 * s, 0, 1 * s);
}

// A class's chip, its middle on `middle`. Returns its width.
float ClassChip(ImDrawList* d, float x, float middle, std::uint8_t band) {
    return v2::Chip(d, ImVec2(x, middle - (v2::ChipFont() + 8 * Scale()) * .5f), ClassName(band), v2::ClassTone(band));
}

// The You chip ending at `right`, its middle on `middle`. Returns where it starts.
float YouChip(ImDrawList* d, float right, float middle) {
    const std::string you = loc::T("rankings.you");
    const float x = right - v2::ChipWidth(you);
    v2::Chip(d, ImVec2(x, middle - (v2::ChipFont() + 8 * Scale()) * .5f), you, v2::Colors().accent);
    return x;
}

// A frame's colour and name, `size` text with its middle on `middle`.
void FrameMark(ImDrawList* d, float x, float middle, float room, int tier, float size) {
    const float s = Scale();
    const v2::Tier t = v2::FrameTier(tier);
    d->AddRectFilled(ImVec2(x, middle - 5 * s), ImVec2(x + 10 * s, middle + 5 * s), v2::TierColor(t).stripe, 2 * s);
    v2::Text(d, ImVec2(x + 16 * s, middle - size * .5f), room - 16 * s, v2::TierName(t), size, v2::Colors().muted);
}

// A player's flag and name, `size` text with its middle on `middle`.
void Name(ImDrawList* d, float x, float middle, float room, const Listing& listing, float size) {
    const float top = middle - size * .5f, flag = DrawNameMarks(d, x, top, size, NameMarks{listing.country, {}}).width;
    v2::Text(d, ImVec2(x + flag, top), room - flag, listing.name, size, v2::Colors().text);
}

std::string PlayersWords(int count) { return loc::Tf(loc::SingularCount(count) ? "rankings.players_one" : "rankings.players_count", count); }

// --- The side list and its page ---

// The side list and the page beside it, from `at` to the body's bottom.
struct Split { v2::Box rail, page; };

Split SplitOf(ImVec2 at, float width, float bottom) {
    const float s = Scale(), rail = (std::min)(Rail * s, width * .32f);
    return {{at, ImVec2(at.x + rail, bottom)}, {ImVec2(at.x + rail + Aside * s, at.y), ImVec2(at.x + width, bottom)}};
}

// A column with a scroll of its own, so a long list moves while the other
// column stays. `draw(at, width)` draws its content inset from the column's
// edges, where the focus's outline still shows, and returns its bottom.
template <class Draw> void Column(const char* id, v2::Box b, Draw&& draw) {
    const float pad = 4 * Scale();
    ImGui::SetCursorScreenPos(b.min);
    ImGui::BeginChild(id, ImVec2(b.W(), b.H()), 0, ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoBackground);
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float bottom = draw(ImVec2(top.x + pad, top.y + pad), ImGui::GetContentRegionAvail().x - 2 * pad);
    ImGui::SetCursorScreenPos(top);
    ImGui::Dummy(ImVec2(1, bottom - top.y + pad));
    ImGui::EndChild();
}

// The side list's name over it, as a section of the site's.
void RailHeading(ImDrawList* d, v2::Box rail, const std::string& title) {
    const auto& c = v2::Colors();
    const float s = Scale(), bottom = rail.min.y + RailTitle * s;
    v2::Text(d, ImVec2(rail.min.x + 4 * s, rail.min.y + 8 * s), rail.W(), title, 15 * s, c.muted, HeadingFont());
    d->AddLine(ImVec2(rail.min.x + 4 * s, bottom - 8 * s), ImVec2(rail.max.x - 4 * s, bottom - 8 * s), c.border, 1 * s);
}

// One entry of the side list: a card with an edge in `tone`, lit with a
// pointer at the page while its page shows. Returns the room inside for the
// entry's own marks.
v2::Box RailRow(const RankingsBoardFrame& f, const MenuEntry& e, v2::Box box, ImU32 tone, bool shown) {
    auto* d = ImGui::GetWindowDrawList();
    const float s = Scale(), middle = box.min.y + box.H() * .5f;
    const bool focused = Focused(f, e);
    const auto hit = PressEntry(f.menu.navigation, f.rows, e, box, f.action, f.changed);
    v2::CardGround(d, box, focused, hit.hovered);
    if (shown) {
        d->AddRectFilled(box.min, box.max, v2::color::Alpha(tone, .14f), 4 * s);
        d->AddTriangleFilled(ImVec2(box.max.x, middle - 7 * s), ImVec2(box.max.x + 7 * s, middle), ImVec2(box.max.x, middle + 7 * s), tone);
    }
    d->AddRectFilled(box.min, ImVec2(box.min.x + (shown ? 6 : 3) * s, box.max.y), e.enabled ? tone : v2::color::Alpha(tone, .35f), 4 * s, ImDrawFlags_RoundCornersLeft);
    if (focused) v2::Focus(d, box.min, box.max);
    return {ImVec2(box.min.x + 20 * s, box.min.y), ImVec2(box.max.x - 14 * s, box.max.y)};
}

// --- The class board ---

// A class in words: Class B+, Master, Grand Master.
std::string ClassTitle(std::uint8_t band) {
    return band + 1 >= SkillBadge::Classes ? ClassName(band) : loc::Tf("class.tip", ClassName(band));
}

// You: the word, your class's chip and why the board lists you or not. It
// stands over both columns and is no stop for the pad.
float YouStrip(ImDrawList* d, const MenuEntry& e, ImVec2 at, float width, std::uint8_t ownBand) {
    const auto& c = v2::Colors();
    const float s = Scale(), middle = at.y + Strip * s * .5f, right = at.x + width - 16 * s;
    Accented(d, v2::Box{at, ImVec2(at.x + width, at.y + Strip * s)});
    float x = at.x + 16 * s;
    x += v2::Text(d, ImVec2(x, middle - 15 * s * .5f), right - x, e.label, 15 * s, c.accent, HeadingFont()) + 16 * s;
    if (ownBand) x += ClassChip(d, x, middle, ownBand) + 12 * s;
    if (x < right) v2::Text(d, ImVec2(x, middle - 15 * s * .5f), right - x, e.detail.substr(0, e.detail.find('\n')), 15 * s, c.muted);
    return at.y + (Strip + 2 * Gap) * s;
}

// A class on the side: its letters in its tone, how many it has against the
// fullest class as the site's chart shows them, and your mark on your own.
float ClassRung(const RankingsBoardFrame& f, const MenuEntry& e, ImVec2 at, float width, std::uint8_t band, const ClassCounts& counts,
    std::uint8_t ownBand, std::uint8_t shown) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale();
    const ImU32 tone = v2::ClassTone(band);
    const int count = counts.of[band];
    const v2::Box box{at, ImVec2(at.x + width, at.y + ClassRungHeight * s)};
    const v2::Box in = RailRow(f, e, box, tone, band == shown);
    const float middle = in.min.y + in.H() * .5f;
    v2::Text(d, ImVec2(in.min.x, middle - 11 * s), 52 * s, ClassShort(band), 22 * s, count ? tone : v2::color::Alpha(tone, .45f), HeadingFont());
    float right = in.max.x - v2::TextEnd(d, ImVec2(in.min.x, middle - 8 * s), in.W(), std::to_string(count), 16 * s, count ? c.text : c.muted) - 12 * s;
    if (band == ownBand) right = YouChip(d, right, middle) - 12 * s;
    const float bar = in.min.x + 60 * s;
    if (count && right > bar) {
        d->AddRectFilled(ImVec2(bar, middle - 2 * s), ImVec2(right, middle + 2 * s), v2::color::Alpha(c.border, .6f), 2 * s);
        d->AddRectFilled(ImVec2(bar, middle - 2 * s), ImVec2(bar + (right - bar) * count / counts.most, middle + 2 * s), tone, 2 * s);
    }
    return box.max.y + 6 * s;
}

// The class on show over its players: its letters large in its tone, its
// name, how many there are, and whether it is yours.
float ClassPlate(ImDrawList* d, ImVec2 at, float width, std::uint8_t band, int count, bool own) {
    const auto& c = v2::Colors();
    const float s = Scale(), h = Plate * s;
    const ImU32 tone = v2::ClassTone(band);
    const v2::Box box{at, ImVec2(at.x + width, at.y + h)};
    d->AddRectFilled(box.min, box.max, v2::color::Alpha(tone, .08f), 4 * s);
    d->AddRect(box.min, box.max, v2::color::Alpha(tone, .35f), 4 * s, 0, 1 * s);
    const v2::Box mark{ImVec2(at.x + 14 * s, at.y + 14 * s), ImVec2(at.x + h, at.y + h - 14 * s)};
    d->AddRectFilled(mark.min, mark.max, v2::color::Alpha(tone, .20f), 4 * s);
    v2::Text(d, ImVec2(mark.min.x, mark.min.y + (mark.H() - 36 * s) * .5f), mark.W(), ClassShort(band), 36 * s, tone, HeadingFont(), true);
    const float x = mark.max.x + 22 * s, room = box.max.x - 18 * s - x;
    v2::Text(d, ImVec2(x, at.y + 24 * s), room, ClassTitle(band), 28 * s, c.text, HeadingFont());
    const float words = v2::Text(d, ImVec2(x, at.y + 66 * s), room, PlayersWords(count), 16 * s, c.muted);
    if (own) v2::Chip(d, ImVec2(x + words + 14 * s, at.y + 74 * s - (v2::ChipFont() + 8 * s) * .5f), loc::T("rankings.your_class"), c.accent);
    return box.max.y + 2 * Gap * s;
}

// A player of the class on show with the row that focuses and presses it.
struct ClassPlayer {
    const MenuEntry& row;
    const BoardEntry& entry;
};

// A player of the class: their flag and name, their country at the end, yours
// washed in the accent with the You chip.
void PlayerRow(const RankingsBoardFrame& f, const ClassPlayer& p, v2::Box box, bool rule) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), middle = box.min.y + box.H() * .5f, left = box.min.x + 18 * s;
    const auto hit = PressEntry(f.menu.navigation, f.rows, p.row, box, f.action, f.changed);
    // A class of hundreds paints only the players in view.
    if (!ImGui::IsRectVisible(box.min, box.max)) return;
    if (p.entry.own) {
        d->AddRectFilled(box.min, box.max, v2::color::Alpha(c.accent, .08f), 4 * s);
        d->AddRectFilled(box.min, ImVec2(box.min.x + 3 * s, box.max.y), c.accent);
    }
    v2::RowGround(d, box, Focused(f, p.row), hit.hovered);
    float right = box.max.x - 18 * s;
    if (p.entry.own) right = YouChip(d, right, middle) - 16 * s;
    if (p.entry.listing.country.Present())
        right -= v2::TextEnd(d, ImVec2(left, middle - 15 * s * .5f), right - left, CountryName(p.entry.listing.country), 15 * s, c.muted) + 24 * s;
    Name(d, left, middle, right - left, p.entry.listing, 18 * s);
    if (rule) d->AddLine(ImVec2(box.min.x + 12 * s, box.max.y), ImVec2(box.max.x - 12 * s, box.max.y), v2::color::Alpha(c.border, .6f), 1 * s);
    if (Focused(f, p.row)) v2::Focus(d, box.min, box.max);
}

float PlayerList(const RankingsBoardFrame& f, const std::vector<ClassPlayer>& players, ImVec2 at, float width) {
    const float s = Scale(), line = PlayerLine * s;
    const v2::Box panel{at, ImVec2(at.x + width, at.y + players.size() * line + Gap * s)};
    PanelBox(ImGui::GetWindowDrawList(), panel);
    float y = at.y + Gap * s * .5f;
    for (std::size_t i = 0; i < players.size(); ++i, y += line)
        PlayerRow(f, players[i], v2::Box{ImVec2(at.x + 4 * s, y), ImVec2(at.x + width - 4 * s, y + line)}, i + 1 < players.size());
    return panel.max.y + Gap * s;
}

// --- The points board ---

// A fighter on the side: their face (Ember's mark for every fighter) and name.
float FighterRung(const RankingsBoardFrame& f, const MenuEntry& e, ImVec2 at, float width, int fighter, bool shown) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), face = 38 * s;
    const v2::Box box{at, ImVec2(at.x + width, at.y + FighterRungHeight * s)};
    const v2::Box in = RailRow(f, e, box, c.accent, shown);
    // A roster of forty paints only the fighters in view.
    if (ImGui::IsRectVisible(box.min, box.max)) {
        const float middle = in.min.y + in.H() * .5f;
        const ImVec2 pic(in.min.x - 6 * s, middle - face * .5f);
        if (fighter >= 0) {
            DrawCharacterPortrait(fighter, pic, ImVec2(pic.x + face, pic.y + face));
            d->AddRect(pic, ImVec2(pic.x + face, pic.y + face), c.border, 3 * s, 0, 1 * s);
        } else v2::BrandMark(d, ImVec2(pic.x + 6 * s, pic.y + 6 * s), face - 12 * s);
        const float x = pic.x + face + 14 * s;
        v2::Text(d, ImVec2(x, middle - 17 * s * .5f), in.max.x - x, e.label, 17 * s, shown ? c.text : c.muted, HeadingFont());
    }
    return box.max.y + 4 * s;
}

// The page's head: the fighter's face, their name over how many players have points.
float PointsHead(ImDrawList* d, ImVec2 at, float width, const std::string& name, const PointsBoard* board, int fighter) {
    const auto& c = v2::Colors();
    const float s = Scale(), face = 64 * s;
    const v2::Box box{at, ImVec2(at.x + width, at.y + Head * s)};
    PanelBox(d, box);
    const ImVec2 pic(at.x + 16 * s, at.y + (box.H() - face) * .5f);
    if (fighter >= 0) {
        DrawCharacterPortrait(fighter, pic, ImVec2(pic.x + face, pic.y + face));
        d->AddRect(pic, ImVec2(pic.x + face, pic.y + face), c.border, 3 * s, 0, 1 * s);
    } else v2::BrandMark(d, ImVec2(pic.x + 8 * s, pic.y + 8 * s), face - 16 * s);
    const float x = pic.x + face + 18 * s, room = box.max.x - 18 * s - x;
    v2::Text(d, ImVec2(x, at.y + 18 * s), room, name, 28 * s, c.text, HeadingFont());
    const std::string about = !board ? loc::T("rankings.points_detail")
        : fighter < 0 ? loc::Tf("rankings.points_bp_total", board->total) : loc::Tf("rankings.points_fp_total", board->total, name);
    v2::Text(d, ImVec2(x, at.y + 56 * s), room, about, 15 * s, c.muted);
    return box.max.y + 2 * Gap * s;
}

// Your line of the board lifted out over it: your place of how many, your
// name, your frame and your points.
float OwnPlace(ImDrawList* d, ImVec2 at, float width, const PointsLine& line, std::uint64_t total, const char* label) {
    const auto& c = v2::Colors();
    const float s = Scale(), middle = at.y + Place * s * .5f, right = at.x + width - 18 * s;
    Accented(d, v2::Box{at, ImVec2(at.x + width, at.y + Place * s)});
    float x = at.x + 18 * s;
    x += v2::Text(d, ImVec2(x, middle - 15 * s * .5f), right - x, loc::T("rankings.your_place"), 15 * s, c.accent, HeadingFont()) + 20 * s;
    x += v2::Text(d, ImVec2(x, middle - 26 * s * .5f), right - x, loc::Tf("rankings.place_of", line.place, total), 26 * s, c.text, HeadingFont()) + 24 * s;
    const float level = right - v2::LevelWidth(line.points, 30 * s, label), frame = level - 24 * s - 110 * s;
    v2::Level(d, ImVec2(level, middle - 15 * s), 30 * s, line.points, label);
    FrameMark(d, frame, middle, 110 * s, line.tier, 15 * s);
    Name(d, x, middle, frame - 12 * s - x, line.listing, 17 * s);
    return at.y + (Place + 16) * s;
}

// The table's columns, those that fit `width`: the class and the frame give way first.
struct Columns {
    float place, player, playerEnd, cls, frame, pointsEnd;
    bool showClass, showFrame;
};

Columns ColumnsOf(float x, float width) {
    const float s = Scale(), pad = 16 * s;
    Columns k{};
    k.place = x + pad; k.player = k.place + 64 * s; k.pointsEnd = x + width - pad;
    k.showFrame = width >= 560 * s; k.showClass = width >= 760 * s;
    k.frame = k.pointsEnd - 150 * s - (k.showFrame ? 120 * s : 0);
    k.cls = k.frame - (k.showClass ? 110 * s : 0);
    k.playerEnd = k.cls - 12 * s;
    return k;
}

// A line of the board with the row that focuses and presses it.
struct TableRow {
    const MenuEntry& row;
    const PointsLine& line;
};

// A line of the table: the place (the top three in the accent), the player,
// their class and frame, and their points as the game draws BP and FP.
void TableLine(const RankingsBoardFrame& f, const TableRow& t, v2::Box box, const Columns& k, bool own, bool rule, const char* label) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), middle = box.min.y + box.H() * .5f;
    const PointsLine& line = t.line;
    const auto hit = PressEntry(f.menu.navigation, f.rows, t.row, box, f.action, f.changed);
    if (!ImGui::IsRectVisible(box.min, box.max)) return;
    if (own) d->AddRectFilled(box.min, box.max, v2::color::Alpha(c.accent, .08f), 4 * s);
    v2::RowGround(d, box, Focused(f, t.row), hit.hovered);
    v2::TextEnd(d, ImVec2(k.place, middle - 17 * s * .5f), 48 * s, std::to_string(line.place), 17 * s, line.place <= 3 ? c.accent : c.muted);
    Name(d, k.player, middle, k.playerEnd - k.player, line.listing, 17 * s);
    if (k.showClass && line.band) ClassChip(d, k.cls, middle, line.band);
    if (k.showFrame) FrameMark(d, k.frame, middle, 120 * s, line.tier, 15 * s);
    v2::Level(d, ImVec2(k.pointsEnd - v2::LevelWidth(line.points, 20 * s, label), middle - 10 * s), 20 * s, line.points, label);
    if (rule) d->AddLine(ImVec2(box.min.x, box.max.y), ImVec2(box.max.x, box.max.y), v2::color::Alpha(c.border, .6f), 1 * s);
    if (Focused(f, t.row)) v2::Focus(d, box.min, box.max);
}

// The board's lines in one panel under the columns' names.
float PointsTable(const RankingsBoardFrame& f, const std::vector<TableRow>& lines, const Listing* listed, ImVec2 at, float width, const char* label) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), head = TableHead * s, line = Line * s;
    const v2::Box panel{at, ImVec2(at.x + width, at.y + head + lines.size() * line + Gap * s)};
    PanelBox(d, panel);
    const Columns k = ColumnsOf(at.x, width);
    const float words = at.y + (head - 13 * s) * .5f;
    v2::TextEnd(d, ImVec2(k.place, words), 48 * s, loc::T("rankings.place"), 13 * s, c.muted);
    v2::Text(d, ImVec2(k.player, words), k.playerEnd - k.player, loc::T("rankings.player"), 13 * s, c.muted);
    if (k.showClass) v2::Text(d, ImVec2(k.cls, words), 110 * s, loc::T("rankings.class"), 13 * s, c.muted);
    if (k.showFrame) v2::Text(d, ImVec2(k.frame, words), 120 * s, loc::T("rankings.frame"), 13 * s, c.muted);
    v2::TextEnd(d, ImVec2(k.pointsEnd - 150 * s, words), 150 * s, loc::T("rankings.points_column"), 13 * s, c.muted);
    d->AddLine(ImVec2(k.place, at.y + head), ImVec2(k.pointsEnd, at.y + head), c.border, 1 * s);
    float y = at.y + head;
    for (std::size_t i = 0; i < lines.size(); ++i, y += line)
        TableLine(f, lines[i], v2::Box{ImVec2(at.x + 4 * s, y), ImVec2(at.x + width - 4 * s, y + line)}, k, listed && lines[i].line.listing == *listed, i + 1 < lines.size(), label);
    return panel.max.y + 2 * Gap * s;
}

// Beside the board: how points are earned, then the frames in their colours and what they mean.
float HowPanels(ImDrawList* d, ImVec2 at, float width) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = 16 * s, room = width - 2 * pad, title = 56 * s, rung = 26 * s;
    const std::string how = loc::T("fighters.how_detail"), frames = loc::T("rankings.frames_about");
    const float howHeight = title + v2::Paragraph(nullptr, ImVec2(), room, how, 15 * s, 0) + 20 * s;
    const v2::Box first = v2::Panel(d, v2::Box{at, ImVec2(at.x + width, at.y + howHeight)}, loc::T("fighters.how"));
    v2::Paragraph(d, ImVec2(first.min.x + pad, first.min.y + pad), room, how, 15 * s, c.muted);
    const float top = at.y + howHeight + pad;
    const float framesHeight = title + 5 * rung + Gap * s + v2::Paragraph(nullptr, ImVec2(), room, frames, 14 * s, 0) + 20 * s;
    const v2::Box second = v2::Panel(d, v2::Box{ImVec2(at.x, top), ImVec2(at.x + width, top + framesHeight)}, loc::T("rankings.frames"));
    float y = second.min.y + pad;
    for (int tier = 0; tier < 5; ++tier, y += rung) FrameMark(d, second.min.x + pad, y + rung * .5f, room, tier, 16 * s);
    v2::Paragraph(d, ImVec2(second.min.x + pad, y + Gap * s), room, frames, 14 * s, c.muted);
    return top + framesHeight;
}

// The body's bottom, and room for a page however short the window.
float BodyBottom(float y) { return (std::max)(y + 240 * Scale(), ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y); }
}

std::string ClassRow(std::uint8_t band) { return "rkb-c:" + std::to_string(band); }
std::string FighterRow(int fighter) { return fighter < 0 ? "rkp-all" : "rkp-f:" + std::to_string(fighter); }
std::string BoardPlayerRow(std::size_t at) { return "rkb-p:" + std::to_string(at); }
std::string PointsPlayerRow(std::size_t at) { return "rkp-p:" + std::to_string(at); }
std::optional<std::uint8_t> ClassOfRow(std::string_view id) {
    const auto band = RowKey<std::uint8_t>(id, "rkb-c:");
    return band && *band >= 1 && *band <= SkillBadge::Classes ? band : std::nullopt;
}
std::optional<int> FighterOfRow(std::string_view id) {
    if (id == "rkp-all") return -1;
    return RowKey<int>(id, "rkp-f:");
}
std::optional<std::size_t> BoardPlayerOfRow(std::string_view id) { return RowKey<std::size_t>(id, "rkb-p:"); }
std::optional<std::size_t> PointsPlayerOfRow(std::string_view id) { return RowKey<std::size_t>(id, "rkp-p:"); }

ClassCounts CountClasses(const std::vector<BoardEntry>& board) {
    ClassCounts counts;
    for (const auto& entry : board)
        if (entry.band >= 1 && entry.band <= SkillBadge::Classes) counts.most = (std::max)(counts.most, ++counts.of[entry.band]);
    return counts;
}

void DrawClassBoard(const RankingsBoardFrame& f, const std::vector<BoardEntry>* board, std::uint8_t ownBand, std::uint8_t shown) {
    auto* d = ImGui::GetWindowDrawList();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float bottom = BodyBottom(top.y);
    float y = top.y;
    std::vector<std::pair<const MenuEntry*, std::uint8_t>> rungs;
    std::vector<ClassPlayer> players;
    for (const MenuEntry& e : f.rows) {
        const auto band = ClassOfRow(e.id);
        const auto at = BoardPlayerOfRow(e.id);
        if (e.id == "rkb-you") y = YouStrip(d, e, ImVec2(top.x, y), width, ownBand);
        else if (band) rungs.emplace_back(&e, *band);
        else if (board && at && *at < board->size()) players.push_back({e, (*board)[*at]});
        else y = StateRow(f, e, ImVec2(top.x, y), width);
    }
    if (board && !rungs.empty()) {
        const ClassCounts counts = CountClasses(*board);
        const Split split = SplitOf(ImVec2(top.x, y), width, bottom);
        RailHeading(d, split.rail, loc::T("rankings.classes"));
        Column("rkb-classes", v2::Box{ImVec2(split.rail.min.x, split.rail.min.y + RailTitle * s), split.rail.max}, [&](ImVec2 at, float w) {
            for (const auto& rung : rungs) at.y = ClassRung(f, *rung.first, at, w - 8 * s, rung.second, counts, ownBand, shown);
            return at.y;
        });
        Column("rkb-players", split.page, [&](ImVec2 at, float w) {
            const float listed = ClassPlate(ImGui::GetWindowDrawList(), at, w, shown, counts.of[(std::min)(shown, SkillBadge::Classes)], shown == ownBand);
            return PlayerList(f, players, ImVec2(at.x, listed), w);
        });
        y = bottom;
    }
    ImGui::SetCursorScreenPos(top);
    ImGui::Dummy(ImVec2(width, y - top.y));
}

void DrawPointsBoard(const RankingsBoardFrame& f, const PointsBoard* board, int fighter, const Listing* listed) {
    auto* d = ImGui::GetWindowDrawList();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const char* label = fighter < 0 ? "points.account_label" : "points.label";
    const std::string current = FighterRow(fighter);
    std::vector<std::pair<const MenuEntry*, int>> rungs;
    std::vector<const MenuEntry*> states;
    std::vector<TableRow> lines;
    std::string name;
    for (const MenuEntry& e : f.rows) {
        const auto rung = FighterOfRow(e.id);
        const auto at = PointsPlayerOfRow(e.id);
        if (e.id == current) name = e.label;
        if (rung) rungs.emplace_back(&e, *rung);
        else if (board && at && *at < board->lines.size()) lines.push_back({e, board->lines[*at]});
        else states.push_back(&e);
    }
    const Split split = SplitOf(top, width, BodyBottom(top.y));
    RailHeading(d, split.rail, loc::T("rankings.fighters"));
    Column("rkp-fighters", v2::Box{ImVec2(split.rail.min.x, split.rail.min.y + RailTitle * s), split.rail.max}, [&](ImVec2 at, float w) {
        for (const auto& rung : rungs) at.y = FighterRung(f, *rung.first, at, w - 8 * s, rung.second, rung.first->id == current);
        return at.y;
    });
    Column("rkp-board", split.page, [&](ImVec2 at, float w) {
        auto* page = ImGui::GetWindowDrawList();
        // Wide enough, how points are earned stands beside the board, as on the site.
        const bool side = w >= 1100 * s;
        const float left = side ? w - (Side + 24) * s : w;
        const float sideBottom = side ? HowPanels(page, ImVec2(at.x + left + 24 * s, at.y), Side * s) : at.y;
        float y = PointsHead(page, at, left, name, board, fighter);
        for (const MenuEntry* e : states) y = StateRow(f, *e, ImVec2(at.x, y), left);
        if (board && !lines.empty()) {
            const auto mine = listed ? std::find_if(board->lines.begin(), board->lines.end(), [&](const PointsLine& line) { return line.listing == *listed; })
                                     : board->lines.end();
            if (mine != board->lines.end()) y = OwnPlace(page, ImVec2(at.x, y), left, *mine, board->total, label);
            y = PointsTable(f, lines, listed, ImVec2(at.x, y), left, label);
        }
        return (std::max)(y, sideBottom);
    });
    ImGui::SetCursorScreenPos(top);
    ImGui::Dummy(ImVec2(width, split.rail.H()));
}
} }
