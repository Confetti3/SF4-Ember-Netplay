#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

// The frame meter and the passive HUD around it; the F6 controls are in
// TrainingPanel.cxx.
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
// A cell of the angled bars: ClassifyMeter's kind, except that holding back
// in a jump near an attack is a guard status with nothing guarded in the air.
MeterKind AngledKind(const MeterView& meter, int index, int side) {
    const auto& sample = meter.frames[index].fighters[side];
    const auto kind = ClassifyMeter(sample);
    if (kind != MeterKind::Guard || sample.status == ActorStatus::DamageGuard) return kind;
    int from = index;
    while (from > 0 && (meter.frames[from].fighters[side].status == ActorStatus::GuardStand ||
        meter.frames[from].fighters[side].status == ActorStatus::GuardCrouch)) --from;
    const unsigned before = meter.frames[from].fighters[side].status;
    return sample.posture > 1 || before == ActorStatus::Jump || before == ActorStatus::StandToJump ? MeterKind::Neutral : kind;
}
// The frames a bar shows: cells across it, and how many of the newest lie
// to its right while a held meter is scrolled back.
struct BarWindow { int cells; std::size_t back; };
// Counting marks, the same in both styles: a thin dark gap parts every cell
// (left by the cells themselves), every fifth boundary is a faint ivory line
// and every tenth a brighter one with a tick under the bar. slant leans them
// with angled cells. Quiet beside the cells' own colours.
void CountMarks(ImDrawList* draw, ImVec2 origin, float cell, float height, float slant, int cells) {
    for (int i = 5; i < cells; i += 5) {
        const bool tenth = i % 10 == 0;
        const float x = origin.x + i * cell - .5f;
        const ImU32 colour = tenth ? IM_COL32(243, 235, 221, 150) : IM_COL32(243, 235, 221, 70);
        draw->AddLine(ImVec2(x + slant, origin.y), ImVec2(x, origin.y + height), colour);
        draw->AddLine(ImVec2(x, origin.y + height), ImVec2(x, origin.y + height + (tenth ? 2.f : 1.f)), colour);
    }
}
// Stable's flat bars: a cell's colour is its state group, so an attack is one
// colour from start to end; a bright line marks a new native action.
void FlatBar(ImDrawList* draw, const MeterView& meter, int side, ImVec2 origin, float width, float height, BarWindow window) {
    const float cell = width / window.cells;
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(28, 29, 29, 100));
    for (int i = 0; i < window.cells; ++i) {
        const int index = MeterFrameIndex(meter.frames.size(), static_cast<std::size_t>(i), static_cast<std::size_t>(window.cells), window.back);
        if (index < 0) continue;
        const auto& sample = meter.frames[index].fighters[side];
        draw->AddRectFilled(ImVec2(origin.x + i * cell, origin.y),
            ImVec2(origin.x + (i + 1) * cell - (cell >= 3 ? 1.f : 0.f), origin.y + height), TrainingCellColor(sample, false));
        if (index > 0 && sample.valid && sample.action >= 0 && sample.action != meter.frames[index - 1].fighters[side].action)
            draw->AddLine(ImVec2(origin.x + i * cell, origin.y), ImVec2(origin.x + i * cell, origin.y + height), palette::Ivory, 2);
    }
    CountMarks(draw, origin, cell, height, 0, window.cells);
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
    const auto label = [&](int from, int to, MeterKind kind) {
        if (kind == MeterKind::Neutral || to - from < 2) return;
        const auto text = std::to_string(to - from);
        const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(digits, FLT_MAX, 0, text.c_str());
        if (extent.x + 2 > (to - from) * cell) return;
        Edged(draw, digits, ImVec2(origin.x + slant / 2 + (from + to) * cell / 2 - extent.x / 2, origin.y + (height - extent.y) / 2), palette::Ivory, text.c_str());
    };
    int runStart = -1; MeterKind runKind = MeterKind::Neutral;
    for (int i = 0; i < window.cells; ++i) {
        const int index = MeterFrameIndex(meter.frames.size(), static_cast<std::size_t>(i), static_cast<std::size_t>(window.cells), window.back);
        if (index < 0) continue;
        const auto& sample = meter.frames[index].fighters[side];
        const auto kind = AngledKind(meter, index, side);
        quad(i * cell, (i + 1) * cell - (cell >= 3 ? 1.f : 0.f), AngledColor(kind));
        const auto* previous = index > 0 ? &meter.frames[index - 1].fighters[side] : nullptr;
        // Guarding is one stretch whatever plays in it: the guard pose, then each blocked hit's own reaction.
        const bool action = runStart >= 0 && previous && sample.valid && sample.action >= 0 && sample.action != previous->action &&
            !(kind == MeterKind::Guard && runKind == MeterKind::Guard);
        const bool struck = runStart >= 0 && previous && kind == MeterKind::Hit && runKind == MeterKind::Hit && !action &&
            (sample.comboDamage > previous->comboDamage || sample.actionFrame < previous->actionFrame);
        if ((action || struck) && kind == runKind && kind != MeterKind::Neutral) quad(i * cell - 1, i * cell + 1, IM_COL32(10, 10, 10, 255));
        if (runStart < 0) { runStart = i; runKind = kind; }
        else if (kind != runKind || action || struck) { label(runStart, i, runKind); runStart = i; runKind = kind; }
    }
    if (runStart >= 0) label(runStart, window.cells, runKind);
    CountMarks(draw, origin, cell, height, slant, window.cells);
}
ImVec4 AdvantageColor(int frames) {
    return ImGui::ColorConvertU32ToFloat4(frames > 0 ? IM_COL32(118, 224, 160, 255) :
        frames < 0 ? IM_COL32(255, 121, 129, 255) : palette::Ivory);
}
void Advantage(const FrameAdvantage& advantage, int side) {
    if (advantage.valid) ImGui::TextColored(AdvantageColor(advantage.frames[side]), "%+d f", advantage.frames[side]);
    else ImGui::TextDisabled("--");
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
void Meter(const MeterView& meter, float hudScale, const MeterOptions& options) {
    const float gap = 10 * hudScale;
    const auto measure=[&](const char* value){return ImGui::CalcTextSize(value).x;};
    // The startup and recovery columns are sized from the translated text they will show.
    const float startup=(std::max)(measure(loc::Tf("training.start_frames",999).c_str()),measure(loc::T("training.start_unknown")));
    const float recovery=options.recovery?(std::max)(measure(loc::Tf("training.recovery_frames",999).c_str()),measure(loc::T("training.recovery_unknown")))+gap:0;
    const float label = measure("P2") + gap + measure("+999 f") + gap + startup + gap + recovery;
    const float height = 12 * hudScale;
    const float width = (std::max)(120.f, ImGui::GetContentRegionAvail().x - label);
    // A held meter scrolls back through the frames it keeps with the mouse
    // wheel over the bars, ten frames a notch; it shows the newest again once
    // it runs. The newest frame is always at the right.
    static std::size_t back = 0;
    BarWindow window{options.shown, 0};
    if (!meter.frozen) back = 0;
    else {
        const ImVec2 bars(ImGui::GetCursorScreenPos().x + label, ImGui::GetCursorScreenPos().y);
        const float rows = 2 * ((std::max)(height, ImGui::GetTextLineHeight()) + ImGui::GetStyle().ItemSpacing.y);
        const int notches = static_cast<int>(ImGui::GetIO().MouseWheel);
        if (notches && ImGui::IsMouseHoveringRect(bars, ImVec2(bars.x + width, bars.y + rows), false))
            back = notches > 0 ? back + 10 * notches : back - (std::min)(back, static_cast<std::size_t>(-10 * notches));
        back = (std::min)(back, MeterBackLimit(meter.frames.size(), static_cast<std::size_t>(window.cells)));
        window.back = back;
    }
    for (int side = 0; side < 2; ++side) {
        const float rowStart = ImGui::GetCursorPosX();
        ImGui::Text("P%d", side + 1);
        ImGui::SameLine(0,gap); Advantage(meter.advantage, side);
        ImGui::SameLine(0,gap);
        if (meter.startupFrames[side] >= 0) ImGui::TextUnformatted(loc::Tf("training.start_frames", meter.startupFrames[side]).c_str());
        else {
            ImGui::TextDisabled("%s", loc::T("training.start_unknown"));
            if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false))
                ImGui::SetTooltip("%s", Unavailable(loc::T("training.startup"), meter.startupUnavailable[side]).c_str());
        }
        // The last move's recovery, as Frame data counts it, once it is over.
        if (options.recovery) {
            const auto& move = meter.moves[side];
            ImGui::SameLine(0,gap);
            if (move.seen && !move.live && move.recovery > 0) ImGui::TextUnformatted(loc::Tf("training.recovery_frames", move.recovery).c_str());
            else ImGui::TextDisabled("%s", loc::T("training.recovery_unknown"));
        }
        ImGui::SameLine(rowStart + label);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (options.flat) FlatBar(ImGui::GetWindowDrawList(), meter, side, origin, width, height, window);
        else AngledBar(ImGui::GetWindowDrawList(), meter, side, origin, width, height, window);
        ReportMenuCard(side ? "training-bar-2" : "training-bar-1", origin, ImVec2(origin.x + width, origin.y + height));
        ImGui::Dummy(ImVec2(width, height));
    }
}
// The first measurement that is unavailable, and why, for the HUD's reason
// line: the pad and keys cannot hover the tooltips that say the same.
std::string MeasurementReason(const MeterView& meter) {
    if(!meter.advantage.valid)return Unavailable(loc::T("training.advantage"),meter.advantage.unavailable);
    for(int side=0;side<2;++side)if(meter.startupFrames[side]<0)
        return "P"+std::to_string(side+1)+" "+Unavailable(loc::T("training.startup"),meter.startupUnavailable[side]);
    return {};
}
struct Reading { std::string label, value; };
std::vector<Reading> Readings(const MeterView& meter, int side) {
    const auto& move = meter.moves[side];
    const auto number = [](int value) { return value > 0 ? std::to_string(value) : std::string("--"); };
    std::vector<Reading> readings{
        {loc::T("training.startup"), number(meter.startupFrames[side])},
        {loc::T("training.meter.active"), number(move.seen ? move.active : 0)},
        {loc::T("training.meter.recovery"), number(move.seen && !move.live ? move.recovery : 0)}};
    // What the move met, and the frames it left its user ahead or behind.
    const auto& advantage = meter.advantage;
    const char* met = advantage.attacker == side ? (advantage.blocked ? "training.meter.block" : "training.meter.hit") :
        advantage.attacker < 0 && !advantage.pending && move.seen && !move.live ? "training.meter.whiff" : nullptr;
    char frames[16] = "--";
    if (advantage.valid) std::snprintf(frames, sizeof frames, "%+d", advantage.frames[side]);
    if (meter.meatyValid[side]) {
        const int offset = meter.meatyFrames[side];
        const bool landed = offset <= 0 && -offset < (std::max)(1, move.active);
        char timing[16]; std::snprintf(timing, sizeof timing, landed ? "%d" : "%+d", landed ? -offset : offset);
        readings.push_back({loc::T("training.meter.meaty"), timing});
    }
    if (met) readings.push_back({loc::T("training.result"), std::string(loc::T(met)) +
        (std::string(met) == "training.meter.whiff" ? "" : " " + std::string(frames))});
    else readings.push_back({loc::T("training.advantage"), frames});
    return readings;
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
unsigned TrainingCellColor(const training::FighterSample& sample, bool angled) {
    if (!angled) return BarColor(sample.valid ? ClassifyStatus(sample.status) : Phase::Unknown);
    return AngledColor(ClassifyMeter(sample));
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
    if (angled) for (const auto& named : angledKinds) entries.push_back({named.label, named.phase, named.kind, AngledColor(named.kind)});
    else for (const auto& named : plain) entries.push_back({named.label, named.phase, named.kind, BarColor(named.phase)});
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
}
// The match is ready: across the middle of the screen while the battle is
// about to be taken away, while the call stands. Under it, smaller: who sat
// down, and go now when it can be pressed.
void ChallengerBanner(const training::View& view, const ChallengerCall& call) {
    if (view.leavingIn <= 0 || !call.called) return;
    const auto* vp = ImGui::GetMainViewport();
    auto* draw = ImGui::GetForegroundDrawList();
    auto* font = ImGui::GetFont();
    const char* text = loc::T("training.challenger");
    const float size = vp->Size.y * .075f;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    const float small = size * .4f, glyph = small * 1.4f, gap = small * 1.5f;
    const char* goNow = call.goNowGlyph ? loc::T("training.go_now") : nullptr;
    const float goWidth = goNow ? glyph + small * .35f + font->CalcTextSizeA(small, FLT_MAX, 0, goNow).x : 0;
    if (goNow) ReportMenuText("challenger-go-now", small, glyph, goWidth, vp->Size.x * .9f);
    std::string who = call.opponent;
    if (const auto* fighter = who.empty() ? nullptr : selection::FindFighter(call.fighter)) who += "  \xC2\xB7  " + std::string(fighter->name);
    // Only the name gives way, so go now is never pushed off the screen.
    const float whoRoom = vp->Size.x * .9f - goWidth - (goNow ? gap : 0);
    if (!who.empty() && font->CalcTextSizeA(small, FLT_MAX, 0, who.c_str()).x > whoRoom) {
        const char* end = nullptr;
        font->CalcTextSizeA(small, (std::max)(1.f, whoRoom - font->CalcTextSizeA(small, FLT_MAX, 0, "...").x), 0, who.c_str(), nullptr, &end);
        who = std::string(who.c_str(), end) + "...";
    }
    const float whoWidth = who.empty() ? 0 : font->CalcTextSizeA(small, FLT_MAX, 0, who.c_str()).x;
    const bool second = !who.empty() || goNow;
    const float y = vp->Pos.y + vp->Size.y * .42f, band = size * 1.8f, below = second ? glyph + size * .3f : 0;
    draw->AddRectFilled(ImVec2(vp->Pos.x, y - (band - extent.y) / 2), ImVec2(vp->Pos.x + vp->Size.x, y + (band + extent.y) / 2 + below), IM_COL32(0, 0, 0, 170));
    const ImVec2 at(vp->Pos.x + (vp->Size.x - extent.x) / 2, y);
    // Lit and dim by turns, as the game's banner flashes.
    const ImU32 colour = (view.leavingIn / 8) % 2 ? IM_COL32(255, 214, 72, 255) : palette::Ember;
    for (const ImVec2 offset : {ImVec2(-2, 0), ImVec2(2, 0), ImVec2(0, -2), ImVec2(0, 2)})
        draw->AddText(font, size, ImVec2(at.x + offset.x, at.y + offset.y), IM_COL32(10, 10, 10, 255), text);
    draw->AddText(font, size, at, colour, text);
    if (!second) return;
    const float lineY = y + extent.y + size * .25f;
    float x = vp->Pos.x + (vp->Size.x - whoWidth - goWidth - (whoWidth && goWidth ? gap : 0)) / 2;
    if (!who.empty()) {
        NoteUserText(call.opponent);
        draw->AddText(font, small, ImVec2(x, lineY + (glyph - small) / 2), palette::Ivory, who.c_str());
        x += whoWidth + gap;
    }
    if (goNow) {
        DrawPromptGlyph(draw, call.goNowGlyph, ImVec2(x, lineY), glyph, MenuArt(), glyph / 32);
        draw->AddText(font, small, ImVec2(x + glyph + small * .35f, lineY + (glyph - small) / 2), palette::Ivory, goNow);
    }
}
void DrawTrainingRoomStatus(const TrainingRoomStatus& status) {
    if (status.Empty()) return;
    const auto* vp = ImGui::GetMainViewport();
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y * TrainingRoomStatusTop), ImGuiCond_Always, ImVec2(.5f, 0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training room status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        // The players' names give way so the line stays within two fifths of
        // the screen, clear of the gauges at the sides. Ember's own words keep
        // their room; the two names share what is left: half each, and either
        // takes what the other does not need. Each keeps at least a letter and
        // "...", so where Ember's words alone fill that room (a narrow screen,
        // a long translation) the line is those words and the two shortest
        // names; the render test holds every translation within nine tenths of
        // the screen.
        const auto width = [](const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x; };
        const bool hasRoom = !status.room.empty(), hasOpponent = !status.opponent.empty();
        const std::string mark = "W";
        const float fixed = width(TrainingRoomLine(status, hasRoom ? mark : std::string(), mark)) - (hasRoom ? width(mark) : 0) - (hasOpponent ? width(mark) : 0);
        const float spare = (std::max)(0.f, vp->Size.x * .4f - fixed), least = width("W...");
        const float roomWidth = hasRoom ? width(status.room) : 0, opponentWidth = hasOpponent ? width(status.opponent) : 0;
        const float half = hasRoom && hasOpponent ? spare / 2 : spare;
        const float roomFits = (std::min)(roomWidth, (std::max)(half, spare - (std::min)(opponentWidth, half)));
        const float opponentFits = (std::min)(opponentWidth, spare - roomFits);
        const std::string line = TrainingRoomLine(status, hasRoom ? FitLabel(status.room, (std::max)(least, roomFits)) : std::string(),
            hasOpponent ? FitLabel(status.opponent, (std::max)(least, opponentFits)) : std::string());
        ReportMenuText("training-room-line", ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight(), width(line), vp->Size.x * .9f);
        NoteUserText(line);
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
// The meter's own window, centred, its bottom edge at hudBottom. Takes no
// input. Returns its top left corner.
static ImVec2 MeterWindow(const training::MeterView& meter, float hudScale, float width, float hudBottom, const training::MeterOptions& options) {
    const auto* vp = ImGui::GetMainViewport();
    ImVec2 hudTop(vp->Pos.x + (vp->Size.x - width) / 2, hudBottom);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, hudBottom), ImGuiCond_Always, ImVec2(.5f, 1));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * hudScale, 6 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin(FrameMeterWindow, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        Meter(meter, hudScale, options);
        ImGui::TextDisabled("%s", loc::Tf("training.frame_advantage",
            loc::T(meter.advantage.pending ? "training.measuring" : meter.advantage.knockdown ? "training.wakeup" : meter.frozen ? "training.held" : "training.live")).c_str());
        // The HUD is a NoInputs window, so IsItemHovered is always false here;
        // test the pointer against the item rectangle instead. The tooltip is
        // its own window and does not make the HUD capture input.
        if (!meter.advantage.valid && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false))
            ImGui::SetTooltip("%s", Unavailable(loc::T("training.advantage"), meter.advantage.unavailable).c_str());
        // Always one line, so the HUD does not jump as measurements come and go.
        const auto reason=MeasurementReason(meter);
        ImGui::TextDisabled("%s", FitLabel(reason.empty()?" ":reason,ImGui::GetContentRegionAvail().x).c_str());
        hudTop = ImGui::GetWindowPos();
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    return hudTop;
}
// The frame meter alone, over a rollback match.
void DrawMatchMeter(const training::View& view) {
    if (!view.watching) return;
    const auto* vp = ImGui::GetMainViewport();
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    MeterWindow(view.meter, hudScale, (std::min)(620 * hudScale, vp->Size.x * .75f), vp->Pos.y + vp->Size.y * TrainingHudBottom, TrainingMeterOptions());
}
// A Training table's match HUD explains why shared position controls are unavailable.
void DrawMatchPracticeNotice() {
    const auto* vp=ImGui::GetMainViewport();
    const float hudScale=(std::max)(1.f,(std::min)(1.5f,vp->Size.y/900.f));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x/2,vp->Pos.y+vp->Size.y*TrainingHudBottom+4*hudScale),ImGuiCond_Always,ImVec2(.5f,0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(4*hudScale,3*hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);
    if(ImGui::Begin("Match practice keys",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f*hudScale/Scale());
        ImGui::TextDisabled("%s",loc::T("training.match.checkpoint_disabled"));
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
TrainingHudInput DrawTrainingHud(const training::View& view, bool called) {
    if (!view.available) return {};
    const auto* vp = ImGui::GetMainViewport();
    // Size the passive HUD to the game viewport; menu/DPI scaling should not
    // turn it into a large panel over the fight.
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    const float width = (std::min)(620 * hudScale, vp->Size.x * .75f);
    // The game's super meters and their SUPER! banners start about 17% above
    // the bottom edge and scale with the height, so the meter sits just above them.
    const float hudBottom = vp->Pos.y + vp->Size.y * TrainingHudBottom;
    const ImVec2 hudTop = MeterWindow(view.meter, hudScale, width, hudBottom, TrainingMeterOptions());
    // The one input this HUD takes: a chip that opens the controls for a
    // mouse, as F6 does from the keyboard. It captures the mouse only while
    // the pointer is over it, so the passive meter below never does. After a
    // pad press it names the pad's buttons instead: Back and Start open the
    // controls, and Back alone resets or saves the position.
    TrainingHudInput input;
    // Under the call back from a room the controls cannot open, so the chip
    // that opens them and its pad hints would be wrong; the call's banner
    // says what can be pressed then.
    if (called) return input;
    ImGui::SetNextWindowPos(ImVec2(hudTop.x, hudTop.y - 4 * hudScale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training shortcuts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        const int device = MenuPromptDevice();
        const bool pad = device == sf4e::input::PadXInput || device == sf4e::input::PadDirectInput, xbox = device == sf4e::input::PadXInput;
        // An Xbox pad's buttons as its prompt art; a DirectInput pad's labels are unknown, so they are named.
        const auto back = [&] { if (xbox) PromptGlyph("View", ImGui::GetTextLineHeight()); else ImGui::TextUnformatted("Select"); };
        if (pad) {
            back(); ImGui::SameLine(0, 2 * hudScale); ImGui::TextUnformatted("+"); ImGui::SameLine(0, 2 * hudScale);
            if (xbox) PromptGlyph("Start", ImGui::GetTextLineHeight()); else ImGui::TextUnformatted("Start");
            ImGui::SameLine(0, 4 * hudScale);
            input.open = ImGui::SmallButton(loc::T("training.controls_chip"));
        } else input.open = ImGui::SmallButton(loc::T("training.open_chip"));
        ReportMenuCard("training-open", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        bool failed = false;
        const auto notice = TrainingNotice(failed);
        if (pad && notice.empty()) { back(); ImGui::SameLine(0, 4 * hudScale); }
        const auto hint = !notice.empty() ? FitLabel(notice, width - ImGui::GetCursorPosX() - 4 * hudScale) :
            std::string(loc::T(pad ? "training.pad_hint" : "training.hide_hint"));
        if (failed && !notice.empty()) ImGui::TextColored(AdvantageColor(-1), "%s", hint.c_str());
        else ImGui::TextDisabled("%s", hint.c_str());
        ImGui::SetWindowFontScale(1.f);
        input.pointer = ImGui::IsWindowHovered();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return input;
}
} }
