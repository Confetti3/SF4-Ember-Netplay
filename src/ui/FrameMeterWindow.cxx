#include "FrameMeterWindow.hxx"
#include "Theme.hxx"
#include "MenuDialogs.hxx"
#include "MenuProbes.hxx"
#include "InputGlyphs.hxx"
#include "../common/ButtonLayoutTypes.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

// The frame meter's bars, numbers and colour key (FrameMeterWindow.hxx).
namespace sf4e { namespace ui {
namespace {
using namespace training;
ImU32 PhaseColor(Phase phase) {
    switch (phase) {
    case Phase::Neutral: return IM_COL32(100, 112, 110, 255);
    case Phase::Movement: return IM_COL32(88, 160, 204, 255);
    case Phase::Attack: return palette::Ember;
    case Phase::Guard: return IM_COL32(109, 151, 241, 255);
    case Phase::Hit: return IM_COL32(232, 91, 103, 255);
    case Phase::Down: return IM_COL32(178, 123, 210, 255);
    default: return IM_COL32(220, 206, 166, 255);
    }
}
ImU32 CellAlpha(ImU32 color, bool idle) {
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(idle ? 130 : 215) << IM_COL32_A_SHIFT);
}
// What a bar cell and the F6 colour key both draw for a phase; idle is dimmer.
ImU32 BarColor(Phase phase) { return CellAlpha(PhaseColor(phase), phase == Phase::Neutral); }
// Angled bars: an attack's startup, active and recovery in the theme's green,
// Ember and amber, and an attack whose script names no boundary in Muted.
// Hitstun and blockstun keep the flat bars' colours, as does every other kind.
ImU32 KindColor(MeterKind kind) {
    switch (kind) {
    case MeterKind::Startup: return palette::Ready;
    case MeterKind::Active: return palette::Ember;
    case MeterKind::Recovery: return ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
    case MeterKind::Attack: return palette::Muted;
    case MeterKind::Neutral: return PhaseColor(Phase::Neutral);
    case MeterKind::Movement: return PhaseColor(Phase::Movement);
    case MeterKind::Guard: return PhaseColor(Phase::Guard);
    case MeterKind::Hit: return PhaseColor(Phase::Hit);
    case MeterKind::Down: case MeterKind::Rise: return PhaseColor(Phase::Down);
    default: return PhaseColor(Phase::Unknown);
    }
}
ImU32 AngledColor(MeterKind kind) { return CellAlpha(KindColor(kind), kind == MeterKind::Neutral); }
// Text with a dark edge, to read on any fill.
void Edged(ImDrawList* draw, float size, ImVec2 at, ImU32 colour, const char* text) {
    for (const ImVec2 offset : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1)})
        draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + offset.x, at.y + offset.y), IM_COL32(10, 10, 10, 255), text);
    draw->AddText(ImGui::GetFont(), size, at, colour, text);
}
// The frames a bar shows: cells across it, and how many pages before the
// newest frame's it shows while a held meter is stepped back.
// smooth: the bars slide a frame at a time (MeterSlideIndex) instead of
// turning a page; back then counts frames, not pages.
struct BarWindow { int cells; std::size_t back; bool smooth = false; };
// The frame a bar cell shows, or -1.
int CellFrame(const MeterView& meter, BarWindow window, int cell) {
    return window.smooth ? MeterSlideIndex(meter.frames.size(), static_cast<std::size_t>(cell), static_cast<std::size_t>(window.cells), window.back) :
        MeterFrameIndex(meter.frames.size(), meter.length, static_cast<std::size_t>(cell), static_cast<std::size_t>(window.cells), window.back);
}
// Counting marks, the same in both styles: a thin dark gap parts every cell
// (left by the cells themselves), every fifth boundary is an ivory line and
// every tenth a thicker, brighter one with a tick under the bar. slant leans
// them with angled cells. They grow with the HUD (height is 14 at scale 1)
// and stay short of opaque, so a cell's colour still shows beside them.
void CountMarks(ImDrawList* draw, ImVec2 origin, float cell, float height, float slant, int cells) {
    const float unit = height / 14;
    for (int i = 5; i < cells; i += 5) {
        const bool tenth = i % 10 == 0;
        const float x = origin.x + i * cell;
        const float thickness = (std::max)(1.f, (tenth ? 2.f : 1.5f) * unit);
        const ImU32 colour = (palette::Ivory & ~IM_COL32_A_MASK) | (static_cast<ImU32>(tenth ? 220 : 150) << IM_COL32_A_SHIFT);
        draw->AddLine(ImVec2(x + slant, origin.y), ImVec2(x, origin.y + height), colour, thickness);
        draw->AddLine(ImVec2(x, origin.y + height), ImVec2(x, origin.y + height + (tenth ? 2.5f : 1.5f) * unit), colour, thickness);
    }
}
ImVec4 AdvantageColor(int frames) {
    return ImGui::ColorConvertU32ToFloat4(frames > 0 ? IM_COL32(118, 224, 160, 255) :
        frames < 0 ? IM_COL32(255, 121, 129, 255) : palette::Ivory);
}
// A hit's advantage (MeterCell::hitAdvantage) in the row between the bars:
// whose hit, its bracket's cells, and the span its number takes. The bracket
// is the advantage itself: ahead, the frames after the attacker's last
// recovery frame (or the last before its next move) in which the other is
// still stunned; behind, the attacker's last recovery frames, in which the
// other could already act. Even, it is the frame they both act on.
struct HitMark { int side, first, last, frames; float from, to; std::string text; };
// Both fighters' hits. left: where each bar's first cell meets that row.
// Where two numbers would overlap (a multi-hit move) the later hit's shows.
std::vector<HitMark> HitMarks(const MeterView& meter, BarWindow window, const float (&left)[2], float cell, float size) {
    std::vector<HitMark> marks;
    for (int i = window.cells - 1; i >= 0; --i) {
        const int index = CellFrame(meter, window, i);
        for (int side = 0; side < 2 && index >= 0; ++side) {
            const auto& shown = meter.frames[index].cells[side];
            if (shown.hitAdvantage == NoHitAdvantage) continue;
            int recovered = i;
            while (recovered + 1 < window.cells) {
                const int at = CellFrame(meter, window, recovered + 1);
                if (at < 0 || meter.frames[at].cells[side].newAction || meter.frames[at].cells[side].phase != Phase::Attack) break;
                ++recovered;
            }
            const int frames = shown.hitAdvantage;
            const int first = frames < 0 ? (std::max)(i, recovered + frames + 1) : recovered + 1;
            const int last = frames > 0 ? recovered + frames : frames < 0 ? recovered : recovered + 1;
            char text[8];
            std::snprintf(text, sizeof text, "%+d", frames);
            const float w = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text).x + 1, x = left[side] + (first + last + 1) * cell / 2 - w / 2;
            if (!marks.empty() && x + w + 2 > marks.back().from) continue;
            marks.push_back({side, first, last, frames, x, x + w, text});
        }
    }
    return marks;
}
// The row between the bars holds a bracket and its number, size tall.
inline float HitRow(float size) { return size * 1.2f; }
// Each hit's bracket in its advantage's colour, its ends towards its bar and
// its number on the other side, in the row from top; the first bar's above
// its number, the second's under it. Numbers are drawn twice a pixel apart to
// read bold.
void DrawHitMarks(ImDrawList* draw, const std::vector<HitMark>& marks, const float (&left)[2], float cell, float top, float size) {
    const float unit = size / 10, line = (std::max)(1.f, unit), bottom = top + HitRow(size);
    for (const auto& mark : marks) {
        const ImU32 colour = ImGui::ColorConvertFloat4ToU32(AdvantageColor(mark.frames));
        const float from = left[mark.side] + mark.first * cell + line, to = left[mark.side] + (mark.last + 1) * cell - line, mid = (from + to) / 2;
        const float edge = mark.side ? bottom : top, y = mark.side ? bottom - 2 * unit : top + 2 * unit, toward = mark.side ? -1.5f * unit : 1.5f * unit;
        draw->AddLine(ImVec2(from, edge), ImVec2(from, y), colour, line);
        draw->AddLine(ImVec2(from, y), ImVec2(to, y), colour, line);
        draw->AddLine(ImVec2(to, y), ImVec2(to, edge), colour, line);
        draw->AddLine(ImVec2(mid, y), ImVec2(mid, y + toward), colour, line);
        // The number tucks into the font's own space above its digits.
        const float textTop = mark.side ? top : y + toward - 1.5f * unit;
        for (const float bold : {0.f, 1.f}) Edged(draw, size, ImVec2(mark.from + bold, textTop), colour, mark.text.c_str());
    }
}
constexpr ImU32 StopColor = IM_COL32(120, 200, 255, 235);
// The stop line's dashes, slanted by slant, over a box from origin height tall.
void StopLine(ImDrawList* draw, ImVec2 origin, float height, float slant) {
    for (int dash = 0; dash < 3; ++dash) {
        const float a = dash / 3.f, b = a + 1 / 6.f;
        draw->AddLine(ImVec2(origin.x + slant * (1 - a), origin.y + height * a), ImVec2(origin.x + slant * (1 - b), origin.y + height * b), StopColor, (std::max)(1.f, height / 10));
    }
}
// On a bar: a dotted line where frames left out of the bars (hitstop, a
// super's freeze) were.
void DrawStops(ImDrawList* draw, const MeterView& meter, int side, BarWindow window, ImVec2 origin, float cell, float height, float slant) {
    for (int i = 0; i < window.cells; ++i) {
        const int index = CellFrame(meter, window, i);
        if (index < 0 || !meter.frames[index].cells[side].afterStop) continue;
        StopLine(draw, ImVec2(origin.x + i * cell, origin.y), height, slant);
    }
}
// A run's length, centred over its cells where the number fits. A neutral run
// or a single cell carries none.
void RunLength(ImDrawList* draw, ImVec2 origin, float slant, float cell, float height, float digits, int from, int to, MeterKind kind, ImU32 colour) {
    if (kind == MeterKind::Neutral || to - from < 2) return;
    const auto text = std::to_string(to - from);
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(digits, FLT_MAX, 0, text.c_str());
    if (extent.x + 2 > (to - from) * cell) return;
    Edged(draw, digits, ImVec2(origin.x + slant / 2 + (from + to) * cell / 2 - extent.x / 2, origin.y + (height - extent.y) / 2), colour, text.c_str());
}
// The walk both slanted bars share over their `cells` cells, left to right.
// cell(i, shown) draws a cell with a frame and returns the kind its run
// counts it as; missing(i) draws one with none yet, which ends no run. A cell
// that starts a new count inside a run of its own kind (a cancel, each hit
// of a combo) is parted from the one before by edge(i). Each run is passed to
// run(from, to, kind) as it ends; the leftmost cell shown starts a run
// whatever came before it, and the last ends at the newest frame, short of
// the right edge until the bars are full.
template <class Cell, class Missing, class Edge, class Run>
void WalkRuns(const MeterView& meter, int side, BarWindow window, int cells, Cell cell, Missing missing, Edge edge, Run run) {
    int runStart = -1, runEnd = 0; MeterKind runKind = MeterKind::Neutral;
    for (int i = 0; i < cells; ++i) {
        const int index = CellFrame(meter, window, i);
        if (index < 0) { missing(i); continue; }
        // The model resolved each cell as it arrived (training::MeterCell).
        const auto& shown = meter.frames[index].cells[side];
        const MeterKind kind = cell(i, shown);
        const bool split = runStart >= 0 && shown.split;
        if (split && kind == runKind && kind != MeterKind::Neutral) edge(i);
        if (runStart < 0) { runStart = i; runKind = kind; }
        else if (kind != runKind || split) { run(runStart, i, runKind); runStart = i; runKind = kind; }
        runEnd = i + 1;
    }
    if (runStart >= 0) run(runStart, runEnd, runKind);
}
// Stable's flat bars: a cell's colour is its state group, so an attack is one
// colour from start to end; a bright line marks a new native action.
void FlatBar(ImDrawList* draw, const MeterView& meter, int side, ImVec2 origin, float width, float height, BarWindow window) {
    const float cell = width / window.cells;
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(28, 29, 29, 100));
    for (int i = 0; i < window.cells; ++i) {
        const int index = CellFrame(meter, window, i);
        if (index < 0) continue;
        const auto& shown = meter.frames[index].cells[side];
        draw->AddRectFilled(ImVec2(origin.x + i * cell, origin.y),
            ImVec2(origin.x + (i + 1) * cell - (cell >= 3 ? 1.f : 0.f), origin.y + height), TrainingCellColor(shown, false));
        // Opaque and thicker than a counting mark, so the two do not read alike.
        if (shown.newAction)
            draw->AddLine(ImVec2(origin.x + i * cell, origin.y), ImVec2(origin.x + i * cell, origin.y + height), palette::Ivory, (std::max)(2.f, 2.5f * height / 14));
    }
    CountMarks(draw, origin, cell, height, 0, window.cells);
    DrawStops(draw, meter, side, window, origin, cell, height, 0);
}
// The angled bars, the default: a cell leans as the game's gauges do, its top
// edge further right; an attack shows its startup, active and recovery; and
// each run of one kind carries its length where the number fits. A new action
// inside a run (a cancel, a string's next hit) and each hit of a combo start
// a count of their own, parted from the one before by a dark edge.
void AngledBar(ImDrawList* draw, const MeterView& meter, int side, ImVec2 origin, float width, float height, BarWindow window) {
    const float slant = height * .35f, cell = (width - slant) / window.cells, digits = height * .82f;
    const auto quad = [&](float from, float to, ImU32 colour) {
        draw->AddQuadFilled(ImVec2(origin.x + from + slant, origin.y), ImVec2(origin.x + to + slant, origin.y),
            ImVec2(origin.x + to, origin.y + height), ImVec2(origin.x + from, origin.y + height), colour);
    };
    quad(0, width - slant, IM_COL32(28, 29, 29, 100));
    // The run lengths are drawn last, over the counting marks. The list is
    // kept from one bar to the next, so a drawn bar does not allocate it.
    struct Run { int from, to; MeterKind kind; };
    static std::vector<Run> runs;
    runs.clear();
    WalkRuns(meter, side, window, window.cells,
        [&](int i, const MeterCell& shown) {
            quad(i * cell, (i + 1) * cell - (cell >= 3 ? 1.f : 0.f), TrainingCellColor(shown, true));
            return shown.kind;
        },
        [](int) {},
        [&](int i) { quad(i * cell - 1, i * cell + 1, IM_COL32(10, 10, 10, 255)); },
        [&](int from, int to, MeterKind kind) { runs.push_back({from, to, kind}); });
    CountMarks(draw, origin, cell, height, slant, window.cells);
    DrawStops(draw, meter, side, window, origin, cell, height, slant);
    for (const auto& run : runs) RunLength(draw, origin, slant, cell, height, digits, run.from, run.to, run.kind, palette::Ivory);
}
// The last reading stays until the next one replaces it, dimmed while that
// one is being measured.
void Advantage(const MeterView& meter, int side) {
    const auto& shown = ShownAdvantage(meter);
    if (!shown.valid) { ImGui::TextDisabled("--"); return; }
    auto colour = AdvantageColor(shown.frames[side]);
    if (!meter.advantage.valid && meter.advantage.pending) colour.w = .45f;
    ImGui::TextColored(colour, "%+d f", shown.frames[side]);
}
// Why the advantage shown is missing or dimmed; nothing while a reading stands.
bool AdvantageExplained(const MeterView& meter) {
    return !meter.advantage.valid && (!ShownAdvantage(meter).valid || meter.advantage.pending);
}
std::string Unavailable(const char* label, MeasurementUnavailable reason) {
    const char* reasonId = "training.unavailable.invalid";
    switch (reason) {
    case MeasurementUnavailable::WaitingForAttackBoundary: reasonId = "training.unavailable.waiting_attack"; break;
    case MeasurementUnavailable::NoAttackBoundary: reasonId = "training.unavailable.no_attack"; break;
    case MeasurementUnavailable::NoContact: reasonId = "training.unavailable.no_contact"; break;
    case MeasurementUnavailable::MeasuringRecovery: reasonId = "training.unavailable.measuring_recovery"; break;
    case MeasurementUnavailable::Interrupted: reasonId = "training.unavailable.interrupted"; break;
    default: break;
    }
    return loc::Tf("training.unavailable", label, loc::T(reasonId));
}
// The frames a held meter shows: a notch of the mouse wheel over the bars
// (from, to) steps a page back, as far as the frames it keeps, and the newest
// frame's page shows again once it runs.
BarWindow HeldWindow(const MeterView& meter, int cells, ImVec2 from, ImVec2 to, bool smooth) {
    static std::size_t back = 0;
    if (!meter.frozen) { back = 0; return {cells, 0, smooth}; }
    // A notch is a page, or MeterSlideStep frames when the bars slide.
    const int notches = static_cast<int>(ImGui::GetIO().MouseWheel) * (smooth ? static_cast<int>(MeterSlideStep) : 1);
    if (notches && ImGui::IsMouseHoveringRect(from, to, false))
        back = notches > 0 ? back + notches : back - (std::min)(back, static_cast<std::size_t>(-notches));
    // Every frame, as Frames shown or the scrolling can change while the meter is held.
    back = (std::min)(back, smooth ? MeterSlideLimit(meter.frames.size(), static_cast<std::size_t>(cells)) :
        MeterBackLimit(meter.frames.size(), meter.length, static_cast<std::size_t>(cells)));
    return {cells, back, smooth};
}
void ClassicMeter(const MeterView& meter, float hudScale, const MeterOptions& options);
void Meter(const MeterView& meter, float hudScale, const MeterOptions& options) {
    if (options.style == MeterStyle::Classic) { ClassicMeter(meter, hudScale, options); return; }
    const float gap = 10 * hudScale;
    const auto measure=[&](const char* value){return ImGui::CalcTextSize(value).x;};
    // The startup and recovery columns are sized from the translated text they will show.
    const float startup=(std::max)(measure(loc::Tf("training.start_frames",999).c_str()),measure(loc::T("training.start_unknown")));
    const float recovery=options.recovery?(std::max)({measure(loc::Tf("training.recovery_frames",999).c_str()),measure(loc::T("training.recovery_unknown")),
        measure(loc::T("training.recovery_cancelled"))})+gap:0;
    const float label = options.numbers ? measure("P2") + gap + measure("+999 f") + gap + startup + gap + recovery : 0;
    // As tall as the text beside it allows, so the run lengths read too.
    const float height = 14 * hudScale;
    const float width = (std::max)(120.f, ImGui::GetContentRegionAvail().x - label);
    // The bars fill from the left and an exchange longer than them goes on
    // from their left edge (MeterFrameIndex).
    const ImVec2 bars(ImGui::GetCursorScreenPos().x + label, ImGui::GetCursorScreenPos().y);
    const float rows = 2 * ((std::max)(height, ImGui::GetTextLineHeight()) + ImGui::GetStyle().ItemSpacing.y);
    const BarWindow window = HeldWindow(meter, options.shown, bars, ImVec2(bars.x + width, bars.y + rows), options.smooth);
    for (int side = 0; side < 2; ++side) {
        const float rowStart = ImGui::GetCursorPosX();
        if (options.numbers) {
            ImGui::TextDisabled("P%d", side + 1);
            ImGui::SameLine(0,gap); Advantage(meter, side);
            ImGui::SameLine(0,gap);
            // A focus cancel's move keeps its startup through the focus and the dash.
            const auto* chain = ChainOf(meter.advantage, side);
            const int startup = chain ? chain->startup : meter.startupFrames[side];
            if (startup >= 0) ImGui::TextUnformatted(loc::Tf("training.start_frames", startup).c_str());
            else {
                ImGui::TextDisabled("%s", loc::T("training.start_unknown"));
                if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false))
                    ImGui::SetTooltip("%s", Unavailable(loc::T("training.startup"), chain ? chain->startupUnavailable : meter.startupUnavailable[side]).c_str());
            }
            // The last move's recovery, as Frame data counts it, once it is over.
            if (options.recovery) {
                const auto& move = meter.moves[side];
                ImGui::SameLine(0,gap);
                if (chain) ImGui::TextUnformatted(loc::T("training.recovery_cancelled"));
                else if (move.seen && !move.live && move.recovery > 0) ImGui::TextUnformatted(loc::Tf("training.recovery_frames", move.recovery).c_str());
                else ImGui::TextDisabled("%s", loc::T("training.recovery_unknown"));
            }
            if (options.bars) ImGui::SameLine(rowStart + label);
        }
        if (!options.bars) continue;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (options.style == MeterStyle::Flat) FlatBar(ImGui::GetWindowDrawList(), meter, side, origin, width, height, window);
        else AngledBar(ImGui::GetWindowDrawList(), meter, side, origin, width, height, window);
        ReportMenuCard(side ? "training-bar-2" : "training-bar-1", origin, ImVec2(origin.x + width, origin.y + height));
        ImGui::Dummy(ImVec2(width, height));
    }
}
// What a focus cancel was, from the reading it belongs to.
const char* ChainName(const CancelChain& chain, bool last = false) {
    if (chain.dash == ActionClass::BackDash) return last ? loc::T("training.last_focus_back") : loc::T("training.meter.focus_back");
    return last ? loc::T("training.last_focus_forward") : loc::T("training.meter.focus_forward");
}
// The focus cancel this exchange's reading ends on, for the line under the
// bars; a kept one would hide why the next move's startup is missing.
std::string ChainLine(const MeterView& meter) {
    for (int side = 0; side < 2; ++side)
        if (const auto* chain = ChainOf(meter.advantage, side)) return "P" + std::to_string(side + 1) + " " + ChainName(*chain);
    return {};
}
// The first measurement that is unavailable, and why, for the HUD's reason
// line: the pad and keys cannot hover the tooltips that say the same. A
// focus cancel the reading ends on comes before a missing startup.
std::string MeasurementReason(const MeterView& meter) {
    if(AdvantageExplained(meter))return Unavailable(loc::T("training.advantage"),meter.advantage.unavailable);
    const auto cancel=ChainLine(meter);
    if(!cancel.empty())return cancel;
    for(int side=0;side<2;++side)if(meter.startupFrames[side]<0&&!ChainOf(meter.advantage,side))
        return "P"+std::to_string(side+1)+" "+Unavailable(loc::T("training.startup"),meter.startupUnavailable[side]);
    return {};
}
// What a reading is, so a reader of it need not go by its place or its words.
enum class ReadingKind { Startup, Active, Recovery, Cancel, Meaty, Result, Last };
// advantage: a result's frames, 0 while there are none, for its colour.
struct Reading { std::string label, value; ReadingKind kind; int advantage = 0; };
std::vector<Reading> Readings(const MeterView& meter, int side) {
    const auto& move = meter.moves[side];
    const auto number = [](int value) { return value > 0 ? std::to_string(value) : std::string("--"); };
    // A focus cancel gives the move it cancelled, taken before the focus
    // began, and that move's recovery was cancelled.
    const auto& advantage = meter.advantage;
    const auto* chain = ChainOf(advantage, side);
    std::vector<Reading> readings{
        {loc::T("training.startup"), number(chain ? chain->startup : meter.startupFrames[side]), ReadingKind::Startup},
        {loc::T("training.meter.active"), number(chain ? chain->active : move.seen ? move.active : 0), ReadingKind::Active},
        {loc::T("training.meter.recovery"), chain ? std::string(loc::T("training.meter.cancelled")) : number(move.seen && !move.live ? move.recovery : 0), ReadingKind::Recovery}};
    if (chain) readings.push_back({ChainName(*chain), std::string(), ReadingKind::Cancel});
    // What the move met, and the frames it left its user ahead or behind:
    // this exchange's, so it is never told of another's move.
    const auto met = [&](const FrameAdvantage& exchange) -> const char* {
        return exchange.attacker == side ? (exchange.blocked ? "training.meter.block" : "training.meter.hit") : nullptr;
    };
    const auto signedFrames = [&](const FrameAdvantage& exchange) {
        char frames[16] = "--";
        if (exchange.valid) std::snprintf(frames, sizeof frames, "%+d", exchange.frames[side]);
        return std::string(frames);
    };
    const char* result = advantage.attacker < 0 && !advantage.pending && move.seen && !move.live ? "training.meter.whiff" : met(advantage);
    if (meter.meatyValid[side]) {
        const int offset = meter.meatyFrames[side];
        const bool landed = offset <= 0 && -offset < (std::max)(1, move.active);
        char timing[16]; std::snprintf(timing, sizeof timing, landed ? "%d" : "%+d", landed ? -offset : offset);
        readings.push_back({loc::T("training.meter.meaty"), timing, ReadingKind::Meaty});
    }
    const int frames = advantage.valid ? advantage.frames[side] : 0;
    if (result) readings.push_back({loc::T("training.result"), std::string(loc::T(result)) +
        (std::string(result) == "training.meter.whiff" ? "" : " " + signedFrames(advantage)), ReadingKind::Result, frames});
    else readings.push_back({loc::T("training.advantage"), signedFrames(advantage), ReadingKind::Result, frames});
    // The HUD keeps the last exchange measured until the next one is
    // (ShownAdvantage); here it is a reading of its own, apart from the moves above.
    const auto& last = meter.lastAdvantage;
    if (!advantage.valid && last.valid) {
        const auto* lastChain = ChainOf(last, side);
        readings.push_back({lastChain ? ChainName(*lastChain, true) : loc::T("training.last_advantage"),
            (met(last) ? std::string(loc::T(met(last))) + " " : std::string()) + signedFrames(last), ReadingKind::Last, last.frames[side]});
    }
    return readings;
}

