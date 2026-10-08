#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "InputGlyphs.hxx"
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
// The colours Street Fighter 6's frame meter taught: green startup, red
// active, blue recovery, yellow for the time spent hit or blocking.
ImU32 KindColor(MeterKind kind) {
    switch (kind) {
    case MeterKind::Neutral: return IM_COL32(70, 74, 78, 255);
    case MeterKind::Movement: return IM_COL32(120, 150, 160, 255);
    case MeterKind::Startup: return IM_COL32(60, 196, 110, 255);
    case MeterKind::Active: return IM_COL32(236, 56, 96, 255);
    case MeterKind::Recovery: return IM_COL32(60, 120, 232, 255);
    case MeterKind::Attack: return palette::Ember;
    case MeterKind::Guard: return IM_COL32(244, 222, 96, 255);
    case MeterKind::Hit: return IM_COL32(246, 176, 48, 255);
    case MeterKind::Down: return IM_COL32(150, 92, 196, 255);
    case MeterKind::Rise: return IM_COL32(216, 184, 240, 255);
    case MeterKind::Sequence: return IM_COL32(220, 206, 166, 255);
    case MeterKind::Meaty: return IM_COL32(255, 72, 214, 255);
    default: return IM_COL32(48, 50, 54, 255);
    }
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
// Text with a dark edge, to read on any fill.
void Edged(ImDrawList* draw, float size, ImVec2 at, ImU32 colour, const char* text) {
    for (const ImVec2 offset : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1)})
        draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + offset.x, at.y + offset.y), IM_COL32(10, 10, 10, 230), text);
    draw->AddText(ImGui::GetFont(), size, at, colour, text);
}
// A fighter's last move as the data line shows it: the label in the game's
// orange, the number in white after it.
struct Reading { std::string label, value; ImU32 colour; bool toned; };
std::vector<Reading> Readings(const MeterView& meter, int side) {
    const auto& move = meter.moves[side];
    const auto number = [](int value) { return value > 0 ? std::to_string(value) : std::string("--"); };
    std::vector<Reading> readings{
        {loc::T("training.startup"), number(meter.startupFrames[side]), KindColor(MeterKind::Startup), false},
        {loc::T("training.meter.active"), number(move.seen ? move.active : 0), KindColor(MeterKind::Active), false},
        {loc::T("training.meter.recovery"), number(move.seen && !move.live ? move.recovery : 0), KindColor(MeterKind::Recovery), false}};
    // What the move met, and the frames it left its user ahead or behind.
    const auto& advantage = meter.advantage;
    const char* met = advantage.attacker == side ? (advantage.blocked ? "training.meter.block" : "training.meter.hit") :
        advantage.attacker < 0 && !advantage.pending && move.seen && !move.live ? "training.meter.whiff" : nullptr;
    char frames[16] = "--";
    if (advantage.valid) std::snprintf(frames, sizeof frames, "%+d", advantage.frames[side]);
    const ImU32 tone = !advantage.valid ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : advantage.frames[side] > 0 ? IM_COL32(118, 224, 160, 255) :
        advantage.frames[side] < 0 ? IM_COL32(255, 121, 129, 255) : palette::Ivory;
    if (meter.meatyValid[side]) {
        // A meaty, in pink: how many of the attack's active frames had passed
        // when the other got up, 0 for its first; the more, the meatier. Not
        // one, in red and signed: -N ended N frames too early to be active
        // then, +N became active N frames after.
        const int offset = meter.meatyFrames[side];
        const bool landed = offset <= 0 && -offset < (std::max)(1, move.active);
        char timing[16]; std::snprintf(timing, sizeof timing, landed ? "%d" : "%+d", landed ? -offset : offset);
        readings.push_back({loc::T("training.meter.meaty"), timing, landed ? KindColor(MeterKind::Meaty) : IM_COL32(255, 121, 129, 255), true});
    }
    if (met && std::string(met) == "training.meter.whiff") readings.push_back({loc::T(met), "", palette::Ivory, false});
    else readings.push_back({met ? loc::T(met) : loc::T("training.advantage"), frames, tone, true});
    return readings;
}
// The frame meter in the manner of Street Fighter 6's, dressed as this game
// dresses its own panels: slanted, black, edged in orange. A data line gives
// each fighter's last move as startup, active and recovery frames and what it
// was worth on hit or block; under it one bar a fighter, a cell a frame, each
// run of a state carrying its length.
void Meter(const MeterView& meter, float hudScale) {
    const float full = ImGui::GetContentRegionAvail().x, half = full / 2;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    // The game's own trim: a bar that runs from orange into yellow.
    draw->AddRectFilledMultiColor(ImVec2(top.x, top.y - 4 * hudScale), ImVec2(top.x + full, top.y - 2 * hudScale),
        palette::Ember, IM_COL32(255, 214, 72, 255), IM_COL32(255, 214, 72, 255), palette::Ember);
    ImGui::SetWindowFontScale(hudScale / Scale());
    const float line = ImGui::GetTextLineHeight(), small = line * .72f, gap = 8 * hudScale;
    for (int side = 0; side < 2; ++side) {
        const auto readings = Readings(meter, side);
        const char tag[3] = {'P', static_cast<char>('1' + side), 0};
        // Labels where there is room for them, numbers alone where there is not.
        float labelled = ImGui::CalcTextSize(tag).x + gap;
        for (const auto& reading : readings) labelled += ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, reading.label.c_str()).x + 4 * hudScale + ImGui::CalcTextSize(reading.value.c_str()).x + gap;
        const bool labels = labelled <= half;
        float x = top.x + side * half;
        Edged(draw, line, ImVec2(x, top.y), side ? IM_COL32(120, 190, 255, 255) : IM_COL32(255, 120, 110, 255), tag);
        x += ImGui::CalcTextSize(tag).x + gap;
        for (std::size_t i = 0; i < readings.size(); ++i) {
            const auto& reading = readings[i];
            // The last reading always names itself: hit, block or whiff is the news.
            if (labels || i + 1 == readings.size()) {
                draw->AddText(ImGui::GetFont(), small, ImVec2(x, top.y + line - small - 1), palette::Ember, reading.label.c_str());
                x += ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, reading.label.c_str()).x + 4 * hudScale;
            } else {
                draw->AddRectFilled(ImVec2(x, top.y + line * .25f), ImVec2(x + 3 * hudScale, top.y + line * .85f), reading.colour);
                x += 5 * hudScale;
            }
            Edged(draw, line, ImVec2(x, top.y), i + 1 == readings.size() || reading.toned ? reading.colour : IM_COL32(255, 255, 255, 255), reading.value.c_str());
            x += ImGui::CalcTextSize(reading.value.c_str()).x + gap;
        }
    }
    ImGui::Dummy(ImVec2(full, line));
    ImGui::SetWindowFontScale(.8f * hudScale / Scale());
    const float height = 16 * hudScale, slant = height * .35f;
    const float width = (std::max)(120.f, full - slant), cell = width / 120, digits = height * .82f;
    // A held meter scrolls back through the exchange with the mouse wheel over
    // the bars, ten frames a notch; it shows the newest frames again once it runs.
    static int back = 0;
    const int kept = static_cast<int>(meter.frames.size()), shownMost = static_cast<int>(MeterShown);
    if (!meter.frozen) back = 0;
    else {
        const ImVec2 bars = ImGui::GetCursorScreenPos();
        if (ImGui::IsMouseHoveringRect(bars, ImVec2(bars.x + full, bars.y + 2 * height + ImGui::GetStyle().ItemSpacing.y), false))
            back += static_cast<int>(ImGui::GetIO().MouseWheel * 10);
    }
    back = (std::max)(0, (std::min)(back, kept - shownMost));
    const int first = (std::max)(0, kept - shownMost - back);
    // A meaty is an attack whose active frames are already out as the other
    // gets up. The active frames that passed before it met them are the
    // meaty frames: each is a frame of advantage the attack gains over
    // hitting with its first one, and they show so on the attack's own bar.
    // From the frame it meets them on it is active as any attack is.
    // ponytail: found by walking the kept frames each draw, at most MeterHistory; mark it in the meter if that is ever felt.
    std::vector<char> meaty[2];
    for (int side = 0; side < 2; ++side) {
        meaty[side].assign(static_cast<std::size_t>(kept), 0);
        const auto active = [&](int at) { return ClassifyMeter(meter.frames[at].fighters[side]) == MeterKind::Active; };
        for (int at = 1; at < kept; ++at) {
            // The other is first seen up at `at`: the frame it could be hit on is the one before.
            if (!meter.frames[at].fighters[1 - side].wake || !active(at - 1)) continue;
            for (int run = at - 2; run >= 0 && active(run); --run) meaty[side][run] = 1;
        }
    }
    // What was pressed, on a lane of its own beside each bar: Player 1's over
    // their bar, Player 2's under theirs, so the two bars stay together. A
    // direction shows as it changes and a button as it goes down, at the frame
    // it did; presses too close to draw apart are moved right, in order.
    const float lane = 13 * hudScale;
    const auto inputs = [&](int side) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const int shown = (std::min)(shownMost, kept - first);
        float cursor = origin.x;
        unsigned before = first > 0 ? meter.frames[first - 1].fighters[side].input : 0;
        static const unsigned masks[] = {0x10, 0x20, 0x400, 0x40, 0x80, 0x800};
        static const ImU32 colours[] = {IM_COL32(96, 176, 255, 255), IM_COL32(255, 214, 72, 255), IM_COL32(255, 96, 96, 255)};
        const auto place = [&](float at) -> float {
            // Not more than a few cells late: a press shown far from its frame would mislead.
            const float x = (std::max)(at, cursor);
            return x - at > 6 * cell ? -1.f : x;
        };
        for (int i = 0; i < shown; ++i) {
            const unsigned now = meter.frames[first + i].fighters[side].input;
            const float at = origin.x + slant / 2 + i * cell;
            const unsigned way = now & 0xF;
            if (way != (before & 0xF) && way) {
                // Numpad digit of the direction held; opposite directions held together cancel.
                const int x = ((way & 8) ? 1 : 0) - ((way & 4) ? 1 : 0), y = ((way & 1) ? 1 : 0) - ((way & 2) ? 1 : 0);
                const char digit = static_cast<char>('5' + x + 3 * y);
                const float spot = place(at);
                if (digit != '5' && spot >= 0) cursor = spot + glyphs::Arrow(draw, ImVec2(spot, origin.y), lane, digit, false, IM_COL32(235, 235, 235, 255));
            }
            for (int button = 0; button < 6; ++button) {
                if (!(now & masks[button]) || (before & masks[button])) continue;
                const float spot = place(at);
                if (spot >= 0) cursor = spot + glyphs::Disc(draw, ImVec2(spot, origin.y), lane, button < 3 ? 'P' : 'K', colours[button % 3], IM_COL32(255, 255, 255, 255));
            }
            before = now;
        }
        ImGui::Dummy(ImVec2(full, lane));
    };
    for (int side = 0; side < 2; ++side) {
        if (side == 0) inputs(0);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // A cell leans as the game's gauges do: its top edge sits further right.
        const auto quad = [&](float from, float to, ImU32 colour) {
            draw->AddQuadFilled(ImVec2(origin.x + from + slant, origin.y), ImVec2(origin.x + to + slant, origin.y),
                ImVec2(origin.x + to, origin.y + height), ImVec2(origin.x + from, origin.y + height), colour);
        };
        quad(0, width, IM_COL32(12, 12, 14, 200));
        const int count = (std::min)(shownMost, kept - first);
        // A run's length, centred on it when it fits.
        const auto label = [&](int from, int to, MeterKind kind) {
            if (kind == MeterKind::Neutral || to - from < 2) return;
            const auto text = std::to_string(to - from);
            const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(digits, FLT_MAX, 0, text.c_str());
            if (extent.x + 2 > (to - from) * cell) return;
            Edged(draw, digits, ImVec2(origin.x + slant / 2 + (from + to) * cell / 2 - extent.x / 2, origin.y + (height - extent.y) / 2), IM_COL32(255, 255, 255, 255), text.c_str());
        };
        int runStart = 0; MeterKind runKind = MeterKind::Neutral;
        for (int i = 0; i < 120; ++i) {
            if (i >= count) { quad(i * cell, (i + 1) * cell - 1, IM_COL32(48, 50, 54, 120)); continue; }
            const auto& sample = meter.frames[first + i].fighters[side];
            auto kind = ClassifyMeter(sample);
            // The frame a fighter can first be hit after a knockdown, one before it is seen up, and the
            // other's active frames that had passed by then.
            if ((first + i + 1 < kept && meter.frames[first + i + 1].fighters[side].wake) || meaty[side][first + i]) kind = MeterKind::Meaty;
            // Holding back in a jump near an attack puts the fighter in a guard status, and nothing is guarded in the air.
            if (kind == MeterKind::Guard && sample.status != 22) {
                int from = first + i;
                while (from > 0 && (meter.frames[from].fighters[side].status == 14 || meter.frames[from].fighters[side].status == 15)) --from;
                const unsigned before = meter.frames[from].fighters[side].status;
                if (sample.posture > 1 || before == 2 || before == 5) kind = MeterKind::Neutral;
            }
            quad(i * cell, (i + 1) * cell - (cell >= 3 ? 1.f : 0.f), (KindColor(kind) & ~IM_COL32_A_MASK) | (static_cast<ImU32>(kind == MeterKind::Neutral ? 150 : 245) << IM_COL32_A_SHIFT));
            // A new action inside a run (a cancel, the next hit of a string) starts a new count.
            // Guarding is one stretch whatever plays in it: the guard pose, then each blocked hit's own reaction.
            const bool action = i > 0 && sample.valid && sample.action >= 0 && sample.action != meter.frames[first + i - 1].fighters[side].action &&
                !(kind == MeterKind::Guard && runKind == MeterKind::Guard);
            // Each hit of a combo is its own stretch of being hit: a new one starts where the damage of the combo grows
            // or the reaction plays again, and a dark line parts it from the one before.
            const bool struck = i > 0 && kind == MeterKind::Hit && runKind == MeterKind::Hit && !action &&
                (sample.comboDamage > meter.frames[first + i - 1].fighters[side].comboDamage || sample.actionFrame < meter.frames[first + i - 1].fighters[side].actionFrame);
            if (struck || (action && kind == MeterKind::Hit && runKind == MeterKind::Hit)) quad(i * cell - 1, i * cell + 1, IM_COL32(12, 12, 14, 255));
            if (i == 0) runKind = kind;
            else if (kind != runKind || action || struck) { label(runStart, i, runKind); runStart = i; runKind = kind; }
        }
        if (count) label(runStart, count, runKind);
        // Earlier frames lie to the left, later ones to the right, of what is shown.
        if (first > 0) Edged(draw, digits, ImVec2(origin.x - digits * .1f, origin.y + (height - digits) / 2), IM_COL32(255, 214, 72, 255), "<");
        if (back > 0) Edged(draw, digits, ImVec2(origin.x + width + slant - digits * .5f, origin.y + (height - digits) / 2), IM_COL32(255, 214, 72, 255), ">");
        // Every tenth frame, under the bar.
        for (int i = 10; i < 120; i += 10) draw->AddLine(ImVec2(origin.x + i * cell, origin.y + height), ImVec2(origin.x + i * cell, origin.y + height + 2 * hudScale), IM_COL32(255, 214, 72, 200));
        ImGui::Dummy(ImVec2(full, height));
        if (side == 1) inputs(1);
    }
}
// What the colours mean and what the meter is doing, where the reason line has nothing to say.
void MeterLegend(const MeterView& meter, float hudScale) {
    const float h = ImGui::GetTextLineHeight(), left = ImGui::GetCursorPosX(), full = ImGui::GetContentRegionAvail().x;
    const std::pair<MeterKind, const char*> entries[] = {{MeterKind::Startup, "training.startup"}, {MeterKind::Active, "training.meter.active"},
        {MeterKind::Recovery, "training.meter.recovery"}, {MeterKind::Hit, "training.meter.hit"}, {MeterKind::Guard, "training.meter.block"},
        {MeterKind::Down, "training.meter.knockdown"}, {MeterKind::Rise, "training.meter.wakeup"}, {MeterKind::Meaty, "training.meter.meaty"}, {MeterKind::Sequence, "training.meter.throw"}, {MeterKind::Attack, "training.meter.attack"},
        {MeterKind::Movement, "training.meter.move"}};
    // The last three are named only while the bars show them.
    bool shown[static_cast<int>(MeterKind::Unknown) + 1] = {};
    for (const auto& frame : meter.frames) for (const auto& fighter : frame.fighters) shown[static_cast<int>(ClassifyMeter(fighter))] = true;
    bool following = false;
    for (const auto& entry : entries) {
        if (entry.first != MeterKind::Startup && entry.first != MeterKind::Active && entry.first != MeterKind::Recovery &&
            entry.first != MeterKind::Hit && entry.first != MeterKind::Guard && entry.first != MeterKind::Down && entry.first != MeterKind::Rise && entry.first != MeterKind::Meaty &&
            !shown[static_cast<int>(entry.first)]) continue;
        if (following) ImGui::SameLine(0, 10 * hudScale);
        following = true;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + h * .2f), ImVec2(at.x + h * .6f, at.y + h * .8f), KindColor(entry.first));
        ImGui::Dummy(ImVec2(h * .6f, h)); ImGui::SameLine(0, 4 * hudScale);
        ImGui::TextDisabled("%s", loc::T(entry.second));
    }
    const auto state = loc::Tf("training.frame_advantage",
        loc::T(meter.advantage.pending ? "training.measuring" : meter.advantage.knockdown ? "training.wakeup" : meter.frozen ? "training.held" : "training.live"));
    const float stateAt = left + full - ImGui::CalcTextSize(state.c_str()).x;
    ImGui::SameLine(0, 10 * hudScale);
    if (ImGui::GetCursorPosX() <= stateAt) { ImGui::SameLine(stateAt); ImGui::TextDisabled("%s", state.c_str()); }
    else ImGui::NewLine();
}
// The first measurement that is unavailable, and why, for the HUD's reason
// line: the pad and keys cannot hover the tooltips that say the same.
std::string MeasurementReason(const MeterView& meter) {
    if(!meter.advantage.valid)return Unavailable(loc::T("training.advantage"),meter.advantage.unavailable);
    for(int side=0;side<2;++side)if(meter.startupFrames[side]<0)
        return "P"+std::to_string(side+1)+" "+Unavailable(loc::T("training.startup"),meter.startupUnavailable[side]);
    return {};
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
        // Always one line, so the HUD does not jump as measurements come and go.
        const auto reason=MeasurementReason(meter);
        // The legend is the more useful line; a reason shows only while a reading is being waited for.
        if(reason.empty()||!meter.advantage.pending) MeterLegend(meter,hudScale);
        else ImGui::TextDisabled("%s", FitLabel(reason,ImGui::GetContentRegionAvail().x).c_str());
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
    MeterWindow(view.meter, hudScale, (std::min)(900 * hudScale, vp->Size.x * .75f), vp->Pos.y + vp->Size.y * TrainingHudBottom);
}
// A Training table's match HUD explains why shared position controls are unavailable.
unsigned MatchPracticeKeys(bool) {
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
    return 0;
}
TrainingHudInput DrawTrainingHud(const training::View& view) {
    if (!view.available) return {};
    ChallengerBanner(view);
    const auto* vp = ImGui::GetMainViewport();
    // Size the passive HUD to the game viewport; menu/DPI scaling should not
    // turn it into a large panel over the fight.
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    // Wide, so a frame is a cell the eye can pick out.
    const float width = (std::min)(900 * hudScale, vp->Size.x * .75f);
    // The game's super meters and their SUPER! banners start about 17% above
    // the bottom edge and scale with the height, so the meter sits just above them.
    const float hudBottom = vp->Pos.y + vp->Size.y * TrainingHudBottom;
    const ImVec2 hudTop = MeterWindow(view.meter, hudScale, width, hudBottom);
    // The one input this HUD takes: a chip that opens the controls for a
    // mouse, as F6 does from the keyboard. It captures the mouse only while
    // the pointer is over it, so the passive meter above never does. It
    // hangs under the meter, between the game's two super gauges, so the
    // middle of the screen stays the fight's.
    TrainingHudInput input;
    ImGui::SetNextWindowPos(ImVec2(hudTop.x, hudBottom + 4 * hudScale), ImGuiCond_Always, ImVec2(0, 0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training shortcuts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        input.open = ImGui::SmallButton(loc::T("training.open_chip"));
        ReportMenuCard("training-open", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        // F8 as a chip: play the selected slot again, or stop it.
        const bool playing = view.mode == training::Mode::Playback;
        if (playing || view.lengths[view.selected] > 0) {
            ImGui::SameLine();
            if (playing) input.stop = ImGui::SmallButton(loc::T("training.stop_chip"));
            else input.replay = ImGui::SmallButton(loc::Tf("training.replay_chip", view.selected + 1).c_str());
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", loc::T("training.hide_hint"));
        // The position keys where the chips leave room for them inside the HUD's width.
        const auto keys = TrainingKeyHints();
        ImGui::SameLine();
        if (!keys.empty() && ImGui::GetCursorPosX() + ImGui::CalcTextSize(keys.c_str()).x <= width) ImGui::TextDisabled("%s", keys.c_str());
        else ImGui::NewLine();
        // What the last position key did, for the few seconds it is news.
        bool failed = false;
        const auto notice = TrainingNotice(failed);
        if (!notice.empty()) {
            const auto shown = FitLabel(notice, width - 8 * hudScale);
            if (failed) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 121, 129, 255)), "%s", shown.c_str());
            else ImGui::TextDisabled("%s", shown.c_str());
        }
        ImGui::SetWindowFontScale(1.f);
        input.pointer = ImGui::IsWindowHovered();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return input;
}
} }
