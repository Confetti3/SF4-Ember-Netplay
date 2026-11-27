#include "RankingsDetail.hxx"
#include "ApplicationShell.hxx"
#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "NameMarks.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

namespace sf4e { namespace ui {
namespace {
using netplay::rankings::Standing;

// The panes Rankings draws, each with the answer it shows.
struct StandingPane {};
struct RatingPane { const netplay::rankings::OwnRating& rating; };
struct PointsBoardPane {};
struct PointsPlayerPane { const netplay::rankings::PointsBoard& board; const netplay::rankings::PointsLine& line; };
struct ClassPlayerPane { const netplay::rankings::BoardEntry& entry; };
using Pane = std::variant<std::monostate, StandingPane, RatingPane, PointsBoardPane, PointsPlayerPane, ClassPlayerPane>;

void Gap(float px) {
    ImGui::Dummy(ImVec2(0, px * Scale()));
}

void Title(const std::string& text) {
    const float s = Scale();
    const auto& c = v2::Colors();
    v2::Paragraph(text, 22 * s, c.text, HeadingFont());
}

void Section(const std::string& text) {
    const float s = Scale();
    const auto& c = v2::Colors();
    Gap(16);
    v2::Paragraph(text, 15 * s, c.accent, HeadingFont());
    Gap(4);
}

void Muted(const std::string& text) {
    const float s = Scale();
    const auto& c = v2::Colors();
    v2::Paragraph(text, 15 * s, c.muted);
}

std::string Whole(double value) {
    return std::to_string(static_cast<long long>(std::lround(value)));
}

// A rating change with its sign; the hyphen, as the fonts have no minus sign.
std::string Signed(double change) {
    const long long n = static_cast<long long>(std::lround(std::abs(change)));
    return (change >= 0 ? "+" : "-") + std::to_string(n);
}

void FactsAcross(std::vector<v2::TagView::Fact> facts, std::size_t across) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = v2::FactCells(ImGui::GetWindowDrawList(), at, w, facts, across);
    ImGui::Dummy(ImVec2(w, h));
}

void PlayerTag(const netplay::rankings::Listing& listing, std::uint8_t band) {
    const float s = Scale();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    auto* d = ImGui::GetWindowDrawList();
    v2::TagView tag;
    tag.name = listing.name;
    tag.marks = {listing.country, band ? SkillBadge::OfClass(band, true) : SkillBadge()};
    const float h = v2::Tag(d, cursor, width, tag, v2::TagSize::M);
    ImGui::Dummy(ImVec2(width, h + 10 * s));
}

void FramesSection() {
    const float s = Scale();
    const auto& c = v2::Colors();
    const float width = ImGui::GetContentRegionAvail().x;
    auto* d = ImGui::GetWindowDrawList();
    Section(loc::T("rankings.frames"));
    for (int i = 0; i < 5; ++i) {
        const auto t = v2::FrameTier(i);
        const ImVec2 row = ImGui::GetCursorScreenPos();
        d->AddRectFilled(ImVec2(row.x, row.y + 6 * s), ImVec2(row.x + 10 * s, row.y + 16 * s), v2::TierColor(t).stripe, 2 * s);
        v2::Text(d, ImVec2(row.x + 20 * s, row.y), width - 20 * s, v2::TierName(t), 15 * s, c.text);
        ImGui::Dummy(ImVec2(width, 24 * s));
    }
    Gap(4);
    Muted(loc::T("rankings.frames_about"));
}

// Your class as the site's card shows it: the emblem large over its name and
// what it means. Without a class to show, just the words.
void DrawStanding(const ShellView& v, const MenuEntry& e) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, emblem = 88 * s;
    const auto& st = v.rankings.standing;
    const NameMarks marks{CountryCode(), st.value.badge};
    if (st.loaded && st.value.kind == Standing::Kind::Ranked && st.value.classesShown && NameMarksWidth(marks, emblem) > 0) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        DrawNameMarks(ImGui::GetWindowDrawList(), at.x, at.y, emblem, marks);
        ImGui::Dummy(ImVec2(width, emblem + 12 * s));
    }
    Title(e.label);
    Muted(e.detail);
}

void Draw(const ShellView&, const MenuEntry&, std::monostate) {}

