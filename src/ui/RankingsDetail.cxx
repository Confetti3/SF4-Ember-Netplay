#include "RankingsDetail.hxx"
#include "ApplicationShell.hxx"
#include "NameMarks.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <variant>

namespace sf4e { namespace ui {
namespace {
using netplay::rankings::OwnRating;
using netplay::rankings::Standing;

// The panes Rankings draws, each with the answer it shows.
struct StandingPane {};
struct RatingPane { const OwnRating& rating; };
using Pane = std::variant<std::monostate, StandingPane, RatingPane>;

// The pane's measures, in logical pixels.
constexpr float Title = 22, Body = 15, Small = 13, Tile = 78, Rung = 34;

void Gap(float px) { ImGui::Dummy(ImVec2(0, px * Scale())); }

// A block of the pane: its heading in the accent, as the site heads its panels.
void Section(const std::string& text) {
    Gap(22);
    v2::Paragraph(text, 17 * Scale(), v2::Colors().accent, HeadingFont());
    Gap(6);
}

void Muted(const std::string& text, float size = Body) { v2::Paragraph(text, size * Scale(), v2::Colors().muted); }

std::string Whole(double value) { return std::to_string(static_cast<long long>(std::lround(value))); }

// A rating change with its sign; the hyphen, as the fonts have no minus sign.
std::string Signed(double change) {
    return (change >= 0 ? "+" : "-") + std::to_string(static_cast<long long>(std::lround(std::abs(change))));
}

// Reserves `height` across the pane under what was drawn at the cursor.
void Reserve(float height) { ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, height)); }

// The note that only you see these numbers, on the accent's wash as the site's lock note.
void PrivacyNote() {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, pad = 12 * s;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const std::string text = loc::T("rankings.rating_private");
    const float height = v2::Paragraph(nullptr, at, width - 2 * pad, text, 14 * s, 0) + 2 * pad;
    d->AddRectFilled(at, ImVec2(at.x + width, at.y + height), v2::color::Alpha(c.accent, .08f), 4 * s);
    d->AddRect(at, ImVec2(at.x + width, at.y + height), v2::color::Alpha(c.accent, .4f), 4 * s, 0, 1 * s);
    v2::Paragraph(d, ImVec2(at.x + pad, at.y + pad), width - 2 * pad, text, 14 * s, c.accentLight);
    Reserve(height);
}

// The three numbers side by side, each on a tile: its name over the number.
void NumberTiles(const OwnRating& r) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 10 * s, w = (width - 2 * gap) / 3, h = Tile * s;
    char volatility[32];
    std::snprintf(volatility, sizeof(volatility), "%.3f", r.volatility);
    const struct { const char* name; std::string value; } tiles[] = {
        {"rankings.rating", Whole(r.rating)}, {"rankings.deviation", Whole(r.deviation)}, {"rankings.volatility", volatility}};
    const ImVec2 at = ImGui::GetCursorScreenPos();
    for (int i = 0; i < 3; ++i) {
        const ImVec2 min(at.x + i * (w + gap), at.y), max(min.x + w, min.y + h);
        d->AddRectFilled(min, max, v2::PanelGround(), 4 * s);
        d->AddRect(min, max, c.border, 4 * s, 0, 1 * s);
        v2::Text(d, ImVec2(min.x + 14 * s, min.y + 12 * s), w - 28 * s, loc::T(tiles[i].name), Small * s, c.muted);
        v2::Text(d, ImVec2(min.x + 14 * s, min.y + 32 * s), w - 28 * s, tiles[i].value, 30 * s, c.text, HeadingFont());
    }
    Reserve(h);
}

