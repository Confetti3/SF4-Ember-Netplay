#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
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
// What a bar cell and the F6 colour key both draw for a phase; idle is dimmer.
ImU32 BarColor(Phase phase) {
    return (PhaseColor(phase) & ~IM_COL32_A_MASK) | (static_cast<ImU32>(phase == Phase::Neutral ? 130 : 215) << IM_COL32_A_SHIFT);
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
void Meter(const MeterView& meter, float hudScale) {
    const float gap = 10 * hudScale;
    const auto measure=[&](const char* value){return ImGui::CalcTextSize(value).x;};
    // The startup column is sized from the translated text it will show.
    const float startup=(std::max)(measure(loc::Tf("training.start_frames",999).c_str()),measure(loc::T("training.start_unknown")));
    const float label = measure("P2") + gap + measure("+999 f") + gap + startup + gap;
    const float height = 12 * hudScale;
    const float width = (std::max)(120.f, ImGui::GetContentRegionAvail().x - label);
    const float cell = width / 120;
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
        ImGui::SameLine(rowStart + label);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(28, 29, 29, 100));
        for (int i = 0; i < 120; ++i) {
            const int index = MeterFrameIndex(meter.frames.size(), static_cast<std::size_t>(i));
            if (index >= 0) {
                const auto& frame = meter.frames[index];
                const auto& sample = frame.fighters[side];
                const auto phase = sample.valid ? ClassifyStatus(sample.status) : Phase::Unknown;
                const ImU32 color = BarColor(phase);
                draw->AddRectFilled(ImVec2(origin.x + i * cell, origin.y),
                    ImVec2(origin.x + (i + 1) * cell - (cell >= 3 ? 1.f : 0.f), origin.y + height), color);
                if (index > 0 && sample.valid && sample.action >= 0 && sample.action != meter.frames[index - 1].fighters[side].action)
                    draw->AddLine(ImVec2(origin.x + i * cell, origin.y), ImVec2(origin.x + i * cell, origin.y + height), palette::Ivory, 2);
            }
            if (i % 10 == 0) draw->AddLine(ImVec2(origin.x + i * cell, origin.y),
                ImVec2(origin.x + i * cell, origin.y + height), IM_COL32(243, 235, 221, 90));
        }
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
// The key lists the phases the bars draw, with the colour they draw them in.
// Idle has no entry: it is the dim gap between moves and has no label.
std::vector<TrainingKeyEntry> TrainingColorKeyEntries() {
    const std::pair<Phase, const char*> phases[] = {{Phase::Attack, "training.meter.attack"}, {Phase::Hit, "training.meter.hit"},
        {Phase::Guard, "training.meter.block"}, {Phase::Down, "training.meter.knockdown"}, {Phase::Movement, "training.meter.move"},
        {Phase::Unknown, "training.meter.throw"}};
    std::vector<TrainingKeyEntry> entries;
    for (const auto& phase : phases) entries.push_back({phase.second, phase.first, BarColor(phase.first)});
    return entries;
}
void DrawTrainingColorKey() {
    const float h = ImGui::GetTextLineHeight();
    for (const auto& entry : TrainingColorKeyEntries()) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + h * .2f), ImVec2(at.x + h * .6f, at.y + h * .8f), entry.color);
        ImGui::Dummy(ImVec2(h * .6f, h)); ImGui::SameLine(0, 4 * Scale());
        ImGui::TextUnformatted(loc::T(entry.label));
    }
}
// The game's own words for a fight request arriving, across the middle of
// the screen while the battle is about to be taken away.
void ChallengerBanner(const training::View& view) {
    if (view.leavingIn <= 0) return;
    const auto* vp = ImGui::GetMainViewport();
    auto* draw = ImGui::GetForegroundDrawList();
    const char* text = loc::T("training.challenger");
    const float size = vp->Size.y * .075f;
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
    const float y = vp->Pos.y + vp->Size.y * .42f, band = size * 1.8f;
    draw->AddRectFilled(ImVec2(vp->Pos.x, y - (band - extent.y) / 2), ImVec2(vp->Pos.x + vp->Size.x, y + (band + extent.y) / 2), IM_COL32(0, 0, 0, 170));
    const ImVec2 at(vp->Pos.x + (vp->Size.x - extent.x) / 2, y);
    // Lit and dim by turns, as the game's banner flashes.
    const ImU32 colour = (view.leavingIn / 8) % 2 ? IM_COL32(255, 214, 72, 255) : palette::Ember;
    for (const ImVec2 offset : {ImVec2(-2, 0), ImVec2(2, 0), ImVec2(0, -2), ImVec2(0, 2)})
        draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + offset.x, at.y + offset.y), IM_COL32(10, 10, 10, 255), text);
    draw->AddText(ImGui::GetFont(), size, at, colour, text);
}
// The meter's own window, centred, its bottom edge at hudBottom. Takes no
// input. Returns its top left corner.
static ImVec2 MeterWindow(const training::MeterView& meter, float hudScale, float width, float hudBottom) {
    const auto* vp = ImGui::GetMainViewport();
    ImVec2 hudTop(vp->Pos.x + (vp->Size.x - width) / 2, hudBottom);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, hudBottom), ImGuiCond_Always, ImVec2(.5f, 1));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * hudScale, 6 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training frame meter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        Meter(meter, hudScale);
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
    MeterWindow(view.meter, hudScale, (std::min)(620 * hudScale, vp->Size.x * .75f), vp->Pos.y + vp->Size.y * TrainingHudBottom);
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
TrainingHudInput DrawTrainingHud(const training::View& view) {
    if (!view.available) return {};
    const auto* vp = ImGui::GetMainViewport();
    // Size the passive HUD to the game viewport; menu/DPI scaling should not
    // turn it into a large panel over the fight.
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    const float width = (std::min)(620 * hudScale, vp->Size.x * .75f);
    // The game's super meters and their SUPER! banners start about 17% above
    // the bottom edge and scale with the height, so the meter sits just above them.
    const float hudBottom = vp->Pos.y + vp->Size.y * TrainingHudBottom;
    const ImVec2 hudTop = MeterWindow(view.meter, hudScale, width, hudBottom);
    // The one input this HUD takes: a chip that opens the controls for a
    // mouse, as F6 does from the keyboard. It captures the mouse only while
    // the pointer is over it, so the passive meter below never does.
    TrainingHudInput input;
    ImGui::SetNextWindowPos(ImVec2(hudTop.x, hudTop.y - 4 * hudScale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training shortcuts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        input.open = ImGui::SmallButton(loc::T("training.open_chip"));
        ReportMenuCard("training-open", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        bool failed = false;
        const auto notice = TrainingNotice(failed);
        const auto hint = notice.empty() ? loc::T("training.hide_hint") : FitLabel(notice, width - ImGui::GetCursorPosX() - 4 * hudScale);
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