void Draw(const ShellView& v, const MenuEntry& e, StandingPane) { DrawStanding(v, e); }

// Your rating as the site's Rating details page gives it: the three numbers,
// where your class moves, what your next game would move, then what each
// number means.
void Draw(const ShellView&, const MenuEntry& e, RatingPane pane) {
    auto* d = ImGui::GetWindowDrawList();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto& c = v2::Colors();
    const auto& r = pane.rating;
    Title(loc::T("rankings.rating_title"));
    Muted(loc::T("rankings.rating_private"));
    Gap(10);
    char vol[32];
    std::snprintf(vol, sizeof(vol), "%.3f", r.volatility);
    FactsAcross({{loc::T("rankings.rating"), Whole(r.rating)},
                 {loc::T("rankings.deviation"), Whole(r.deviation)},
                 {loc::T("rankings.volatility"), vol}}, 3);
    if (r.upAt && r.downBelow && *r.upAt > *r.downBelow) {
        Gap(14);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float part = static_cast<float>(std::clamp((r.rating - *r.downBelow) / (*r.upAt - *r.downBelow), 0., 1.));
        v2::Meter(d, v2::Box{at, {at.x + width, at.y + 8 * s}}, part, 0);
        const float textY = at.y + 8 * s + 14 * s;
        if (r.band > 1)
            v2::Text(d, ImVec2(at.x, textY), width * .5f, loc::Tf("rankings.down_to", ClassName(static_cast<std::uint8_t>(r.band - 1)), Whole(*r.downBelow)), 14 * s, c.muted);
        if (r.band < 10)
            v2::TextEnd(d, ImVec2(at.x, textY), width, loc::Tf("rankings.up_to", ClassName(static_cast<std::uint8_t>(r.band + 1)), Whole(*r.upAt)), 14 * s, c.muted);
        ImGui::Dummy(ImVec2(width, 8 * s + 14 * s + 18 * s));
        Gap(6);
        if (!e.detail.empty()) Muted(e.detail);
    }
    if (!r.examples.empty()) {
        Section(loc::T("rankings.next_game"));
        const ImVec2 header = ImGui::GetCursorScreenPos();
        v2::Text(d, header, 150 * s, loc::T("rankings.opponent"), 13 * s, c.muted);
        v2::TextEnd(d, header, width - 72 * s, loc::T("rankings.win"), 13 * s, c.muted);
        v2::TextEnd(d, header, width, loc::T("rankings.loss"), 13 * s, c.muted);
        const float lineY = header.y + 20 * s;
        d->AddLine(ImVec2(header.x, lineY), ImVec2(header.x + width, lineY), c.border, 1.f);
        ImGui::Dummy(ImVec2(width, 22 * s));
        const std::size_t count = (std::min)(r.examples.size(), (std::size_t)4);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& ex = r.examples[i];
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const std::string left = ex.band ? loc::Tf("rankings.example", ClassName(ex.band), Whole(ex.opponent)) : loc::T("rankings.example_new");
            v2::Text(d, at, width - 150 * s, left, 15 * s, c.text);
            v2::TextEnd(d, at, width - 72 * s, Signed(ex.win.change), 15 * s, c.ready);
            v2::TextEnd(d, at, width, Signed(ex.loss.change), 15 * s, c.danger);
            ImGui::Dummy(ImVec2(width, 26 * s));
        }
    }
    const struct { const char* name; const char* about; } abouts[] = {
        {"rankings.rating", "rankings.rating_about"},
        {"rankings.deviation", "rankings.deviation_about"},
        {"rankings.volatility", "rankings.volatility_about"},
    };
    for (const auto& a : abouts) {
        Gap(10);
        v2::Paragraph(loc::T(a.name), 15 * s, c.text, HeadingFont());
        Muted(loc::T(a.about));
    }
}