// The classes from D to Grand Master as a ladder in their tones, as the site's
// chart labels them, your class lit and your rating a line through it, placed
// between where the class moves down and up.
void ClassLadder(const OwnRating& r) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, label = 22 * s, h = Rung * s;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y + label, w = width / SkillBadge::Classes;
    float x = at.x, markX = -1;
    for (std::uint8_t band = 1; band <= SkillBadge::Classes; ++band) {
        const ImVec2 min(x + 1 * s, y), max(x + w - 1 * s, y + h);
        const ImU32 tone = v2::ClassTone(band);
        const bool own = band == r.band;
        d->AddRectFilled(min, max, v2::color::Alpha(tone, own ? .28f : .09f), 2 * s);
        if (own) d->AddRect(min, max, tone, 2 * s, 0, 1 * s);
        v2::Text(d, ImVec2(min.x, y + (h - Small * s) * .5f), max.x - min.x, ClassShort(band), Small * s, own ? c.text : tone, HeadingFont(), true);
        if (own) {
            double part = .5;
            if (r.upAt && r.downBelow && *r.upAt > *r.downBelow) part = std::clamp((r.rating - *r.downBelow) / (*r.upAt - *r.downBelow), 0., 1.);
            else if (r.upAt) part = .25;
            else if (r.downBelow) part = .75;
            markX = min.x + static_cast<float>(part) * (max.x - min.x);
        }
        x += w;
    }
    // Your rating points down at the rung from over it, clear of the class's letters.
    if (markX >= 0) {
        d->AddTriangleFilled(ImVec2(markX - 5 * s, y - 7 * s), ImVec2(markX + 5 * s, y - 7 * s), ImVec2(markX, y - 1 * s), c.text);
        d->AddRectFilled(ImVec2(markX - 1 * s, y + h), ImVec2(markX + 1 * s, y + h + 5 * s), c.text);
        const std::string mine = loc::Tf("rankings.your_rating", Whole(r.rating));
        const float w = HeadingFont()->CalcTextSizeA(Small * s, FLT_MAX, 0, mine.c_str()).x + 2 * s;
        const float left = std::clamp(markX - w * .5f, at.x, at.x + width - w);
        v2::Text(d, ImVec2(left, at.y - 2 * s), w, mine, Small * s, c.text, HeadingFont());
    }
    const float under = y + h + 12 * s;
    if (r.downBelow && r.band > 1)
        v2::Text(d, ImVec2(at.x, under), width * .5f, loc::Tf("rankings.down_to", ClassName(static_cast<std::uint8_t>(r.band - 1)), Whole(*r.downBelow)), 14 * s, c.muted);
    if (r.upAt && r.band < SkillBadge::Classes)
        v2::TextEnd(d, ImVec2(at.x, under), width, loc::Tf("rankings.up_to", ClassName(static_cast<std::uint8_t>(r.band + 1)), Whole(*r.upAt)), 14 * s, c.muted);
    Reserve(label + h + 12 * s + 20 * s);
}

// What the next game would move against each kind of opponent: their class and
// rating, then the change a win and a loss make, as the site's table.
void NextGame(const OwnRating& r) {
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, column = 96 * s, row = 40 * s;
    const ImVec2 head = ImGui::GetCursorScreenPos();
    v2::Text(d, head, width - 2 * column, loc::T("rankings.opponent"), Small * s, c.muted);
    v2::TextEnd(d, head, width - column, loc::T("rankings.win"), Small * s, c.muted);
    v2::TextEnd(d, head, width, loc::T("rankings.loss"), Small * s, c.muted);
    float y = head.y + 24 * s;
    d->AddLine(ImVec2(head.x, y), ImVec2(head.x + width, y), c.border, 1 * s);
    const std::size_t count = (std::min)(r.examples.size(), std::size_t{4});
    for (std::size_t i = 0; i < count; ++i) {
        const auto& ex = r.examples[i];
        const float text = y + (row - Body * s) * .5f;
        float x = head.x;
        if (ex.band) {
            const float chip = v2::ChipFont() + 8 * s;
            x += v2::Chip(d, ImVec2(x, y + (row - chip) * .5f), ClassName(ex.band), v2::ClassTone(ex.band)) + 10 * s;
        }
        const std::string who = ex.band ? loc::Tf("rankings.rated", Whole(ex.opponent)) : loc::T("rankings.example_new");
        v2::Text(d, ImVec2(x, text), head.x + width - 2 * column - x, who, Body * s, c.text);
        v2::TextEnd(d, ImVec2(head.x, text), width - column, Signed(ex.win.change), 17 * s, c.ready);
        v2::TextEnd(d, ImVec2(head.x, text), width, Signed(ex.loss.change), 17 * s, c.danger);
        y += row;
        if (i + 1 < count) d->AddLine(ImVec2(head.x, y), ImVec2(head.x + width, y), v2::color::Alpha(c.border, .6f), 1 * s);
    }
    Reserve(y - head.y);
}