// The first meter's colours, Street Fighter 6's: green startup, red active,
// blue recovery, yellow block, orange hit, pink meaty frames.
ImU32 ClassicColor(MeterKind kind) {
    switch (kind) {
    // Free frames stand out from the dim cells still to come, so the gap a hit or block leaves reads off the bars.
    case MeterKind::Neutral: return IM_COL32(150, 156, 162, 200);
    case MeterKind::Movement: return IM_COL32(120, 150, 160, 245);
    case MeterKind::Startup: return IM_COL32(60, 196, 110, 245);
    case MeterKind::Active: return IM_COL32(236, 56, 96, 245);
    case MeterKind::Recovery: return IM_COL32(60, 120, 232, 245);
    case MeterKind::Attack: return (palette::Ember & ~IM_COL32_A_MASK) | (245u << IM_COL32_A_SHIFT);
    case MeterKind::Guard: return IM_COL32(244, 222, 96, 245);
    case MeterKind::Hit: return IM_COL32(246, 176, 48, 245);
    case MeterKind::Down: return IM_COL32(150, 92, 196, 245);
    case MeterKind::Rise: return IM_COL32(216, 184, 240, 245);
    case MeterKind::Sequence: return IM_COL32(220, 206, 166, 245);
    case MeterKind::Meaty: return IM_COL32(255, 72, 214, 245);
    default: return IM_COL32(48, 50, 54, 245);
    }
}
// The data line's colour for a reading: the three move numbers in their bar
// colours, a meaty in pink, a result or advantage green ahead and red behind.
ImU32 ReadingColor(const Reading& reading) {
    switch (reading.kind) {
    case ReadingKind::Startup: return ClassicColor(MeterKind::Startup);
    case ReadingKind::Active: return ClassicColor(MeterKind::Active);
    case ReadingKind::Recovery: return ClassicColor(MeterKind::Recovery);
    case ReadingKind::Meaty: return ClassicColor(MeterKind::Meaty);
    case ReadingKind::Cancel: return palette::Ivory;
    case ReadingKind::Result: case ReadingKind::Last: break;
    }
    return ImGui::ColorConvertFloat4ToU32(AdvantageColor(reading.advantage));
}
// The first meter, as the classic style draws it: a data line per fighter
// (startup, active, recovery, meaty and the result), each fighter's presses on
// a lane beside its bar, and dots between the bars counting frames, every
// fifth bold. A cell keeps a width that reads, so more frames than fit show
// the newest that do instead of squeezing them. The first bar is set forward
// by the slant, so a frame's two cells meet straight across the row between
// the bars, and a hit's bracket there sits under both.
void ClassicMeter(const MeterView& meter, float hudScale, const MeterOptions& options) {
    auto* draw = ImGui::GetWindowDrawList();
    const float full = ImGui::GetContentRegionAvail().x;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    if (options.classicExtras)
        // The game's own trim: a bar that runs from orange into yellow.
        draw->AddRectFilledMultiColor(ImVec2(top.x, top.y), ImVec2(top.x + full, top.y + 2 * hudScale),
            palette::Ember, IM_COL32(255, 214, 72, 255), IM_COL32(255, 214, 72, 255), palette::Ember);
    ImGui::Dummy(ImVec2(full, 2 * hudScale));
    const float line = ImGui::GetTextLineHeight(), small = line * .72f, gap = 8 * hudScale, half = full / 2;
    if (options.classicData) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        for (int side = 0; side < 2; ++side) {
            const auto readings = Readings(meter, side);
            const char tag[3] = {'P', static_cast<char>('1' + side), 0};
            // Labels where there is room for them, coloured ticks with the numbers where there is not.
            // Each label and value is measured once, here, and drawn at that width.
            static std::vector<ImVec2> widths;
            widths.clear();
            float labelled = ImGui::CalcTextSize(tag).x + gap;
            for (const auto& reading : readings) {
                widths.push_back(ImVec2(ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, reading.label.c_str()).x, ImGui::CalcTextSize(reading.value.c_str()).x));
                labelled += widths.back().x + 4 * hudScale + widths.back().y + gap;
            }
            const bool labels = labelled <= half;
            float x = at.x + side * half;
            Edged(draw, line, ImVec2(x, at.y), side ? IM_COL32(120, 190, 255, 255) : IM_COL32(255, 120, 110, 255), tag);
            x += ImGui::CalcTextSize(tag).x + gap;
            for (std::size_t i = 0; i < readings.size(); ++i) {
                const auto& reading = readings[i];
                const ImU32 colour = ReadingColor(reading);
                // A result always names itself: hit, block or whiff is the news.
                if (labels || i + 1 == readings.size() || reading.value.empty()) {
                    draw->AddText(ImGui::GetFont(), small, ImVec2(x, at.y + line - small - 1), palette::Ember, reading.label.c_str());
                    x += widths[i].x + 4 * hudScale;
                } else {
                    draw->AddRectFilled(ImVec2(x, at.y + line * .25f), ImVec2(x + 3 * hudScale, at.y + line * .85f), colour);
                    x += 5 * hudScale;
                }
                const bool move = reading.kind == ReadingKind::Startup || reading.kind == ReadingKind::Active || reading.kind == ReadingKind::Recovery;
                Edged(draw, line, ImVec2(x, at.y), move ? IM_COL32(255, 255, 255, 255) : colour, reading.value.c_str());
                x += widths[i].y + gap;
            }
        }
        ImGui::Dummy(ImVec2(full, line));
    }
    if (!options.bars) return;
    // Geometry: the first bar set forward by the slant, and as many whole
    // fives of cells as fit at the narrowest width that still reads, never
    // more than Frames shown.
    // Upright cells when the slant is off: the bars then sit straight over each other.
    // The row between the bars holds the dots and each hit's bracket and advantage (HitRow).
    const float height = options.barHeight * hudScale, digits = height * .82f, slant = options.classicSlant ? height * .35f : 0.f;
    const float dots = HitRow(digits), lane = 9 * hudScale;
    const float usable = (std::max)(60.f, full - 2 * slant);
    const int fit = (std::max)(10, static_cast<int>(usable / (4 * hudScale)) / 5 * 5);
    const int cells = (std::min)(options.shown, fit);
    const float cell = usable / cells;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float lanes = options.classicInputs ? 2 * (lane + hudScale) : 0;
    const BarWindow window = HeldWindow(meter, cells, start, ImVec2(start.x + full, start.y + 2 * height + dots + lanes), options.smooth);
    const auto index = [&](int i) { return CellFrame(meter, window, i); };
    float y = start.y;
    // What each fighter pressed, at the frame they did: a direction as it
    // changes, a button as it goes down. Presses too close to draw apart move
    // right, in order, and are left out more than a few cells late.
    const auto inputs = [&](int side, float x0) {
        float cursor = x0;
        unsigned before = 0;
        for (int i = 0; i < cells; ++i) {
            const int at = index(i);
            if (at < 0) continue;
            const unsigned now = meter.frames[at].fighters[side].input;
            const float spotAt = x0 + slant / 2 + i * cell;
            const auto place = [&]() { const float x = (std::max)(spotAt, cursor); return x - spotAt > 6 * cell ? -1.f : x; };
            const unsigned way = now & 0xF;
            if (way != (before & 0xF) && way) {
                const int dx = ((way & 8) ? 1 : 0) - ((way & 4) ? 1 : 0), dy = ((way & 1) ? 1 : 0) - ((way & 2) ? 1 : 0);
                const char digit = static_cast<char>('5' + dx + 3 * dy);
                const float spot = place();
                if (digit != '5' && spot >= 0) cursor = spot + glyphs::Arrow(draw, ImVec2(spot, y), lane, digit, false, IM_COL32(235, 235, 235, 255));
            }
            for (int button = 0; button < static_cast<int>(gamepads::AttackMasks.size()); ++button) {
                if (!(now & gamepads::AttackMasks[button]) || (before & gamepads::AttackMasks[button])) continue;
                const float spot = place();
                if (spot >= 0) cursor = spot + glyphs::Disc(draw, ImVec2(spot, y), lane, button < 3 ? 'P' : 'K', glyphs::Strength[button % 3], IM_COL32(255, 255, 255, 255));
            }
            before = now;
        }
        y += lane + hudScale;
    };
    for (int side = 0; side < 2; ++side) {
        const float x0 = start.x + (side ? 0 : slant);
        if (options.classicInputs && side == 0) inputs(0, x0);
        const ImVec2 origin(x0, y);
        // A cell leans as the game's gauges do: its top edge sits further right.
        const auto quad = [&](float from, float to, ImU32 colour) {
            draw->AddQuadFilled(ImVec2(origin.x + from + slant, origin.y), ImVec2(origin.x + to + slant, origin.y),
                ImVec2(origin.x + to, origin.y + height), ImVec2(origin.x + from, origin.y + height), colour);
        };
        quad(0, cells * cell, IM_COL32(12, 12, 14, 200));
        // Each run's length is drawn as the run ends, before the cells after it.
        WalkRuns(meter, side, window, cells,
            [&](int i, const MeterCell& shown) {
                // Meaty frames, as the model marked them once the other was up (MeterCell::meaty).
                const MeterKind kind = options.classicColors && shown.meaty ? MeterKind::Meaty : shown.kind;
                quad(i * cell, (i + 1) * cell - (cell >= 3 ? 1.f : 0.f), options.classicColors ? ClassicColor(kind) : TrainingCellColor(shown, true));
                return kind;
            },
            // Frames still to come stay dim, so the bar keeps its length.
            [&](int i) { quad(i * cell, (i + 1) * cell - 1, IM_COL32(48, 50, 54, 120)); },
            [&](int i) { quad(i * cell - 1, i * cell + 1, IM_COL32(12, 12, 14, 255)); },
            [&](int from, int to, MeterKind kind) { RunLength(draw, origin, slant, cell, height, digits, from, to, kind, IM_COL32(255, 255, 255, 255)); });
        DrawStops(draw, meter, side, window, origin, cell, height, slant);
        if (options.classicExtras) {
            // Earlier frames lie to the left, later ones to the right, of what is shown.
            if (index(0) > 0) Edged(draw, digits, ImVec2(origin.x - digits * .45f, origin.y + (height - digits) / 2), IM_COL32(255, 214, 72, 255), "<");
            if (window.back > 0) Edged(draw, digits, ImVec2(origin.x + cells * cell + slant, origin.y + (height - digits) / 2), IM_COL32(255, 214, 72, 255), ">");
        }
        ReportMenuCard(side ? "training-bar-2" : "training-bar-1", origin, ImVec2(origin.x + cells * cell + slant, origin.y + height));
        y += height;
        if (side == 0) {
            // Between the bars, a dot a frame where its two cells meet, every
            // fifth bold, for counting.
            const float mid = y + dots / 2;
            for (int i = 0; i < cells; ++i) {
                const bool fifth = (i + 1) % 5 == 0;
                draw->AddCircleFilled(ImVec2(x0 + (i + .5f) * cell, mid), (fifth ? 1.6f : .9f) * hudScale,
                    fifth ? IM_COL32(255, 214, 72, 235) : IM_COL32(200, 200, 200, 150));
            }
            // Over the dots, each hit's bracket and advantage.
            const float left[2] = {x0, start.x + slant};
            DrawHitMarks(draw, HitMarks(meter, window, left, cell, digits), left, cell, y, digits);
            y += dots;
        }
        if (options.classicInputs && side == 1) inputs(1, x0);
    }
    ImGui::Dummy(ImVec2(full, y - start.y));
    if (options.classicExtras) {
        // What the colours mean, going on to a next line where one is full.
        const float left = ImGui::GetCursorScreenPos().x, box = line * .55f;
        float x = left, yKey = ImGui::GetCursorScreenPos().y;
        int lines = 1;
        auto entries = TrainingColorKeyEntries(true);
        // Meaty frames are pink only in Street Fighter 6's colours.
        if (options.classicColors) entries.push_back({"training.meter.meaty", Phase::Attack, MeterKind::Meaty, ClassicColor(MeterKind::Meaty)});
        // Places an entry's name and returns where its swatch goes.
        const auto place = [&](const char* name) {
            const float w = box + 3 * hudScale + ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, name).x + gap;
            if (x > left && x + w > top.x + full) { x = left; yKey += line; ++lines; }
            draw->AddText(ImGui::GetFont(), small, ImVec2(x + box + 3 * hudScale, yKey + line - small - 1), palette::Ivory, name);
            const ImVec2 at(x, yKey + (line - box) / 2);
            x += w;
            return at;
        };
        for (const auto& entry : entries) {
            const ImVec2 at = place(loc::T(entry.label));
            draw->AddRectFilled(at, ImVec2(at.x + box, at.y + box), options.classicColors ? ClassicColor(entry.kind) : entry.color);
        }
        const ImVec2 at = place(loc::T("training.meter.hitstop"));
        StopLine(draw, ImVec2(at.x + box * .25f, at.y), box, options.classicSlant ? box * .35f : 0.f);
        ImGui::Dummy(ImVec2(full, lines * line));
    }
}
}
std::string TrainingFrameData(const training::MeterView& meter, int side) {
    std::string text;
    for (const auto& reading : Readings(meter, side)) {
        if (!text.empty()) text += "\n";
        text += reading.label + (reading.value.empty() ? "" : ": " + reading.value);
    }
    if (!meter.meatyValid[side]) text += "\n" + std::string(loc::T("training.meter.meaty")) + ": --";
    return text;
}
unsigned TrainingCellColor(const training::MeterCell& cell, bool angled) {
    return angled ? AngledColor(cell.kind) : BarColor(cell.phase);
}
// The key lists what the bars draw, with the colour they draw it in. Idle has
// no entry: it is the dim gap between moves and has no label.
std::vector<TrainingKeyEntry> TrainingColorKeyEntries(bool angled) {
    struct Named { Phase phase; MeterKind kind; const char* label; };
    static const Named plain[] = {{Phase::Attack, MeterKind::Attack, "training.meter.attack"}, {Phase::Hit, MeterKind::Hit, "training.meter.hit"},
        {Phase::Guard, MeterKind::Guard, "training.meter.block"}, {Phase::Down, MeterKind::Down, "training.meter.knockdown"},
        {Phase::Movement, MeterKind::Movement, "training.meter.move"}, {Phase::Unknown, MeterKind::Sequence, "training.meter.throw"}};
    static const Named angledKinds[] = {{Phase::Attack, MeterKind::Startup, "training.startup"}, {Phase::Attack, MeterKind::Active, "training.meter.active"},
        {Phase::Attack, MeterKind::Recovery, "training.meter.recovery"}, {Phase::Attack, MeterKind::Attack, "training.meter.attack_unsplit"},
        {Phase::Hit, MeterKind::Hit, "training.meter.hit"}, {Phase::Guard, MeterKind::Guard, "training.meter.block"},
        {Phase::Down, MeterKind::Down, "training.meter.knockdown"}, {Phase::Movement, MeterKind::Movement, "training.meter.move"},
        {Phase::Unknown, MeterKind::Sequence, "training.meter.throw"}};
    std::vector<TrainingKeyEntry> entries;
    // In the bars' own colours: TrainingCellColor of a cell of that kind or phase.
    const auto add = [&](const Named& named) { MeterCell cell; cell.kind = named.kind; cell.phase = named.phase; entries.push_back({named.label, named.phase, named.kind, TrainingCellColor(cell, angled)}); };
    if (angled) for (const auto& named : angledKinds) add(named);
    else for (const auto& named : plain) add(named);
    return entries;
}
void DrawTrainingColorKey(bool angled) {
    const float h = ImGui::GetTextLineHeight();
    for (const auto& entry : TrainingColorKeyEntries(angled)) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + h * .2f), ImVec2(at.x + h * .6f, at.y + h * .8f), entry.color);
        ImGui::Dummy(ImVec2(h * .6f, h)); ImGui::SameLine(0, 4 * Scale());
        ImGui::TextUnformatted(loc::T(entry.label));
    }
    // Not a colour: the dotted line where hitstop was left out of the bars.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    StopLine(ImGui::GetWindowDrawList(), ImVec2(at.x + h * .15f, at.y + h * .2f), h * .6f, angled ? h * .21f : 0);
    ImGui::Dummy(ImVec2(h * .6f, h)); ImGui::SameLine(0, 4 * Scale());
    ImGui::TextUnformatted(loc::T("training.meter.hitstop"));
}
ImVec2 DrawFrameMeterWindow(const training::MeterView& meter, float hudScale, float width, float hudBottom,
    const training::MeterOptions& options, const std::function<void()>& above) {
    const auto* vp = ImGui::GetMainViewport();
    ImVec2 hudTop(vp->Pos.x + (vp->Size.x - width) / 2, hudBottom);
    // With every part off there is no window to draw.
    if (!above && !options.numbers && !options.bars && !options.state) return hudTop;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, hudBottom), ImGuiCond_Always, ImVec2(.5f, 1));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(TrainingHudBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * hudScale, 6 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin(FrameMeterWindow, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        if (above) above();
        if (options.numbers || options.bars) Meter(meter, hudScale, options);
        if (options.state) {
            ImGui::TextDisabled("%s", loc::Tf("training.frame_advantage",
                loc::T(meter.advantage.pending ? "training.measuring" : meter.advantage.knockdown ? "training.wakeup" : meter.frozen ? "training.held" : "training.live")).c_str());
            // The HUD is a NoInputs window, so IsItemHovered is always false here;
            // test the pointer against the item rectangle instead. The tooltip is
            // its own window and does not make the HUD capture input.
            if (AdvantageExplained(meter) && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false))
                ImGui::SetTooltip("%s", Unavailable(loc::T("training.advantage"), meter.advantage.unavailable).c_str());
            // Why a reading is missing, on the same line, so the HUD does not jump as measurements come and go.
            const auto reason=MeasurementReason(meter);
            if(!reason.empty()) { ImGui::SameLine(0, 10 * hudScale); ImGui::TextDisabled("%s", FitLabel(reason,ImGui::GetContentRegionAvail().x).c_str()); }
        }
        hudTop = ImGui::GetWindowPos();
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    return hudTop;
}
float FrameMeterHudScale(int size) {
    // Small is the size the HUD had before it could be chosen; standard sets its
    // text 22 pixels tall at 1080p, and large 28.
    constexpr float sizes[training::HudSizes] = {1.f, 1.3f, 1.6f};
    const auto* vp = ImGui::GetMainViewport();
    const float window = (std::min)(vp->Size.y / 900.f, vp->Size.x / 1200.f);
    return (std::max)(.65f, window * sizes[size] * InterfaceScale());
}
float TrainingMeterWidth(float hudScale) { return (std::min)(620 * hudScale, ImGui::GetMainViewport()->Size.x * .75f); }
} }