// A points board: the fighter's face and name over its total (all fighters'
// BP board just its title), then how points are earned and the frames.
void DrawPointsBoard(const ShellView& v) {
    auto* d = ImGui::GetWindowDrawList();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto& c = v2::Colors();
    const auto* b = v.rankings.points.loaded ? v.rankings.points.value.get() : nullptr;
    if (!b) {
        Title(loc::T("rankings.points_title"));
        Muted(loc::T("rankings.points_detail"));
        return;
    }
    if (const auto* fighter = b->fighter >= 0 ? selection::FindFighter(b->fighter) : nullptr) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float face = 72 * s, x = at.x + face + 14 * s;
        DrawCharacterPortrait(b->fighter, at, ImVec2(at.x + face, at.y + face));
        d->AddRect(at, ImVec2(at.x + face, at.y + face), c.border, 2 * s, 0, 1.f);
        v2::Text(d, ImVec2(x, at.y + 10 * s), width - face - 14 * s, fighter->name, 22 * s, c.text, HeadingFont());
        v2::Text(d, ImVec2(x, at.y + 40 * s), width - face - 14 * s, loc::T("rankings.points_fp_title"), 15 * s, c.muted);
        ImGui::Dummy(ImVec2(width, face + 10 * s));
        Muted(loc::Tf("rankings.points_fp_total", b->total, fighter->name));
    } else {
        Title(loc::T("rankings.points_bp_title"));
        Muted(loc::Tf("rankings.points_bp_total", b->total));
    }
    Section(loc::T("fighters.how"));
    Muted(loc::T("fighters.how_detail"));
    FramesSection();
}

void Draw(const ShellView& v, const MenuEntry&, PointsBoardPane) { DrawPointsBoard(v); }

// A line of the points board: the player's tag over their points, then their place, frame, class and country.
void Draw(const ShellView&, const MenuEntry&, PointsPlayerPane pane) {
    auto* d = ImGui::GetWindowDrawList();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto* b = &pane.board;
    const auto& line = pane.line;
    PlayerTag(line.listing, line.band);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    v2::Level(d, cursor, 30 * s, line.points, b->fighter < 0 ? "points.account_label" : "points.label");
    ImGui::Dummy(ImVec2(width, 30 * s + 12 * s));
    std::vector<v2::TagView::Fact> facts{
        {loc::T("rankings.place"), loc::Tf("rankings.place_of", line.place, b->total)},
        {loc::T("rankings.frame"), v2::TierName(v2::FrameTier(line.tier))},
        {loc::T("rankings.class"), line.band ? ClassName(line.band) : loc::T("rankings.no_class")},
    };
    if (line.listing.country.Present())
        facts.push_back({loc::T("rankings.country"), CountryName(line.listing.country)});
    if (b->fighter >= 0) {
        const auto* fighter = selection::FindFighter(b->fighter);
        if (fighter)
            facts.push_back({loc::T("rankings.points_fighter"), fighter->name});
    }
    FactsAcross(facts, 2);
    if (!line.card.empty()) {
        Gap(10);
        Muted(loc::T("rankings.opens_card"));
    }
}

// A player of the class board: their tag, class and country.
void Draw(const ShellView&, const MenuEntry&, ClassPlayerPane pane) {
    const auto& entry = pane.entry;
    PlayerTag(entry.listing, entry.band);
    std::vector<v2::TagView::Fact> facts{{loc::T("rankings.class"), ClassName(entry.band)}};
    if (entry.listing.country.Present())
        facts.push_back({loc::T("rankings.country"), CountryName(entry.listing.country)});
    FactsAcross(facts, 2);
    Gap(10);
    Muted(loc::T("rankings.opens_card"));
}

// The pane the focused row takes, its answer found and checked here once:
// none while the answer it shows is missing, so the menu's own pane stands in.
Pane PaneOf(const ShellView& v, const std::string& id) {
    const auto& rankings = v.rankings;
    if (id == "rk-standing") return StandingPane{};
    if (id == "rk-rating") {
        if (rankings.rating.loaded && rankings.rating.value.rating) return RatingPane{*rankings.rating.value.rating};
        return {};
    }
    if (id == "rkp-fighter") return PointsBoardPane{};
    if (const auto at = RowKey<std::size_t>(id, "rkp-p:")) {
        const auto* b = rankings.points.loaded ? rankings.points.value.get() : nullptr;
        if (b && *at < b->lines.size()) return PointsPlayerPane{*b, b->lines[*at]};
        return {};
    }
    if (const auto at = RowKey<std::size_t>(id, "rkb-p:")) {
        const auto* board = rankings.board.loaded ? rankings.board.value.get() : nullptr;
        if (board && *at < board->size()) return ClassPlayerPane{(*board)[*at]};
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
