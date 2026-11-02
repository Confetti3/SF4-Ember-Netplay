#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include "../training/PracticeSettings.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

// The Data panel: each fighter's script, position, state, gauges, juggle and
// charges as the game holds them this frame (training::DataView), for those
// who write frame data down. Drawn over the fight clear of the frame meter's
// HUD (PlacePanel), never taking input.
namespace sf4e { namespace ui {
namespace {
using training::FighterData;
// The meter's own colours for P1 and P2, and a stance's chip.
constexpr ImU32 SideColor[2] = {IM_COL32(255, 120, 110, 255), IM_COL32(120, 190, 255, 255)};
constexpr ImU32 Track = IM_COL32(255, 255, 255, 28), Rule = IM_COL32(255, 255, 255, 36);
constexpr ImU32 Stun = IM_COL32(246, 176, 48, 255), Full = IM_COL32(60, 196, 110, 255), Super = IM_COL32(70, 150, 255, 255), Revenge = IM_COL32(236, 76, 60, 255);
ImU32 StanceColor(training::Stance stance) {
    switch (stance) {
    case training::Stance::Crouching: return IM_COL32(80, 170, 160, 255);
    case training::Stance::Airborne: return IM_COL32(90, 150, 240, 255);
    case training::Stance::Down: return IM_COL32(150, 92, 196, 255);
    default: return IM_COL32(120, 120, 120, 255);
    }
}
const char* StanceName(training::Stance stance) {
    switch (stance) {
    case training::Stance::Crouching: return "training.data.crouching";
    case training::Stance::Airborne: return "training.data.airborne";
    case training::Stance::Down: return "training.data.down";
    default: return "training.data.standing";
    }
}
const char* RangeRuleText(training::RangeRule rule) {
    switch (rule) {
    case training::RangeRule::From: return "training.data.range_from";
    case training::RangeRule::Within: return "training.data.range_within";
    case training::RangeRule::HeightFrom: return "training.data.height_from";
    case training::RangeRule::HeightWithin: return "training.data.height_within";
    default: return nullptr;
    }
}
std::string Number(const char* format, double value) { char text[32]; std::snprintf(text, sizeof text, format, value); return text; }
std::string Count(int value) { return value < 0 ? std::string("-") : std::to_string(value); }

// A block of rows drawn straight on the window, `width` wide from its left
// edge; Dummy at the end makes the window as tall as what was drawn. With no
// draw list the rows only measure: y goes as far as drawing them would.
struct Rows {
    ImDrawList* draw; float s, left, width, y, line, small;
    ImVec2 Measure(const std::string& text, float size) const { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()); }
    void Text(float x, float top, float size, ImU32 colour, const std::string& text) const { if (draw) draw->AddText(ImGui::GetFont(), size, ImVec2(x, top), colour, text.c_str()); }
    void Right(float right, float top, float size, ImU32 colour, const std::string& text) const { Text(right - Measure(text, size).x, top, size, colour, text); }
    void Rect(ImVec2 from, ImVec2 to, ImU32 colour, float rounding = 0, ImDrawFlags flags = 0) const { if (draw) draw->AddRectFilled(from, to, colour, rounding, flags); }
    void Line(ImVec2 from, ImVec2 to, ImU32 colour, float thickness) const { if (draw) draw->AddLine(from, to, colour, thickness); }
    void Triangle(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 colour) const { if (draw) draw->AddTriangleFilled(a, b, c, colour); }
    // A section's name in Ember small capitals and a rule to the edge.
    void Section(const char* id) {
        y += 4 * s;
        const std::string name = loc::T(id);
        Text(left, y, small, palette::Ember, name);
        const float after = left + Measure(name, small).x + 6 * s;
        Line(ImVec2(after, y + small * .55f), ImVec2(left + width, y + small * .55f), Rule, s);
        y += small + 3 * s;
    }
    // A label on the left and its value on the right of the row.
    void Pair(const std::string& label, const std::string& value, ImU32 colour = palette::Ivory) {
        Text(left, y + (line - small) * .5f, small, palette::Muted, label);
        Right(left + width, y, line, colour, value);
        y += line + 2 * s;
    }
    // Two pairs side by side, each half the row.
    void Pairs(const std::string& a, const std::string& av, const std::string& b, const std::string& bv) {
        const float half = width * .5f, gap = 8 * s;
        Text(left, y + (line - small) * .5f, small, palette::Muted, v2::Fit(a, half - gap - Measure(av, line).x - 6 * s, small, nullptr, false));
        Right(left + half - gap, y, line, palette::Ivory, av);
        Text(left + half + gap, y + (line - small) * .5f, small, palette::Muted, v2::Fit(b, half - gap - Measure(bv, line).x - 6 * s, small, nullptr, false));
        Right(left + width, y, line, palette::Ivory, bv);
        y += line + 2 * s;
    }
    // A label (from `indent`) and its value over a bar the row's width,
    // filled to `share` and cut in `parts` (a gauge's bars); grey up to
    // `behind` under the fill, as life that refills.
    void Bar(const std::string& label, float share, int parts, ImU32 colour, const std::string& value, float indent = 0, float behind = 0) {
        const float height = 5 * s;
        Text(left + indent, y + (line - small) * .5f, small, palette::Muted, v2::Fit(label, width - indent - Measure(value, line).x - 8 * s, small, nullptr, false));
        Right(left + width, y, line, palette::Ivory, value);
        y += line + s;
        const float x0 = left, x1 = left + width, top = y;
        Rect(ImVec2(x0, top), ImVec2(x1, top + height), Track, 2 * s);
        behind = (std::max)(0.f, (std::min)(1.f, behind));
        if (behind > 0) Rect(ImVec2(x0, top), ImVec2(x0 + (x1 - x0) * behind, top + height), IM_COL32(150, 150, 150, 200), 2 * s);
        share = (std::max)(0.f, (std::min)(1.f, share));
        if (share > 0) Rect(ImVec2(x0, top), ImVec2(x0 + (x1 - x0) * share, top + height), colour, 2 * s);
        for (int part = 1; part < parts; ++part) {
            const float at = x0 + (x1 - x0) * part / parts;
            Line(ImVec2(at, top), ImVec2(at, top + height), IM_COL32(0, 0, 0, 200), 1.5f * s);
        }
        y += height + 5 * s;
    }
};

// The script as a line, its frames left to right in the colours the frame
// meter gives them (training::ScriptRuns), and the frame it is on as an
// ivory mark.
void Timeline(Rows& rows, const FighterData& fighter, const bac::Script& script) {
    const float height = 7 * rows.s;
    // A measuring pass needs only the timeline's height.
    if (!rows.draw) { rows.y += height + 7 * rows.s; return; }
    const float x0 = rows.left, x1 = rows.left + rows.width, top = rows.y + 2 * rows.s;
    rows.Rect(ImVec2(x0, top), ImVec2(x1, top + height), Track, 2 * rows.s);
    const float total = static_cast<float>((std::max)(1, script.total));
    const auto at = [&](float frame) { return x0 + (x1 - x0) * (std::max)(0.f, (std::min)(1.f, frame / total)); };
    for (const auto& run : training::ScriptRuns(*fighter.scripts, fighter.script)) {
        training::MeterCell cell; cell.kind = run.kind;
        // The line's own rounded ends stay round; a run of a single frame stays in sight.
        const ImDrawFlags ends = (run.from == 0 ? ImDrawFlags_RoundCornersLeft : 0) | (run.to == script.total ? ImDrawFlags_RoundCornersRight : 0);
        rows.Rect(ImVec2(at(static_cast<float>(run.from)), top), ImVec2((std::max)(at(static_cast<float>(run.to)), at(static_cast<float>(run.from)) + rows.s), top + height),
            TrainingCellColor(cell, true), ends ? 2 * rows.s : 0, ends);
    }
    // The frame it can be cancelled from, as a thin line.
    if (script.interruptible > 0 && script.interruptible < script.total)
        rows.Line(ImVec2(at(static_cast<float>(script.interruptible)), top - rows.s), ImVec2(at(static_cast<float>(script.interruptible)), top + height + rows.s), palette::Muted, rows.s);
    if (script.total > 0) {
        const float mark = at(fighter.scriptFrame);
        rows.Rect(ImVec2(mark - rows.s, top - 2 * rows.s), ImVec2(mark + rows.s, top + height + 2 * rows.s), palette::Ivory);
    }
    rows.y += height + 7 * rows.s;
}

// The fighter's tag and the battle's frame count.
void HeaderRows(Rows& rows, const training::DataView& data, int side) {
    const char tag[3] = {'P', static_cast<char>('1' + side), 0};
    rows.Text(rows.left, rows.y, rows.line * 1.1f, SideColor[side], tag);
    rows.Right(rows.left + rows.width, rows.y + (rows.line * 1.1f - rows.small) * .5f, rows.small, palette::Muted,
        loc::Tf("training.data.frame", data.frame));
    rows.y += rows.line * 1.1f + 2 * rows.s;
}
void MoveRows(Rows& rows, const training::View& view, int side) {
    const FighterData& fighter = view.data.fighters[side];
    rows.Section("training.data.move");
    // The script's name and number, and the frame it is on of its total.
    const bac::Script* named = fighter.Script();
    const int total = named ? named->total : 0;
    const std::string script = "#" + std::to_string(fighter.script);
    const std::string frame = (std::fabs(fighter.scriptFrame - std::round(fighter.scriptFrame)) < .01f ? Number("%.0f", fighter.scriptFrame) : Number("%.2f", fighter.scriptFrame)) +
        (total > 0 ? " / " + std::to_string(total) : std::string());
    // The name comes first; the number shows when there is room for it too.
    const float room = rows.width - rows.Measure(frame, rows.line).x - 8 * rows.s;
    float x = rows.left;
    if (named && !named->name.empty()) {
        const std::string name = v2::Fit(named->name, room, rows.line, nullptr, false);
        rows.Text(x, rows.y, rows.line, palette::Ivory, name);
        x += rows.Measure(name, rows.line).x + 6 * rows.s;
    }
    if (x + rows.Measure(script, rows.small).x <= rows.left + room) rows.Text(x, rows.y + (rows.line - rows.small) * .5f, rows.small, palette::Muted, script);
    rows.Right(rows.left + rows.width, rows.y, rows.line, palette::Ivory, frame);
    rows.y += rows.line + 2 * rows.s;
    if (named) Timeline(rows, fighter, *named);
    // The frame the script can be cancelled from (its header's), and the
    // advantage the meter measured for this fighter.
    const auto& advantage = training::ShownAdvantage(view.meter);
    const int frames = advantage.frames[side];
    rows.Pairs(loc::T("training.data.interruptible"), named && named->interruptible > 0 ? std::to_string(named->interruptible) : std::string("-"),
        loc::T("training.data.advantage"), advantage.valid ? (frames > 0 ? "+" : "") + std::to_string(frames) : std::string("-"));
    // A close or far normal's rule: the distance at which the game picks it.
    if (const char* rule = RangeRuleText(fighter.rangeRule))
        rows.Pair(loc::T("training.data.range"), loc::Tf(rule, Number("%.3f", fighter.range)));
}
void PositionRows(Rows& rows, const FighterData& fighter, float distance) {
    rows.Section("training.data.position");
    rows.Pairs("X", Number("%.3f", fighter.x), "Y", Number("%.3f", fighter.y));
    rows.Pair(loc::T("training.data.distance"), Number("%.3f", distance));
}
void StateRows(Rows& rows, const FighterData& fighter) {
    rows.Section("training.data.state");
    // The stance as a coloured chip, the game's own status number beside it.
    const std::string stance = loc::T(StanceName(fighter.stance));
    const ImVec2 size = rows.Measure(stance, rows.small);
    const ImVec2 chip(rows.left, rows.y + 1 * rows.s);
    rows.Rect(chip, ImVec2(chip.x + size.x + 12 * rows.s, chip.y + rows.line - 2 * rows.s), StanceColor(fighter.stance), 3 * rows.s);
    rows.Text(chip.x + 6 * rows.s, rows.y + (rows.line - rows.small) * .5f, rows.small, IM_COL32(255, 255, 255, 255), stance);
    rows.Right(rows.left + rows.width, rows.y + (rows.line - rows.small) * .5f, rows.small, palette::Muted, loc::Tf("training.data.status", fighter.status));
    rows.y += rows.line + 2 * rows.s;
}
void GaugeRows(Rows& rows, const FighterData& fighter) {
    rows.Section("training.data.gauges");
    // Two a row: life with its grey part that refills and stun, then
    // Super in its four bars and Revenge in two halves, as the game
    // draws them.
    const float gap = 10 * rows.s, half = (rows.width - gap) * .5f;
    const auto share = [](int amount, int top) { return top ? static_cast<float>(amount) / top : 0.f; };
    Rows life = rows, stun = rows;
    life.width = stun.width = half;
    stun.left += half + gap;
    life.Bar(loc::T("training.data.life"), share(fighter.life, fighter.lifeMax), 1, Full, std::to_string(fighter.life), 0, share(fighter.recoverable, fighter.lifeMax));
    stun.Bar(loc::T("training.data.stun"), share(fighter.stun, fighter.stunMax), 1, Stun, std::to_string(fighter.stun));
    rows.y = life.y;
    Rows super = rows, revenge = rows;
    super.width = revenge.width = half;
    revenge.left += half + gap;
    super.Bar(loc::T("training.data.super"), share(fighter.super, fighter.superMax), 4, Super, std::to_string(fighter.super));
    revenge.Bar(loc::T("training.data.revenge"), share(fighter.revenge, fighter.revengeMax), 2, Revenge, std::to_string(fighter.revenge));
    rows.y = super.y;
}
void JuggleRows(Rows& rows, const FighterData& fighter) {
    rows.Section("training.data.juggle");
    // The fighter's own juggle count while it is juggled, then the move's
    // hit: the count a launch sets, what each hit adds, and the count up
    // to which it still connects, each in a cell of its own.
    const float gap = 4 * rows.s, cell = (rows.width - 3 * gap) / 4, height = rows.small + rows.line + 6 * rows.s;
    const std::pair<const char*, std::string> values[] = {
        {"training.data.juggled", fighter.juggled < 0 ? std::string(loc::T("training.data.none")) : std::to_string(fighter.juggled)},
        {"training.data.juggle_start", Count(fighter.juggle.start)}, {"training.data.juggle_add", Count(fighter.juggle.add)},
        {"training.data.juggle_limit", Count(fighter.juggle.limit)}};
    for (int i = 0; i < 4; ++i) {
        const float x = rows.left + (cell + gap) * i;
        rows.Rect(ImVec2(x, rows.y), ImVec2(x + cell, rows.y + height), i ? Track : IM_COL32(255, 255, 255, 44), 3 * rows.s);
        const std::string label = v2::Fit(loc::T(values[i].first), cell - 4 * rows.s, rows.small, nullptr, false);
        rows.Text(x + (cell - rows.Measure(label, rows.small).x) * .5f, rows.y + 2 * rows.s, rows.small, palette::Muted, label);
        const ImU32 colour = !i && fighter.juggled < 0 ? palette::Muted : palette::Ivory;
        rows.Text(x + (cell - rows.Measure(values[i].second, rows.line).x) * .5f, rows.y + rows.small + 3 * rows.s, rows.line, colour, values[i].second);
    }
    rows.y += height + 3 * rows.s;
}
void ChargeRows(Rows& rows, const FighterData& fighter) {
    rows.Section("training.data.charge");
    if (!fighter.chargeCount) rows.Pair(loc::T("training.data.no_charge"), std::string());
    // Each charge by its name in the command file after an arrow for the
    // directions it holds (back, down or down-back, as on a P1 stick),
    // green once it is full.
    const float icon = rows.small * .8f;
    for (std::size_t i = 0; i < fighter.chargeCount; ++i) {
        const auto& charge = fighter.charges[i];
        const bool back = (charge.directions & training::ChargeBack) != 0, down = (charge.directions & training::ChargeDown) != 0;
        const float x0 = rows.left, y0 = rows.y + (rows.line - icon) * .5f, x1 = x0 + icon, y1 = y0 + icon;
        if (back && down) rows.Triangle(ImVec2(x0, y0), ImVec2(x0, y1), ImVec2(x1, y1), palette::Ivory);
        else if (back) rows.Triangle(ImVec2(x0, (y0 + y1) * .5f), ImVec2(x1, y0), ImVec2(x1, y1), palette::Ivory);
        else if (down) rows.Triangle(ImVec2(x0, y0), ImVec2(x1, y0), ImVec2((x0 + x1) * .5f, y1), palette::Ivory);
        const bool full = charge.required > 0 && charge.held >= charge.required;
        rows.Bar(fighter.ChargeName(charge.index), charge.required ? static_cast<float>(charge.held) / charge.required : 0.f, 1,
            full ? Full : palette::Ember, std::to_string(charge.held) + " / " + std::to_string(charge.required), icon + 4 * rows.s);
    }
}

// The panel's frame round its rows, the gap it keeps from the HUD and between
// its two columns, and a column's widest and narrowest, at the HUD's scale;
// and its line of text.
constexpr float PadX = 10, PadY = 8, Gap = 12, Widest = 250, Narrowest = 170, LineHeight = 15;
// rows.width is a column's: the panel's tag runs over the whole width, then
// its sections go down one column, or in two side by side (the move, position
// and state; then gauges, juggle and charges).
void Fighter(Rows& rows, const training::View& view, int side, const training::MeterOptions& options, bool twoColumns) {
    Rows header = rows;
    if (twoColumns) header.width = 2 * rows.width + Gap * rows.s;
    HeaderRows(header, view.data, side);
    rows.y = header.y;
    Rows right = rows;
    right.left += rows.width + Gap * rows.s;
    Rows& later = twoColumns ? right : rows;
    const FighterData& fighter = view.data.fighters[side];
    if (options.dataMove) MoveRows(rows, view, side);
    if (options.dataPosition) PositionRows(rows, fighter, view.data.distance);
    if (options.dataState) StateRows(rows, fighter);
    if (options.dataGauges) GaugeRows(later, fighter);
    if (options.dataJuggle) JuggleRows(later, fighter);
    if (options.dataCharge) ChargeRows(later, fighter);
    rows.y = (std::max)(rows.y, later.y);
}
// Where a fighter's panel goes: its top, its scale and column width, and
// whether it takes two columns.
struct Place { float top, scale, width; bool twoColumns; };
// Under the fighter's life bar beside the HUD (hud), one column as wide as
// the room there allows up to Widest. Where that is under Narrowest, two
// columns over its half of the screen, in the taller of the bands above and
// below the HUD. A panel too tall for where it goes is drawn smaller until it
// fits: every length in it goes with its scale, so its height does too.
Place PlacePanel(const training::View& view, int side, const training::MeterOptions& options, const TrainingHudArea& hud, float hudScale) {
    const auto* vp = ImGui::GetMainViewport();
    const float s = hudScale, margin = vp->Size.x * .02f, frame = 2 * PadX * s, gap = Gap * s;
    const float top = vp->Pos.y + vp->Size.y * .17f, bottom = vp->Pos.y + vp->Size.y - margin;
    const float beside = (side ? vp->Pos.x + vp->Size.x - margin - hud.right : hud.left - vp->Pos.x - margin) - gap - frame;
    Place place{top, s, (std::min)(Widest * s, beside), false};
    float room = bottom - top;
    if (beside < Narrowest * s) {
        const float half = (vp->Size.x - 2 * margin - gap) * .5f - frame;
        const float under = (std::max)(top, hud.bottom + gap), above = hud.top - gap - top, below = bottom - under;
        place = Place{above >= below ? top : under, s, (std::min)(Widest * s, (half - gap) * .5f), true};
        room = (std::max)(above, below);
    }
    Rows probe{nullptr, s, 0, place.width, 0, LineHeight * s, LineHeight * s * .78f};
    Fighter(probe, view, side, options, place.twoColumns);
    const float tall = probe.y + 2 * PadY * s;
    if (tall > room) { place.scale *= (std::max)(0.f, room) / tall; place.width *= (std::max)(0.f, room) / tall; }
    return place;
}
}

void DrawTrainingData(const training::View& view, const TrainingHudArea& hud) {
    const auto options = TrainingMeterOptions();
    if (!options.data || !view.data.valid) return;
    const float hudScale = TrainingHudScale();
    const auto* vp = ImGui::GetMainViewport();
    const float margin = vp->Size.x * .02f;
    // Each fighter's panel on its own side of the screen, clear of the HUD.
    const auto window = [&](const char* name, int side) {
        const Place place = PlacePanel(view, side, options, hud, hudScale);
        const float s = place.scale;
        if (!(s > 0)) return;
        ImGui::SetNextWindowPos(ImVec2(side ? vp->Pos.x + vp->Size.x - margin : vp->Pos.x + margin, place.top), ImGuiCond_Always, ImVec2(static_cast<float>(side), 0));
        ImGui::SetNextWindowBgAlpha(options.dataOpacity / 100.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PadX * s, PadY * s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * s);
        if (ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            Rows rows{ImGui::GetWindowDrawList(), s, start.x, place.width, start.y, LineHeight * s, LineHeight * s * .78f};
            Fighter(rows, view, side, options, place.twoColumns);
            ImGui::Dummy(ImVec2(place.twoColumns ? 2 * place.width + Gap * s : place.width, rows.y - start.y));
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    };
    if (options.dataFighters != training::DataFighter::P2) window("Training data P1", 0);
    if (options.dataFighters != training::DataFighter::P1) window("Training data P2", 1);
}
} }