// What each number means: its name over the site's words for it.
void Abouts() {
    const struct { const char* name; const char* about; } abouts[] = {
        {"rankings.rating", "rankings.rating_about"},
        {"rankings.deviation", "rankings.deviation_about"},
        {"rankings.volatility", "rankings.volatility_about"},
    };
    for (const auto& a : abouts) {
        v2::Paragraph(loc::T(a.name), Body * Scale(), v2::Colors().text, HeadingFont());
        Muted(loc::T(a.about), 14);
        Gap(10);
    }
}

// Your class as the site's card shows it: the emblem large over its name and
// what it means. Without a class to show, just the words.
void Draw(const ShellView& v, const MenuEntry& e, StandingPane) {
    const float s = Scale(), emblem = 88 * s;
    const auto& st = v.rankings.standing;
    const NameMarks marks{CountryCode(), st.value.badge};
    if (st.loaded && st.value.kind == Standing::Kind::Ranked && st.value.classesShown && NameMarksWidth(marks, emblem) > 0) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        DrawNameMarks(ImGui::GetWindowDrawList(), at.x, at.y, emblem, marks);
        Reserve(emblem + 12 * s);
    }
    v2::Paragraph(e.label, Title * s, v2::Colors().text, HeadingFont());
    Muted(e.detail);
}

// Your rating as the site's Rating details page gives it: the three numbers,
// where your rating sits among the classes, what your next game would move,
// then what each number means.
void Draw(const ShellView&, const MenuEntry& e, RatingPane pane) {
    const auto& r = pane.rating;
    v2::Paragraph(loc::T("rankings.rating_title"), Title * Scale(), v2::Colors().text, HeadingFont());
    Gap(10);
    PrivacyNote();
    Gap(12);
    NumberTiles(r);
    if (r.band) {
        Section(loc::T("rankings.where_title"));
        ClassLadder(r);
        if (e.id == "rk-rating" && !e.detail.empty()) Muted(e.detail, 14);
    }
    if (!r.examples.empty()) {
        Section(loc::T("rankings.next_game"));
        NextGame(r);
    }
    Section(loc::T("rankings.numbers_title"));
    Abouts();
}

void Draw(const ShellView&, const MenuEntry&, std::monostate) {}

// The pane the focused row takes, its answer found and checked here once: your
// class shows your rating details once they are read, and none while the answer
// a pane shows is missing, so the menu's own pane stands in.
Pane PaneOf(const ShellView& v, const std::string& id) {
    const auto& rating = v.rankings.rating;
    const OwnRating* own = rating.loaded && rating.value.rating ? &*rating.value.rating : nullptr;
    if (id == "rk-rating" || id == "rk-standing") {
        if (own) return RatingPane{*own};
        if (id == "rk-standing") return StandingPane{};
    }
    return {};
}
}

bool RankingsDrawsDetail(const ShellView& v, const std::string& id) {
    return !std::holds_alternative<std::monostate>(PaneOf(v, id));
}

void DrawRankingsDetail(const ShellView& v, const MenuEntry& e) {
    std::visit([&](const auto& pane) { Draw(v, e, pane); }, PaneOf(v, e.id));
}
} }
