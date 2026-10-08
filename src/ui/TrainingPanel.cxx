#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include "../common/FighterCatalog.hxx"
#include "../netplay/JsonFileStore.hxx"
#include "../training/ComboTrial.hxx"
#include "../training/ComboEdit.hxx"
#include "ComboGlyphs.hxx"
#include "ComboBlocks.hxx"
#include "../training/RecordingFile.hxx"
#include "../training/ComboReplay.hxx"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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
std::string Buttons(unsigned bits) {
    std::string text;
    const unsigned masks[] = {1, 2, 4, 8, 0x10, 0x20, 0x400, 0x40, 0x80, 0x800};
    const char* labels[] = {"U", "D", "L", "R", "LP", "MP", "HP", "LK", "MK", "HK"};
    for (int i = 0; i < 10; ++i) if (bits & masks[i]) {
        if (!text.empty()) text += " + "; text += labels[i];
    }
    return text.empty() ? loc::T("training.neutral") : text;
}
}


namespace {
// The combo creator: combos typed as notation, kept in packs, shared as text
// through the clipboard, and shown as a tree of every route. It owns no game
// state, so it works on the overlay thread alone.
struct ComboCreator {
    std::filesystem::path directory;
    // unreadable: combos.json exists but could not be read, so it is never overwritten.
    bool loaded=false, unreadable=false, failed=false;
    std::vector<combo::Pack> packs;
    int pack=0, entry=0, fighter=0;
    std::string name, steps, notes, notice;
    combo::Tree tree;
    // Who performs a replay, which way they face at the start, and the frames
    // between linked moves.
    int replayBy=0, replayOffset=0; bool replayFacingRight=true;
    // Every replay or trial attempt starts from the saved position.
    bool resetBeforeReplay=true;
    // Record combo ends when the combo drops, so a retry is not written down behind it.
    bool captureStopOnDrop=true;
    // The side Player 1 starts a replay or trial attempt on: 0 where the
    // fighters stand, 1 the left, 2 the right. checkpointPlace: where the two
    // stood when the position was saved this battle.
    int side=0; bool checkpointPlaced=false; float checkpointPlace[2]={0,0};
    // The dummy and gauge settings being edited: the selected combo's, or the
    // next combo's when none is selected.
    combo::Setup setup;
    // What the dummy does by itself, and the keys of the six hotkeys (replay,
    // reset, trial, record, save, save position) as offsets from F1 (-1
    // unbound): the player's, kept in training.json.
    training::DummyPlan plan; std::string replyMoves;
    int keys[6]={0,1,2,3,8,10};
} creator;
constexpr const wchar_t* ComboBookFile=L"combos.json";
constexpr const wchar_t* PracticeFile=L"training.json";
// The pack a combo goes to when none exists yet. Saved, so it stays English.
constexpr const char* DefaultPack="My combos";
const char* FighterName(const std::string& code) {
    for(int id=0;id<selection::FighterCount;++id) if(const auto* fighter=selection::FindFighter(id)) if(code==fighter->code) return fighter->name;
    return code.c_str();
}
combo::Pack* CurrentPack() { return creator.packs.empty()?nullptr:&creator.packs[(std::min)(creator.pack,static_cast<int>(creator.packs.size())-1)]; }
// The selected fighter's code, which every pack and combo shown is linked to.
std::string FighterCode() { const auto* fighter=selection::FindFighter(creator.fighter); return fighter?fighter->code:""; }
// The pack's first combo of the selected fighter at or after `from`, going `step`; -1 when it has none there.
int FighterEntry(const combo::Pack& pack,int from=0,int step=1) {
    const auto code=FighterCode();
    for(int i=from;i>=0&&i<static_cast<int>(pack.combos.size());i+=step) if(pack.combos[i].character==code) return i;
    return -1;
}
// A pack is the selected fighter's when it holds a combo of theirs, or none at all yet.
bool FighterPack(const combo::Pack& pack) { return pack.combos.empty()||FighterEntry(pack)>=0; }
// The selected combo: only ever one of the selected fighter.
combo::Combo* CurrentCombo() {
    auto* pack=CurrentPack();
    if(!pack||pack->combos.empty()) return nullptr;
    auto* shown=&pack->combos[(std::min)(creator.entry,static_cast<int>(pack->combos.size())-1)];
    return shown->character==FighterCode()?shown:nullptr;
}
// The selected pack when it is the selected fighter's, to show and to add to.
combo::Pack* FighterPackShown() { auto* pack=CurrentPack(); return pack&&FighterPack(*pack)?pack:nullptr; }
void ComboNotice(std::string text,bool failed=false) { creator.notice=std::move(text); creator.failed=failed; }
void RefreshComboTree() { creator.tree=combo::BuildTree(creator.packs); }
// One node of the tree in the reader: its step as glyphs, then the combos
// that end on it, with its routes beneath.
void DrawTreeNode(const combo::Node& node, int depth) {
    const float h=ImGui::GetTextLineHeight();
    const ImVec2 at(ImGui::GetCursorScreenPos().x+depth*h, ImGui::GetCursorScreenPos().y);
    float used=depth*h+DrawComboStep(node.step,at,ImGui::GetColorU32(ImGuiCol_Text));
    for(const auto& label:node.combos)
        used+=h*.4f+glyphs::Word(ImGui::GetWindowDrawList(),ImVec2(ImGui::GetCursorScreenPos().x+used+h*.4f,at.y),h,("["+label+"]").c_str(),ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::Dummy(ImVec2(used,h));
    for(const auto& child:node.children) DrawTreeNode(child,depth+1);
}
void SavePractice() {
    if(creator.directory.empty()) return;
    std::string error;
    const nlohmann::json practice{{"reply",{{"when",creator.plan.when},{"slot",creator.plan.slot},{"chance",creator.plan.chance},{"timing",creator.plan.timing},
        {"vary_stance",creator.plan.varyStance},{"moves",creator.replyMoves}}},{"keys",{creator.keys[0],creator.keys[1],creator.keys[2],creator.keys[3],creator.keys[4],creator.keys[5]}},{"record_stops_on_drop",creator.captureStopOnDrop},{"side",creator.side}};
    if(!netplay::json_file::Publish(creator.directory,PracticeFile,practice,error)) ComboNotice(loc::T("training.combo.save_failed"),true);
}
// The plan into the game, its typed reply made into input for either facing.
// No fighter's move names: the reply is the dummy's, whoever that is.
bool SendPlan(const training::View& view,const TrainingSubmit& submit) {
    Command command; command.action=Action::DummyPlan; command.generation=view.generation; command.plan=creator.plan;
    std::vector<std::string> steps; std::string error;
    if(combo::ParseSteps(creator.replyMoves,"",steps,error)&&!steps.empty()) for(int side=0;side<2;++side) command.plan.moves[side]=combo::Synthesize(steps,side==0,0);
    return submit&&submit(command);
}
void LoadCombos() {
    if(creator.loaded) return;
    creator.loaded=true;
    if(creator.directory.empty()) return;
    std::string bytes,error; bool missing=false;
    // The dummy's reply and the hotkeys; anything unreadable keeps its default.
    nlohmann::json practice;
    if(netplay::json_file::ReadBytes(creator.directory/PracticeFile,bytes,missing,error)&&!missing&&netplay::json_file::Parse(bytes,practice,error)&&practice.is_object()) {
        const auto reply=practice.value("reply",nlohmann::json::object());
        const auto number=[&](const char* name,int low,int high,int fallback) { const auto at=reply.find(name); return at!=reply.end()&&at->is_number_integer()&&*at>=low&&*at<=high?at->get<int>():fallback; };
        if(reply.is_object()) {
            creator.plan.when=number("when",0,4,0); creator.plan.slot=number("slot",0,SlotCount-1,0); creator.plan.chance=number("chance",25,100,100);
            creator.plan.timing=number("timing",-MaxReplyTiming,MaxReplyTiming,0); creator.plan.varyStance=reply.value("vary_stance",nlohmann::json(false))==true;
            if(reply.contains("moves")&&reply["moves"].is_string()) creator.replyMoves=combo::Clean(reply["moves"].get<std::string>());
        }
        creator.captureStopOnDrop=practice.value("record_stops_on_drop",nlohmann::json(true))!=false;
        if(const auto side=practice.find("side"); side!=practice.end()&&side->is_number_integer()&&*side>=0&&*side<=2) creator.side=side->get<int>();
        const auto keys=practice.value("keys",nlohmann::json::array());
        for(std::size_t i=0;i<6&&keys.is_array()&&i<keys.size();++i) if(keys[i].is_number_integer()&&keys[i]>=-1&&keys[i]<12&&keys[i]!=9&&(keys[i]<4||keys[i]>7)) creator.keys[i]=keys[i].get<int>();
    }
    bytes.clear(); error.clear(); missing=false;
    if(!netplay::json_file::ReadBytes(creator.directory/ComboBookFile,bytes,missing,error)||
        (!missing&&!combo::Import(bytes,creator.packs,error))) {
        creator.unreadable=true; ComboNotice(loc::Tf("training.combo.invalid",error),true);
    }
    RefreshComboTree();
}
// ponytail: written on the overlay thread, one small file per change; move to
// the settings writer if a save is ever felt as a hitch.
void SaveCombos() {
    creator.pack=(std::max)(0,(std::min)(creator.pack,static_cast<int>(creator.packs.size())-1));
    RefreshComboTree();
    if(creator.directory.empty()) return;
    std::string error;
    const auto text=combo::Export(creator.packs);
    if(creator.unreadable||text.size()>combo::MaxBytes||
        !netplay::json_file::Publish(creator.directory,ComboBookFile,nlohmann::json::parse(text,nullptr,false),error))
        ComboNotice(loc::T("training.combo.save_failed"),true);
}
// Shows the selected combo in the fields, to copy it or to change and add it again.
void ShowCombo() {
    const auto* shown=CurrentCombo();
    if(!shown) return;
    creator.name=shown->name; creator.notes=shown->notes; creator.steps=combo::JoinSteps(shown->steps); creator.setup=shown->setup;
    for(int id=0;id<selection::FighterCount;++id) if(const auto* fighter=selection::FindFighter(id)) if(shown->character==fighter->code) creator.fighter=id;
}
// Adds packs to the book and selects the first of them.
void AddCombos(const std::vector<combo::Pack>& incoming) {
    const int added=combo::Merge(creator.packs,incoming);
    for(std::size_t i=0;i<creator.packs.size();++i) if(!incoming.empty()&&creator.packs[i].name==incoming[0].name) {
        creator.pack=static_cast<int>(i); creator.entry=static_cast<int>(creator.packs[i].combos.size())-1;
        // What came in may be another fighter's: the creator turns to that fighter.
        if(creator.entry>=0) for(int id=0;id<selection::FighterCount;++id) if(const auto* fighter=selection::FindFighter(id))
            if(creator.packs[i].combos[creator.entry].character==fighter->code) creator.fighter=id;
    }
    ComboNotice(added?loc::Tf("training.combo.added",added):loc::T("training.combo.exists"),!added);
    if(added) SaveCombos();
}
// The mod runs inside the game, so the game's folder is that of its program.
std::filesystem::path GameFolder() {
    wchar_t program[32768]={};
    GetModuleFileNameW(nullptr,program,32768);
    return std::filesystem::path(program).parent_path();
}
std::string Shown(const std::filesystem::path& path) { const auto text=path.u8string(); return std::string(text.begin(),text.end()); }
// Reads the selected fighter's command, script and trial files from the game.
// ponytail: under a megabyte, read on the overlay thread when a row is
// chosen; move to a worker if it is ever felt as a hitch.
bool LoadGameTrials(const selection::Fighter& fighter,combo::Fighter& files,clg::File& trials,std::filesystem::path& from) {
    std::string error;
    if(combo::LoadFighter(GameFolder(),fighter.code,files,error)&&combo::LoadTrials(GameFolder(),fighter.code,trials,error,&from)) return true;
    ComboNotice(loc::Tf("training.combo.game_file",error),true);
    return false;
}
// A pack of the game's trials: the fighter's name, then the game they are
// from. Saved, so it stays English.
std::string TrialPackName(const selection::Fighter& fighter,bool ultra) { return std::string(fighter.name)+(ultra?" - Ultra Street Fighter 4 trials":" - Street Fighter 4 trials"); }
// The game's own trials for a fighter into the book, as two packs: Ultra's,
// and the first set the game still carries from before Ultra. read: the
// trials found. Returns how many combos were new.
int AddGameTrials(const selection::Fighter& fighter,int& read) {
    int added=0; read=0;
    for(const bool ultra:{true,false}) {
        combo::Fighter files; clg::File trials; std::string error; std::vector<std::string> issues;
        if(ultra?!(combo::LoadFighter(GameFolder(),fighter.code,files,error)&&combo::LoadTrials(GameFolder(),fighter.code,trials,error)):
            !combo::LoadFirstTrials(GameFolder(),fighter.code,files,trials,error)) continue;
        const auto pack=combo::ReadTrials(trials,files,fighter.code,TrialPackName(fighter,ultra),issues);
        read+=static_cast<int>(pack.combos.size());
        if(!pack.combos.empty()) added+=combo::Merge(creator.packs,{pack});
    }
    return added;
}
// Selects the fighter's Ultra trials, or its first ones.
void SelectGameTrials(const selection::Fighter& fighter) {
    for(const bool ultra:{false,true}) for(std::size_t i=0;i<creator.packs.size();++i)
        if(creator.packs[i].name==TrialPackName(fighter,ultra)&&!creator.packs[i].combos.empty()) { creator.pack=static_cast<int>(i); creator.entry=0; }
}
void ImportTrials() {
    const auto* fighter=selection::FindFighter(creator.fighter);
    if(!fighter) return;
    int read=0; const int added=AddGameTrials(*fighter,read);
    if(!read) { ComboNotice(loc::Tf("training.combo.game_file",fighter->name),true); return; }
    SelectGameTrials(*fighter);
    if(added) SaveCombos();
    ComboNotice(loc::Tf("training.combo.trials_imported",read,read,added));
    ShowCombo();
}
// The selected pack's combos for the selected fighter as the game's trial
// file. It is written beside the combo book, never into the game's folder:
// replacing the game's file is the player's own step.
void ExportTrial() {
    const auto* fighter=selection::FindFighter(creator.fighter); const auto* pack=CurrentPack();
    combo::Fighter files; clg::File stock,made; std::filesystem::path from;
    if(!fighter||!pack||creator.directory.empty()||!LoadGameTrials(*fighter,files,stock,from)) return;
    std::vector<std::string> issues; std::string error;
    const int written=combo::BuildTrials(*pack,fighter->code,files,stock,made,issues);
    const auto folder=creator.directory/L"trials";
    std::string notice;
    if(!written) notice=loc::Tf("training.combo.trial_none",fighter->name);
    else if(!combo::SaveTrials(folder,fighter->code,made,error)) { ComboNotice(loc::Tf("training.combo.trial_failed",error),true); return; }
    else notice=loc::Tf("training.combo.trial_exported",written,Shown(folder/combo::TrialFileName(fighter->code)),Shown(from));
    if(!issues.empty()) notice+=" "+loc::Tf("training.combo.trial_skipped",static_cast<int>(issues.size()),issues[0]);
    ComboNotice(notice,!written);
}
// Starts Ember's own trial on the selected combo. What each step looks for
// comes from the files of the combo's fighter; player 1 has to be that
// fighter, which nothing here can check.
// ponytail: the files are read on the overlay thread when the row is chosen,
// as LoadGameTrials does; move to a worker if it is ever felt as a hitch.
// The combo's dummy and gauge settings into the game, before it is played.
bool ApplySetup(const training::View& view,const TrainingSubmit& submit) {
    if(!submit) return true;
    Command command; command.action=Action::DummyState; command.generation=view.generation;
    command.dummy.action=creator.setup.action; command.dummy.guard=creator.setup.guard; command.dummy.counterHit=creator.setup.counterHit;
    command.dummy.quickStand=creator.setup.quickStand; command.dummy.super=creator.setup.super; command.dummy.revenge=creator.setup.revenge;
    return submit(command);
}
// The saved position first, when there is one and the player wants it.
// The checkpoint when there is one, else the combo's own place.
bool PlaceCommand(const training::View& view,Command& command) {
    const auto* shown=CurrentCombo();
    if(view.checkpoint) { command.action=Action::Restore; }
    else if(shown&&shown->placed) { command.action=Action::Place; command.place[0]=shown->place[0]; command.place[1]=shown->place[1]; }
    else return false;
    command.generation=view.generation; return true;
}
// Where the two stand once Player 1 is on the side the player prefers:
// the saved place (saved), else where they stand now, turned about the
// middle of the stage when Player 1 is on the other side. False when no
// side is preferred.
bool SidePlace(const training::View& view,bool saved,float place[2]) {
    if(!creator.side) return false;
    const auto* shown=CurrentCombo();
    const float* from=saved&&view.checkpoint&&creator.checkpointPlaced?creator.checkpointPlace:saved&&!view.checkpoint&&shown&&shown->placed?shown->place.data():view.x;
    const float sign=(from[0]>from[1])==(creator.side==2)?1.f:-1.f;
    place[0]=sign*from[0]; place[1]=sign*from[1];
    return true;
}
bool SendSide(const training::View& view,bool saved,const TrainingSubmit& submit) {
    Command command; command.action=Action::Place; command.generation=view.generation;
    return !SidePlace(view,saved,command.place)||(submit&&submit(command));
}
bool ResetPosition(const training::View& view,const TrainingSubmit& submit) {
    if(!submit) return true;
    Command restore;
    if(creator.resetBeforeReplay&&PlaceCommand(view,restore)&&!submit(restore)) return false;
    return SendSide(view,creator.resetBeforeReplay,submit);
}
void StartTrial(const training::View& view,const TrainingSubmit& submit) {
    const auto* shown=CurrentCombo();
    if(!shown) return;
    combo::Fighter files; std::string error;
    if(!combo::LoadFighter(GameFolder(),shown->character,files,error)) { ComboNotice(loc::Tf("training.combo.game_file",error),true); return; }
    Command command; command.action=Action::StartTrial; command.generation=view.generation;
    command.trial=combo::PracticeTrial(files,*shown); command.trialSteps=shown->steps;
    command.trialResetOnDrop=creator.resetBeforeReplay;
    command.trialPlaced=creator.resetBeforeReplay&&SidePlace(view,true,command.place);
    // The game's own task list needs the fighter's trial file for its texts;
    // without it the overlay's list shows the trial.
    clg::File stock; std::string unused;
    if(combo::LoadTrials(GameFolder(),shown->character,stock,unused)) command.trialTexts=combo::TrialTexts(files,stock,*shown);
    for(int id=0;id<selection::FighterCount;++id) if(const auto* fighter=selection::FindFighter(id)) if(shown->character==fighter->code) command.trialFighter=id;
    int checked=0;
    for(const auto& step:command.trial.steps) checked+=!step.ids.empty();
    // The trial's own rule for what can run, asked before the command leaves.
    TrialSession probe;
    if(!probe.Load(command.trial,error)) ComboNotice(loc::Tf("training.combo.run.refused",error),true);
    else if(!submit||!ApplySetup(view,submit)||!ResetPosition(view,submit)||!submit(command)) ComboNotice(loc::T("training.command_rejected"),true);
    else ComboNotice(loc::Tf("training.combo.run.started",FighterName(shown->character),checked,static_cast<int>(shown->steps.size())));
}
void Replay(const std::vector<std::string>& steps,const training::View& view,const TrainingSubmit& submit) {
    if(steps.empty()) { ComboNotice(loc::Tf("training.combo.invalid",creator.steps),true); return; }
    // The replay loads into the selected slot, which must not be the reply's recording.
    if(creator.plan.when&&creator.replyMoves.empty()&&view.selected==creator.plan.slot&&view.lengths[view.selected]>0) { ComboNotice(loc::Tf("training.combo.reply.slot_busy",view.selected+1),true); return; }
    if(!ApplySetup(view,submit)||!ResetPosition(view,submit)) { ComboNotice(loc::T("training.command_rejected"),true); return; }
    Command load; load.action=Action::Load; load.generation=view.generation; load.value=creator.replayBy;
    // With a side preferred the facing follows from it: the dummy stands opposite Player 1.
    load.frames=combo::Synthesize(steps,creator.side?(creator.side==1)!=(creator.replayBy==1):creator.replayFacingRight,creator.replayOffset);
    Command play; play.action=Action::Play; play.generation=view.generation;
    if(!submit||!submit(load)||!submit(play)) { ComboNotice(loc::T("training.command_rejected"),true); return; }
    ComboNotice(loc::Tf("training.combo.replay.started",view.selected+1));
}
// The moves the timing screen and a replay work on: the typed line when
// there is one, else the selected combo.
std::vector<std::string> TimingSteps() {
    std::vector<std::string> steps; std::string error;
    const auto* fighter=selection::FindFighter(creator.fighter);
    if(!creator.steps.empty()) {
        // Parsed once per change: the HUD asks every frame.
        static std::string text; static int parsedFor=-1; static std::vector<std::string> parsed;
        if(text!=creator.steps||parsedFor!=creator.fighter) { text=creator.steps; parsedFor=creator.fighter; parsed.clear(); combo::ParseSteps(text,fighter?fighter->code:"",parsed,error); }
        return parsed;
    }
    return CurrentCombo()?CurrentCombo()->steps:steps;
}
// Writes edited moves back where TimingSteps took them from: the typed line,
// and the saved combo too when the line is that combo as it stands.
void StoreSteps(const std::vector<std::string>& steps) {
    auto* shown=CurrentCombo();
    const bool shownLine=shown&&(creator.steps.empty()||combo::JoinSteps(shown->steps)==creator.steps);
    if(!creator.steps.empty()||!shown) creator.steps=combo::JoinSteps(steps);
    if(shownLine) { shown->steps=steps; SaveCombos(); }
}
// The move a row id stands for: the number after its prefix, -1 for the
// screen's other rows ("ct-replay", "cm-add").
int RowIndex(const std::string& id,const char* prefix) {
    const std::size_t length=std::strlen(prefix);
    if(id.size()<=length||id.compare(0,length,prefix)!=0) return -1;
    for(std::size_t i=length;i<id.size();++i) if(id[i]<'0'||id[i]>'9') return -1;
    return std::atoi(id.c_str()+length);
}
// The timing screen: those moves one by one, each with its "@" offset to
// nudge and what the last replay saw for it.
struct Tune { bool on=false, started=false; std::size_t step=0; int best=training::MinOffset-1, candidate=0, settle=0, start=0; } tune;
// How far below a move's starting offset the tuner looks before giving up on it.
constexpr int TuneReach=6;
// A running playback stops first, and tuning with it; else the moves replay.
void ReplayOrStop(const std::vector<std::string>& steps,const training::View& view,const TrainingSubmit& submit) {
    if(view.mode!=Mode::Playback) { Replay(steps,view,submit); return; }
    Command stop; stop.action=Action::Stop; stop.generation=view.generation;
    if(submit&&submit(stop)) { tune.on=false; ComboNotice(loc::T("training.combo.replay.stopped")); }
}
void StartTune(const training::View& view,const TrainingSubmit& submit);
// Writes a move's offset into the saved combo when the typed line is that
// combo (selecting one fills the line), and into the typed line itself.
void SetOffset(std::vector<std::string>& steps,std::size_t index,int offset) {
    combo::Step step; std::string error;
    if(index>=steps.size()||!combo::ParseStep(steps[index],step,error)) return;
    step.offset=(std::max)(training::MinOffset,(std::min)(training::MaxOffset,offset));
    auto* shown=CurrentCombo();
    const bool shownLine=shown&&(creator.steps.empty()||combo::JoinSteps(shown->steps)==creator.steps);
    steps[index]=combo::Canonical(step);
    if(!creator.steps.empty()) creator.steps=combo::JoinSteps(steps);
    if(shownLine) { shown->steps=steps; SaveCombos(); }
}
// The move editor: the combo a move a row. Its add, duplicate and remove
// rows, which the timing screen has too, work on the move row focused last,
// since a row and its action cannot be focused together. A group is a run of
// neighbouring rows, from..to, -1 for none; it lasts while the editor does.
// ponytail: a looped group is written out as rows, so the combo book and the
// replay know nothing of groups. Keep them in the combo once a loop has to
// be edited as one after it was made.
struct RowEdit { int row=0, from=-1, to=-1, loops=2; } rowEdit;
// Rows came in before row at (count > 0) or one went from it (count < 0): the group keeps its moves.
void ShiftGroup(int at,int count) {
    if(rowEdit.from<0) return;
    if(count>0) { if(at<=rowEdit.from) { rowEdit.from+=count; rowEdit.to+=count; } else if(at<=rowEdit.to) rowEdit.to+=count; }
    else if(at<rowEdit.from) { --rowEdit.from; --rowEdit.to; }
    else if(at<=rowEdit.to&&--rowEdit.to<rowEdit.from) rowEdit.from=rowEdit.to=-1;
}
// Add after, duplicate and remove for the move focused last, named in each label.
void EditRows(std::vector<MenuEntry>& rows,const std::string& prefix,int count) {
    rowEdit.row=(std::max)(0,(std::min)(rowEdit.row,count-1));
    const int number=rowEdit.row+1;
    auto add=TextRow(prefix+"add",count?loc::Tf("training.combo.moves.add",number):loc::T("training.combo.moves.add_first"),"",256);
    add.detail=loc::T("training.combo.moves.add.detail"); rows.push_back(add);
    rows.push_back(Row(prefix+"duplicate",loc::Tf("training.combo.moves.duplicate",number),loc::T("training.combo.moves.duplicate.detail"),count>0));
    rows.push_back(Row(prefix+"remove",loc::Tf("training.combo.moves.remove",number),loc::T("training.combo.moves.remove.detail"),count>1));
}
std::vector<MenuEntry> MoveRows() {
    const auto steps=TimingSteps();
    const int count=static_cast<int>(steps.size());
    if(rowEdit.to>=count) rowEdit.from=rowEdit.to=-1;
    const bool grouped=rowEdit.from>=0;
    const std::string range=grouped?"("+std::to_string(rowEdit.from+1)+"-"+std::to_string(rowEdit.to+1)+")":"";
    std::vector<MenuEntry> rows{InfoRow("cm-about",loc::T("training.guide"),"",loc::T("training.combo.moves.guide"))};
    EditRows(rows,"cm-",count);
    const int number=rowEdit.row+1;
    rows.push_back(Value("cm-move",loc::Tf("training.combo.moves.move",number),loc::T("training.combo.moves.move.value"),loc::T("training.combo.moves.move.detail"),count>1));
    rows.push_back(Row("cm-group",loc::Tf(grouped?"training.combo.moves.group.extend":"training.combo.moves.group.start",number),loc::T("training.combo.moves.group.detail"),count>0));
    rows.push_back(Row("cm-group-clear",loc::Tf("training.combo.moves.group.clear",range),loc::T("training.combo.moves.group.clear.detail"),grouped));
    rows.push_back(Row("cm-group-duplicate",loc::Tf("training.combo.moves.group.duplicate",range),loc::T("training.combo.moves.group.duplicate.detail"),grouped));
    rows.push_back(Value("cm-group-loops",loc::T("training.combo.moves.group.loops"),"x"+std::to_string(rowEdit.loops),loc::T("training.combo.moves.group.loops.detail"),grouped));
    rows.push_back(Row("cm-group-loop",loc::Tf("training.combo.moves.group.loop",range,rowEdit.loops),loc::T("training.combo.moves.group.loop.detail"),grouped));
    rows.push_back(ConfirmRow("cm-group-remove",loc::Tf("training.combo.moves.group.remove",range),loc::T("training.combo.moves.group.remove.detail"),grouped));
    rows.push_back(Row("cm-replay",loc::T("training.combo.replay"),loc::T("training.combo.moves.replay.detail"),count>0));
    for(int i=0;i<count;++i) {
        // A group's moves stand in brackets.
        const bool in=grouped&&i>=rowEdit.from&&i<=rowEdit.to;
        auto row=TextRow("cm-"+std::to_string(i),(in?"[":"")+std::to_string(i+1)+(in?"]":"."),steps[i],256);
        row.detail=loc::T("training.combo.moves.row.detail"); rows.push_back(row);
    }
    return rows;
}
void HandleMoves(const MenuAction& a,const training::View& view,const TrainingSubmit& submit) {
    auto steps=TimingSteps();
    const int count=static_cast<int>(steps.size()), row=rowEdit.row;
    const bool grouped=rowEdit.from>=0&&rowEdit.to<count;
    const auto from=static_cast<std::size_t>((std::max)(0,rowEdit.from)), length=static_cast<std::size_t>(rowEdit.to-rowEdit.from+1);
    // An edit changes what a tuning run is replaying, so it ends the run.
    const auto done=[&](bool changed) {
        if(!changed) { ComboNotice(loc::Tf("training.combo.moves.limit",static_cast<int>(combo::MaxSteps)),true); return; }
        tune.on=false; StoreSteps(steps); ComboNotice(loc::Tf("training.combo.moves.changed",static_cast<int>(steps.size())));
    };
    if(a.kind==MenuAction::TextAccepted) {
        const int edited=RowIndex(a.id,"cm-");
        if(a.id!="cm-add"&&(edited<0||edited>=count)) return;
        const auto* fighter=selection::FindFighter(creator.fighter);
        std::vector<std::string> typed; std::string error;
        if(!combo::ParseSteps(a.text,fighter?fighter->code:"",typed,error)||typed.empty()) { ComboNotice(loc::Tf("training.combo.invalid",error.empty()?combo::Clean(a.text):error),true); return; }
        const int added=static_cast<int>(typed.size());
        if(edited>=0) {
            // The typed moves take the row's place.
            if(steps.size()-1+typed.size()>combo::MaxSteps) { done(false); return; }
            steps.erase(steps.begin()+edited); combo::InsertSteps(steps,static_cast<std::size_t>(edited),typed);
            ShiftGroup(edited+1,added-1); done(true);
        } else {
            const int at=count?row+1:0;
            const bool fits=combo::InsertSteps(steps,static_cast<std::size_t>(at),typed);
            // The next add goes after these.
            if(fits) { ShiftGroup(at,added); rowEdit.row=at+added-1; }
            done(fits);
        }
        return;
    }
    if(a.kind==MenuAction::Adjust) {
        if(a.id=="cm-group-loops") rowEdit.loops=(std::max)(2,(std::min)(10,rowEdit.loops+(a.delta>0?1:-1)));
        else if(a.id=="cm-move"&&count>1) {
            const int delta=a.delta>0?1:-1;
            if(combo::MoveStep(steps,static_cast<std::size_t>(row),delta)) { rowEdit.row=row+delta; tune.on=false; StoreSteps(steps); }
        }
        return;
    }
    if(a.kind!=MenuAction::Activate) return;
    if(a.id=="cm-replay") Replay(steps,view,submit);
    else if(a.id=="cm-duplicate"&&count) {
        const bool fits=combo::RepeatSteps(steps,static_cast<std::size_t>(row),1,1);
        if(fits) { ShiftGroup(row+1,1); rowEdit.row=row+1; }
        done(fits);
    } else if(a.id=="cm-remove"&&count) {
        const bool fits=combo::RemoveSteps(steps,static_cast<std::size_t>(row),1);
        if(fits) ShiftGroup(row,-1);
        done(fits);
    } else if(a.id=="cm-group"&&count) {
        if(!grouped) rowEdit.from=rowEdit.to=row;
        else { rowEdit.from=(std::min)(rowEdit.from,row); rowEdit.to=(std::max)(rowEdit.to,row); }
    } else if(a.id=="cm-group-clear") rowEdit.from=rowEdit.to=-1;
    else if(!grouped) return;
    else if(a.id=="cm-group-duplicate") done(combo::RepeatSteps(steps,from,length,1));
    else if(a.id=="cm-group-loop") done(combo::RepeatSteps(steps,from,length,static_cast<std::size_t>(rowEdit.loops-1)));
    else if(a.id=="cm-group-remove") {
        const bool fits=combo::RemoveSteps(steps,from,length);
        if(fits) rowEdit.from=rowEdit.to=-1;
        done(fits);
    }
}
// The moves the editor offers to add: the selected fighter's own, as its
// command file spells them, after the ones every fighter has. Without the
// game's files it is the normals.
// ponytail: read on the overlay thread when the fighter changes, as
// LoadGameTrials does; move to a worker if it is ever felt as a hitch.
struct Addable { bool loaded=false; std::string fighter; std::vector<std::pair<std::string,std::string>> moves; } addable;
void LoadAddable() {
    const auto* fighter=selection::FindFighter(creator.fighter);
    const std::string code=fighter?fighter->code:"";
    if(addable.loaded&&addable.fighter==code) return;
    addable=Addable{}; addable.loaded=true; addable.fighter=code;
    const auto add=[&](const std::string& notation,const std::string& name) {
        if(notation.empty()) return;
        for(const auto& move:addable.moves) if(move.first==notation) return;
        addable.moves.push_back({notation,name});
    };
    add("FADC",loc::T("training.combo.moves.add.fadc")); add("RFADC",loc::T("training.combo.moves.add.rfadc"));
    add("66",loc::T("training.combo.moves.add.dash")); add("44",loc::T("training.combo.moves.add.backdash"));
    add("MP+MK",loc::T("training.combo.moves.add.focus")); add("LP+LK",loc::T("training.combo.moves.add.throw"));
    combo::Fighter files; std::string error;
    if(!code.empty()&&combo::LoadFighter(GameFolder(),code,files,error)) for(const auto& move:files.moves.moves) add(combo::MoveNotation(move),move.name);
    else for(const char* stance:{"5","2","j."}) for(const char* button:{"LP","MP","HP","LK","MK","HK"}) add(std::string(stance)+button,"");
}
// Changes one thing about a move and writes the combo back. Never writes a
// move the notation would not read again.
template<typename Change> void ChangeMove(std::size_t index,Change change) {
    auto steps=TimingSteps();
    combo::Step step,check; std::string error;
    if(index>=steps.size()||!combo::ParseStep(steps[index],step,error)) return;
    change(step);
    if(index==0) step.cancel=step.follow=false;
    const auto text=combo::Canonical(step);
    if(!combo::ParseStep(text,check,error)) return;
    steps[index]=text; tune.on=false; StoreSteps(steps);
}
// A small button that stays lit while its choice is the move's.
bool Chip(const char* label,bool lit,bool enabled=true) {
    if(lit) ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.75f,.40f,.16f,1.f));
    ImGui::BeginDisabled(!enabled);
    const bool pressed=ImGui::SmallButton(label);
    ImGui::EndDisabled();
    if(lit) ImGui::PopStyleColor();
    ImGui::SameLine();
    return pressed;
}
// The move editor's body, in place of the menu's list: the combo's moves on
// the left, the selected move and the screen's rows in the middle, the moves
// to add on the right. The keys still walk the rows; the mouse picks a move,
// drags it to another place, and drags or double-clicks a move in to add it.
void DrawMoveEditor(const std::vector<MenuEntry>& entries,MenuNavigation& nav,MenuAction& action,float height,const training::View& view,const TrainingSubmit& submit) {
    LoadAddable();
    const auto steps=TimingSteps();
    const int count=static_cast<int>(steps.size());
    const float line=ImGui::GetTextLineHeight(), rowHeight=line*1.5f, pad=line*.3f;
    const ImVec2 avail=ImGui::GetContentRegionAvail();
    // The body's own height: what is left of it under the columns says what the focused row does.
    const float detail=line*4.2f, columns=(std::max)(line*6,height-detail-ImGui::GetStyle().ItemSpacing.y), left=avail.x*.32f, middle=avail.x*.36f;
    const ImU32 text=ImGui::GetColorU32(ImGuiCol_Text), dim=ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const bool grouped=rowEdit.from>=0;
    const auto addAfter=[&](int row,const std::string& move) {
        rowEdit.row=row;
        MenuAction add; add.kind=MenuAction::TextAccepted; add.id="cm-add"; add.text=move;
        HandleMoves(add,view,submit);
        nav.Prefer("cm-"+std::to_string(rowEdit.row));
    };
    const auto dropped=[](const char* type) -> int { const auto* payload=ImGui::AcceptDragDropPayload(type); return payload?*static_cast<const int*>(payload->Data):-1; };

    ImGui::BeginChild("cm-combo",ImVec2(left,columns),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoNavInputs);
    ImGui::TextDisabled("%s",loc::T("training.combo.moves.column"));
    static int shownRow=-1;
    for(int i=0;i<count;++i) {
        ImGui::PushID(i);
        const ImVec2 at=ImGui::GetCursorScreenPos();
        if(ImGui::Selectable("##move",i==rowEdit.row,0,ImVec2(0,rowHeight))) { rowEdit.row=i; nav.Focus("cm-"+std::to_string(i),entries); }
        if(i==rowEdit.row&&shownRow!=i&&!ImGui::IsItemVisible()) ImGui::SetScrollHereY(.5f);
        if(ImGui::BeginDragDropSource()) { ImGui::SetDragDropPayload("EmberMove",&i,sizeof i); ImGui::TextUnformatted(steps[i].c_str()); ImGui::EndDragDropSource(); }
        if(ImGui::BeginDragDropTarget()) {
            // A move from the list goes in after this one; one of the combo's own takes this one's place.
            const int added=dropped("EmberAdd"), from=dropped("EmberMove");
            if(added>=0&&added<static_cast<int>(addable.moves.size())) addAfter(i,addable.moves[added].first);
            if(from>=0&&from<count&&from!=i) {
                auto moved=steps; const auto taken=moved[from];
                moved.erase(moved.begin()+from); moved.insert(moved.begin()+(std::min)(i,static_cast<int>(moved.size())),taken);
                combo::LinkFirst(moved); tune.on=false; rowEdit.from=rowEdit.to=-1; rowEdit.row=i;
                StoreSteps(moved); nav.Prefer("cm-"+std::to_string(i));
            }
            ImGui::EndDragDropTarget();
        }
        // A group's moves stand in brackets.
        const bool in=grouped&&i>=rowEdit.from&&i<=rowEdit.to;
        const auto number=(in?"[":"")+std::to_string(i+1)+(in?"]":".");
        const float y=at.y+(rowHeight-line)/2;
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x+pad,y),in?palette::Ember:dim,number.c_str());
        DrawComboStep(steps[i],ImVec2(at.x+pad+line*2.2f,y),text);
        ImGui::PopID();
    }
    shownRow=rowEdit.row;
    if(!count) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s",loc::T("training.combo.moves.empty")); ImGui::PopTextWrapPos(); }
    // The room under the last move takes a move too: it goes on the end.
    const ImVec2 rest=ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##end",ImVec2((std::max)(1.f,rest.x),(std::max)(rowHeight,rest.y)));
    if(ImGui::BeginDragDropTarget()) {
        const int added=dropped("EmberAdd");
        if(added>=0&&added<static_cast<int>(addable.moves.size())) addAfter((std::max)(0,count-1),addable.moves[added].first);
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("cm-selected",ImVec2(middle,columns),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoNavInputs);
    combo::Step step; std::string error;
    if(rowEdit.row<count&&combo::ParseStep(steps[rowEdit.row],step,error)) {
        const auto index=static_cast<std::size_t>(rowEdit.row);
        ImGui::TextDisabled("%s",loc::Tf("training.combo.moves.selected",rowEdit.row+1).c_str());
        const ImVec2 at=ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(DrawComboStep(steps[index],at,text),line)); ImGui::SameLine(); ImGui::TextDisabled("%s",steps[index].c_str());
        ImGui::PushID("cm-properties");
        // How it comes out of the move before it.
        ImGui::TextDisabled("%s",loc::T("training.combo.moves.joins"));
        const int joins=step.follow?2:step.cancel?1:0;
        const char* const joinNames[]={"training.combo.moves.joins.link","training.combo.moves.joins.cancel","training.combo.moves.joins.follow"};
        for(int kind=0;kind<3;++kind) if(Chip(loc::T(joinNames[kind]),joins==kind,index>0||kind==0))
            ChangeMove(index,[&](combo::Step& changed){ changed.cancel=kind>0; changed.follow=kind==2; });
        ImGui::NewLine();
        // One punch or kick button: which, or two of the three for an EX move.
        const bool punches=step.buttons&&!(step.buttons&~combo::Punches), kicks=step.buttons&&!(step.buttons&~combo::Kicks);
        if((punches||kicks)&&!step.mash) {
            const unsigned all=punches?combo::Punches:combo::Kicks;
            const unsigned each[3]={punches?combo::LP:combo::LK,punches?combo::MP:combo::MK,punches?combo::HP:combo::HK};
            const char* const names[3]={"L","M","H"};
            ImGui::TextDisabled("%s",loc::T("training.combo.moves.strength"));
            for(int i=0;i<3;++i) if(Chip(names[i],step.buttons==each[i]))
                ChangeMove(index,[&](combo::Step& changed){ changed.buttons=each[i]; changed.need=1; });
            if((step.charge||step.motion.size()>1)&&Chip("EX",step.buttons==all&&step.need==2))
                ChangeMove(index,[&](combo::Step& changed){ changed.buttons=all; changed.need=2; });
            ImGui::NewLine();
        }
        // A normal: standing, crouching or jumping.
        if(!step.charge&&(step.air?step.motion.empty():step.motion=="5"||step.motion=="2")) {
            const int stance=step.air?2:step.motion=="2"?1:0;
            const char* const stanceNames[]={"training.combo.setup.stand","training.combo.setup.crouch","training.combo.setup.jump"};
            ImGui::TextDisabled("%s",loc::T("training.combo.moves.stance"));
            for(int kind=0;kind<3;++kind) if(Chip(loc::T(stanceNames[kind]),stance==kind))
                ChangeMove(index,[&](combo::Step& changed){ changed.air=kind==2; changed.motion=kind==2?"":kind==1?"2":"5"; changed.range=combo::Range::Any; });
            ImGui::NewLine();
        }
        ImGui::TextDisabled("%s",loc::T("training.combo.moves.offset"));
        if(Chip("-",false,step.offset>training::MinOffset)) ChangeMove(index,[](combo::Step& changed){ --changed.offset; });
        ImGui::Text("@%+d",step.offset); ImGui::SameLine();
        if(Chip("+",false,step.offset<training::MaxOffset)) ChangeMove(index,[](combo::Step& changed){ ++changed.offset; });
        ImGui::NewLine();
        // Anything else about it is typed.
        if(ImGui::SmallButton(loc::T("training.combo.moves.type"))) { nav.Focus("cm-"+std::to_string(index),entries); action=nav.Choose(entries); }
        ImGui::PopID();
        ImGui::Separator();
    }
    // The screen's own rows, as the list would show them: the keys walk these and the moves.
    for(const auto& entry:entries) {
        if(RowIndex(entry.id,"cm-")>=0) continue;
        ImGui::PushID(entry.id.c_str());
        ImGui::BeginDisabled(!entry.enabled);
        if(entry.adjustable) for(int delta=-1;delta<=1;delta+=2) {
            if(ImGui::SmallButton(delta<0?"<":">")) { action.kind=MenuAction::Adjust; action.id=entry.id; action.delta=delta; }
            ImGui::SameLine();
        }
        if(ImGui::Selectable((entry.value.empty()?entry.label:entry.label+": "+entry.value).c_str(),entry.id==nav.Focus())) {
            nav.Focus(entry.id,entries);
            if(!entry.adjustable) action=nav.Choose(entries);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("cm-addable",ImVec2(0,columns),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoNavInputs);
    ImGui::TextDisabled("%s",loc::T("training.combo.moves.addable"));
    for(int i=0;i<static_cast<int>(addable.moves.size());++i) {
        const auto& move=addable.moves[i];
        ImGui::PushID(i);
        const ImVec2 at=ImGui::GetCursorScreenPos();
        if(ImGui::Selectable("##add",false,ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0,rowHeight))&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) addAfter(rowEdit.row,move.first);
        if(ImGui::BeginDragDropSource()) { ImGui::SetDragDropPayload("EmberAdd",&i,sizeof i); ImGui::TextUnformatted(move.first.c_str()); ImGui::EndDragDropSource(); }
        const float y=at.y+(rowHeight-line)/2, used=DrawComboStep(move.first,ImVec2(at.x+pad,y),text);
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x+pad+used+line*.6f,y),dim,(move.first+(move.second.empty()?"":"  "+move.second)).c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    // What the focused row says about itself; on the first row, the screen.
    ImGui::BeginChild("cm-detail",ImVec2(0,detail),0,ImGuiWindowFlags_NoNavInputs);
    const auto focused=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& entry){ return entry.id==nav.Focus(); });
    ImGui::PushTextWrapPos(0);
    if(focused!=entries.end()) ImGui::TextDisabled("%s",focused->detail.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}
std::vector<MenuEntry> TimingRows(const training::View& view) {
    const auto steps=TimingSteps();
    std::vector<MenuEntry> rows{InfoRow("ct-about",loc::T("training.guide"),"",loc::T("training.combo.timing.guide")),
        Row("ct-replay",loc::T("training.combo.replay"),loc::T("training.combo.timing.replay.detail"),!steps.empty()),
        Row("ct-tune",loc::T(tune.on?"training.combo.tune.stop":"training.combo.tune"),loc::T("training.combo.tune.detail"),!steps.empty())};
    EditRows(rows,"ct-",static_cast<int>(steps.size()));
    if(steps.empty()) { rows.push_back(InfoRow("ct-none",loc::T("training.combo.combo"),loc::T("training.combo.none"),loc::T("training.combo.timing.select"))); return rows; }
    for(std::size_t i=0;i<steps.size();++i) {
        combo::Step step; std::string error; combo::ParseStep(steps[i],step,error);
        std::string seen=loc::T("training.combo.timing.none");
        if(i<view.replay.size()) {
            const auto& report=view.replay[i];
            const char* hit=loc::T(report.hit?"training.combo.timing.hit":"training.combo.timing.no_hit");
            if(!report.cued) seen=loc::T("training.combo.timing.gave_up");
            else if(i==0) seen=loc::Tf("training.combo.timing.first",hit);
            else seen=loc::Tf(step.cancel?"training.combo.timing.cancel":"training.combo.timing.link",report.waited,hit);
        }
        if(i<view.trial.drops.size()&&view.trial.drops[i]) seen+=" "+loc::Tf("training.combo.timing.drops",view.trial.drops[i]);
        rows.push_back(Value("ct-"+std::to_string(i),std::to_string(i+1)+". "+steps[i],(step.offset>0?"@+":"@")+std::to_string(step.offset),seen+" "+loc::T("training.combo.timing.adjust")));
    }
    return rows;
}
void HandleTiming(const MenuAction& a,const training::View& view,const TrainingSubmit& submit) {
    auto steps=TimingSteps();
    if(a.kind==MenuAction::Activate&&a.id=="ct-replay") Replay(steps,view,submit);
    if(a.kind==MenuAction::Activate&&a.id=="ct-tune") { if(tune.on) { tune.on=false; ComboNotice(loc::T("training.combo.tune.stopped")); } else StartTune(view,submit); return; }
    if(a.id=="ct-add"||a.id=="ct-duplicate"||a.id=="ct-remove") { MenuAction edit=a; edit.id="cm-"+a.id.substr(3); HandleMoves(edit,view,submit); return; }
    if(a.kind!=MenuAction::Adjust||RowIndex(a.id,"ct-")<0) return;
    const auto index=static_cast<std::size_t>(RowIndex(a.id,"ct-"));
    if(index>=steps.size()) return;
    combo::Step step; std::string error;
    if(!combo::ParseStep(steps[index],step,error)) return;
    SetOffset(steps,index,step.offset+a.delta);
}
// Tune: replays rep by rep, lengthening each follow-up ("~") a frame at a
// time while the move after it still connects, and keeps the longest that
// does. One replay per try; the result is read from the replay report.
// ponytail: tunes follow-ups only; links and cancels have their cues.
std::size_t NextFollow(const std::vector<std::string>& steps,std::size_t from) {
    for(std::size_t i=from;i+1<steps.size();++i) { combo::Step step; std::string error; if(combo::ParseStep(steps[i],step,error)&&step.follow) return i; }
    return steps.size();
}
void StartTune(const training::View& view,const TrainingSubmit& submit) {
    const auto steps=TimingSteps();
    tune=Tune{}; tune.step=NextFollow(steps,0);
    if(tune.step>=steps.size()) { ComboNotice(loc::T("training.combo.tune.none"),true); return; }
    combo::Step step; std::string error; combo::ParseStep(steps[tune.step],step,error);
    tune.on=true; tune.candidate=tune.start=step.offset;
    ComboNotice(loc::Tf("training.combo.tune.trying",static_cast<int>(tune.step)+1,tune.candidate));
}
void TickTune(const training::View& view,const TrainingSubmit& submit) {
    if(!tune.on) return;
    auto steps=TimingSteps();
    if(tune.step>=steps.size()) { tune.on=false; return; }
    if(!tune.started) {
        // Try the candidate: the prefix through the move after the follow-up.
        SetOffset(steps,tune.step,tune.candidate); steps=TimingSteps();
        std::vector<std::string> prefix(steps.begin(),steps.begin()+tune.step+2);
        Replay(prefix,view,submit);
        if(view.mode!=Mode::Playback&&creator.failed) { tune.on=false; return; }
        tune.started=true; tune.settle=0; return;
    }
    if(view.mode==Mode::Playback) { tune.settle=1; return; }
    if(tune.settle==0) return; // Not started yet.
    // Idle after playing: give the last press half a second to land.
    if(++tune.settle<30) return;
    const bool hit=!view.replay.empty()&&view.replay.back().hit;
    tune.started=false;
    if(hit) {
        // Longer while it still connects; the first miss after a hit is the ceiling.
        tune.best=tune.candidate;
        if(tune.candidate<training::MaxOffset) { ++tune.candidate; ComboNotice(loc::Tf("training.combo.tune.trying",static_cast<int>(tune.step)+1,tune.candidate)); return; }
    } else if(tune.best<training::MinOffset&&tune.candidate>tune.start-TuneReach&&tune.candidate>training::MinOffset) {
        // Nothing has connected yet: shorter until something does, within reach.
        --tune.candidate; ComboNotice(loc::Tf("training.combo.tune.trying",static_cast<int>(tune.step)+1,tune.candidate)); return;
    }
    if(tune.best<training::MinOffset) {
        // This rep cannot connect at any run length: the loop ends here. Put the offset back and stop.
        SetOffset(steps,tune.step,tune.start); tune.on=false;
        ComboNotice(loc::Tf("training.combo.tune.failed",static_cast<int>(tune.step)+1),true); return;
    }
    SetOffset(steps,tune.step,tune.best);
    ComboNotice(loc::Tf("training.combo.tune.kept",static_cast<int>(tune.step)+1,tune.best));
    tune.step=NextFollow(TimingSteps(),tune.step+1); tune.best=training::MinOffset-1;
    if(tune.step>=TimingSteps().size()) { tune.on=false; ComboNotice(loc::T("training.combo.tune.done")); return; }
    combo::Step step; std::string error; combo::ParseStep(TimingSteps()[tune.step],step,error); tune.candidate=tune.start=step.offset;
}
// The pattern editor's moves: the typed line or the selected combo, taken
// when the screen opens and written back after every change.
std::vector<std::string> pattern;
void StorePattern() { StoreSteps(pattern); }
void HandlePattern(const MenuAction& a,const training::View& view,const TrainingSubmit& submit) {
    if(a.kind==MenuAction::Adjust&&a.id.compare(0,4,"blk-")==0) {
        if(NudgePattern(pattern,static_cast<std::size_t>(std::atoi(a.id.c_str()+4)),a.delta)) StorePattern();
    } else if(a.kind==MenuAction::TextAccepted&&a.id=="blk-add") {
        const auto* fighter=selection::FindFighter(creator.fighter); std::string error;
        if(AddToPattern(pattern,a.text,fighter?fighter->code:"",error)) { StorePattern(); ComboNotice({}); }
        else ComboNotice(loc::Tf("training.combo.invalid",error),true);
    } else if(a.kind==MenuAction::Activate&&a.id=="blk-delete") {
        if(FocusedPatternBlock()<pattern.size()) { pattern.erase(pattern.begin()+FocusedPatternBlock()); StorePattern(); }
    } else if(a.kind==MenuAction::Activate&&a.id=="blk-replay") Replay(pattern,view,submit);
}
// Record combo: once the capture ends, the moves become the typed line.
bool captureWanted=false;
void TakeCapture(const training::View& view) {
    if(!captureWanted||view.capturing) return;
    captureWanted=false;
    const auto* fighter=selection::FindFighter(creator.fighter); combo::Fighter files; std::string error;
    if(!fighter||!combo::LoadFighter(GameFolder(),fighter->code,files,error)) { ComboNotice(loc::Tf("training.combo.game_file",error),true); return; }
    // Each move keeps how many frames after its cue it was pressed, as "@N",
    // so the replay keeps the recorded timing against the same cues.
    std::vector<std::string> steps; int unnamed=0;
    for(const auto& event:view.captured) {
        auto text=combo::ActionStep(files.moves,event.action,event.cancel&&!steps.empty());
        if(text.empty()) { ++unnamed; continue; }
        combo::Step step; std::string ignored;
        if(event.offset!=training::NoOffset&&combo::ParseStep(text,step,ignored)) {
            step.offset=(std::max)(training::MinOffset,(std::min)(training::MaxOffset,event.offset));
            text=combo::Canonical(step);
        }
        steps.push_back(text);
    }
    combo::FoldFadc(steps);
    if(steps.empty()) { ComboNotice(loc::T("training.combo.capture_empty"),true); return; }
    creator.steps=combo::JoinSteps(steps);
    ComboNotice(loc::Tf("training.combo.captured",static_cast<int>(steps.size()),unnamed));
}
// The choices of each setting as the menu numbers them, -1 first for the
// game's own setting; and their names.
const std::vector<int>& SetupChoices(int kind) {
    static const std::vector<int> choices[5]={{-1,0,1,2,3},{-1,0,1,2,3},{-1,0,1,2},{-1,0,1,2,3},{-1,0,5,7,8}};
    return choices[kind];
}
const char* SetupLabel(int kind,int value) {
    static const char* const names[5][5]={
        {"training.combo.setup.stand","training.combo.setup.crouch","training.combo.setup.jump","training.combo.setup.cpu",""},
        {"training.combo.setup.guard_none","training.combo.setup.guard_first","training.combo.setup.guard_all","training.combo.setup.guard_random",""},
        {"training.combo.setup.counter_off","training.combo.setup.counter_on","training.combo.setup.counter_random","",""},
        {"training.combo.setup.quick_quick","training.combo.setup.quick_normal","training.combo.setup.quick_delayed","training.combo.setup.quick_random",""},
        {"training.combo.setup.gauge_normal","training.combo.setup.gauge_max","training.combo.setup.gauge_infinite","training.combo.setup.gauge_refill",""}};
    const auto& choices=SetupChoices(kind);
    for(std::size_t i=1;i<choices.size();++i) if(choices[i]==value) return loc::T(names[kind][i-1]);
    return loc::T("training.combo.setup.game");
}
// Left and Right step a setting through its choices.
int StepSetup(int kind,int value,int delta) {
    const auto& choices=SetupChoices(kind);
    std::size_t at=0;
    for(std::size_t i=0;i<choices.size();++i) if(choices[i]==value) at=i;
    at=(at+choices.size()+(delta>0?1:choices.size()-1))%choices.size();
    return choices[at];
}
// A hotkey's key as the hint and its row show it.
std::string KeyName(int which,const char* none=nullptr) { return creator.keys[which]<0?none?none:loc::T("common.off"):"F"+std::to_string(creator.keys[which]+1); }
constexpr const char* ReplyNames[]={"common.off","training.combo.reply.hit","training.combo.reply.block","training.combo.reply.rise","training.combo.reply.any"};
std::vector<MenuEntry> ComboRows(const training::View& view,bool trialRunning) {
    LoadCombos();
    const auto* pack=FighterPackShown(); const auto* shown=CurrentCombo();
    const auto* fighter=selection::FindFighter(creator.fighter);
    // The fighter's packs, and the fighter's combos in the selected one.
    int packs=0, combos=0;
    for(const auto& one:creator.packs) packs+=FighterPack(one);
    if(pack) for(const auto& one:pack->combos) combos+=one.character==FighterCode();
    const char* none=loc::T("training.combo.none");
    // Pack and combo names are the player's own text and may be elided.
    auto userText=[](MenuEntry e){e.userText=true;return e;};
    auto reading=[](MenuEntry e){e.reading=true;return e;};
    auto detailed=[](MenuEntry e,const char* detail){e.detail=detail;return e;};
    // Add combo asks for the new combo's name, offering the one in the Name field.
    auto named=[](MenuEntry e){e.draft=creator.name;e.blankDraft=true;e.userText=true;e.detail=loc::T("training.combo.add.detail");return e;};
    // The Moves line is edited a move a line; a new line separates moves as ">" does.
    auto area=[](MenuEntry e){e.multiline=true;for(const auto& move:combo::Tokens(e.value))e.draft+=(e.draft.empty()?"":"\n")+move;e.detail=loc::T("training.combo.steps.detail");return e;};
    return {
        InfoRow("cb-about",loc::T("training.guide"),"",loc::T("training.combo.guide")),
        Value("cb-fighter",loc::T("training.combo.fighter"),fighter?fighter->name:"",loc::T("training.combo.fighter.detail")),
        userText(Value("cb-pack",loc::T("training.combo.pack"),pack?pack->name:none,loc::Tf("training.combo.pack.detail",combos),packs>(pack?1:0))),
        TextRow("cb-new-pack",loc::T("training.combo.new_pack"),"",64),
        userText(Value("cb-combo",loc::T("training.combo.combo"),shown?shown->name.empty()?combo::JoinSteps(shown->steps):shown->name:none,
            shown?std::string(FighterName(shown->character))+": "+combo::JoinSteps(shown->steps)+(shown->notes.empty()?"":"\n"+shown->notes):loc::T("training.combo.tree.empty"),combos>1)),
        Row("cb-start-trial",loc::T("training.combo.run.start"),loc::T("training.combo.run.start.detail"),shown!=nullptr),
        Row("cb-stop-trial",loc::T("training.combo.run.stop"),loc::T("training.combo.run.stop.detail"),trialRunning),
        TextRow("cb-name",loc::T("training.combo.name"),creator.name,64),
        area(TextRow("cb-steps",loc::T("training.combo.steps"),creator.steps,8192)),
        TextRow("cb-notes",loc::T("training.combo.notes"),creator.notes,256),
        named(TextRow("cb-add",loc::T("training.combo.add"),creator.name.empty()?loc::T("training.combo.add.unnamed"):creator.name,64,!creator.steps.empty())),
        Row("cb-save",loc::T("training.combo.save"),loc::T("training.combo.save.detail"),shown!=nullptr&&!creator.steps.empty()),
        Row("cb-duplicate",loc::T("training.combo.duplicate"),loc::T("training.combo.duplicate.detail"),shown!=nullptr),
        Row("cb-capture",loc::T(view.capturing?"training.combo.capture_stop":"training.combo.capture"),loc::T("training.combo.capture.detail"),view.ready),
        Value("cb-capture-drop",loc::T("training.combo.capture.drop"),loc::T(creator.captureStopOnDrop?"common.on":"common.off"),loc::T("training.combo.capture.drop.detail")),
        Row("cb-replay",loc::T("training.combo.replay"),loc::T("training.combo.replay.detail"),!creator.steps.empty()||shown!=nullptr),
        Row("cb-moves",loc::T("training.combo.moves"),loc::T("training.combo.moves.detail"),!creator.steps.empty()||shown!=nullptr),
        Row("cb-timing",loc::T("training.combo.timing"),loc::T("training.combo.timing.detail"),!creator.steps.empty()||shown!=nullptr),
        Row("cb-blocks",loc::T("training.combo.blocks"),loc::T("training.combo.blocks.detail")),
        Row("cb-save-pos",loc::T("training.combo.save_pos"),loc::T("training.combo.save_pos.detail"),view.ready),
        Row("cb-reset-pos",loc::T("training.combo.reset_pos"),loc::T("training.combo.reset_pos.detail"),view.ready&&(view.checkpoint||(shown&&shown->placed)||creator.side)),
        Value("cb-reset-before",loc::T("training.combo.reset_before"),loc::T(creator.resetBeforeReplay?"common.on":"common.off"),loc::T("training.combo.reset_before.detail")),
        Value("cb-side",loc::T("training.combo.side"),loc::T(creator.side==1?"training.combo.side.left":creator.side==2?"training.combo.side.right":"training.combo.side.any"),loc::T("training.combo.side.detail")),
        Value("cb-setup-action",loc::T("training.combo.setup.action"),SetupLabel(0,creator.setup.action),loc::T("training.combo.setup.detail")),
        Value("cb-setup-guard",loc::T("training.combo.setup.guard"),SetupLabel(1,creator.setup.guard),loc::T("training.combo.setup.detail")),
        Value("cb-setup-counter",loc::T("training.combo.setup.counter"),SetupLabel(2,creator.setup.counterHit),loc::T("training.combo.setup.detail")),
        Value("cb-setup-quick",loc::T("training.combo.setup.quick"),SetupLabel(3,creator.setup.quickStand),loc::T("training.combo.setup.detail")),
        Value("cb-setup-super",loc::T("training.combo.setup.super"),SetupLabel(4,creator.setup.super),loc::T("training.combo.setup.detail")),
        Value("cb-setup-revenge",loc::T("training.combo.setup.revenge"),SetupLabel(4,creator.setup.revenge),loc::T("training.combo.setup.detail")),
        Value("cb-reply",loc::T("training.combo.reply"),loc::T(ReplyNames[creator.plan.when]),loc::T("training.combo.reply.detail")),
        detailed(TextRow("cb-reply-moves",loc::T("training.combo.reply.moves"),creator.replyMoves,256),loc::T("training.combo.reply.moves.detail")),
        Value("cb-reply-timing",loc::T("training.combo.reply.timing"),(creator.plan.timing>0?"+":"")+std::to_string(creator.plan.timing)+" f",loc::T("training.combo.reply.timing.detail"),creator.plan.when!=0),
        Value("cb-reply-slot",loc::T("training.combo.reply.slot"),loc::Tf("training.slot",creator.plan.slot+1)+" ("+loc::Tf("training.recorded_frames",view.lengths[creator.plan.slot])+")",loc::T("training.combo.reply.slot.detail"),creator.plan.when!=0&&creator.replyMoves.empty()),
        Value("cb-reply-chance",loc::T("training.combo.reply.chance"),std::to_string(creator.plan.chance)+"%",loc::T("training.combo.reply.chance.detail"),creator.plan.when!=0),
        Value("cb-vary-stance",loc::T("training.combo.vary_stance"),loc::T(creator.plan.varyStance?"common.on":"common.off"),loc::T("training.combo.vary_stance.detail")),
        Value("cb-replay-by",loc::T("training.combo.replay.by"),loc::T(creator.replayBy?"training.combo.replay.by.dummy":"training.combo.replay.by.me"),loc::T("training.combo.replay.by.detail")),
        Value("cb-replay-facing",loc::T("training.combo.replay.facing"),loc::T(creator.replayFacingRight?"training.combo.replay.facing.right":"training.combo.replay.facing.left"),loc::T("training.combo.replay.facing.detail")),
        Value("cb-replay-gap",loc::T("training.combo.replay.gap"),(creator.replayOffset>0?"+":"")+std::to_string(creator.replayOffset),loc::T("training.combo.replay.gap.detail")),
        Value("cb-key-0",loc::T("training.combo.key.replay"),KeyName(0),loc::T("training.combo.key.detail")),
        Value("cb-key-1",loc::T("training.combo.key.reset"),KeyName(1),loc::T("training.combo.key.detail")),
        Value("cb-key-2",loc::T("training.combo.key.trial"),KeyName(2),loc::T("training.combo.key.detail")),
        Value("cb-key-3",loc::T("training.combo.key.record"),KeyName(3),loc::T("training.combo.key.detail")),
        Value("cb-key-4",loc::T("training.combo.key.save"),KeyName(4),loc::T("training.combo.key.save.detail")),
        // Named by the row it presses, so it needs no words of its own.
        Value("cb-key-5",loc::T("training.combo.save_pos"),KeyName(5),loc::T("training.combo.key.detail")),
        reading(Row("cb-tree",loc::T("training.combo.tree"),loc::T("training.combo.tree.detail"))),
        Row("cb-copy-combo",loc::T("training.combo.copy_combo"),loc::T("training.combo.copy.detail"),shown!=nullptr),
        Row("cb-copy-pack",loc::T("training.combo.copy_pack"),loc::T("training.combo.copy.detail"),pack!=nullptr),
        Row("cb-copy-all",loc::T("training.combo.copy_all"),loc::T("training.combo.copy.detail"),pack!=nullptr),
        Row("cb-paste",loc::T("training.combo.paste"),loc::T("training.combo.paste.detail")),
        Row("cb-import-trials",loc::T("training.combo.import_trials"),loc::T("training.combo.import_trials.detail"),fighter!=nullptr),
        Row("cb-export-trial",loc::T("training.combo.export_trial"),
            loc::Tf("training.combo.export_trial.detail",fighter?combo::TrialFileName(fighter->code):""),fighter&&pack&&!creator.directory.empty()),
        ConfirmRow("cb-delete",loc::T("training.combo.delete"),loc::T("training.combo.delete.detail"),shown!=nullptr)};
}
// Turns the pack and combo rows to the selected fighter: a combo of theirs
// already shown stays, else the selected pack's first, else the first pack
// that has one. False when no pack does.
bool SelectFighterPack() {
    if(CurrentCombo()) return true;
    int pack=CurrentPack()&&FighterEntry(*CurrentPack())>=0?creator.pack:-1;
    for(std::size_t i=0;pack<0&&i<creator.packs.size();++i) if(FighterEntry(creator.packs[i])>=0) pack=static_cast<int>(i);
    if(pack<0) return false;
    creator.pack=pack; creator.entry=FighterEntry(creator.packs[pack]); ShowCombo();
    return true;
}
// The fields as typed become a new combo of the selected pack.
void AddTyped() {
    const auto* pack=FighterPackShown(); const auto* fighter=selection::FindFighter(creator.fighter); std::string error;
    combo::Combo made{creator.name,fighter?fighter->code:"",creator.notes,{},creator.setup};
    if(!combo::ParseSteps(creator.steps,made.character,made.steps,error)||made.steps.empty()||made.steps.size()>combo::MaxSteps) {
        ComboNotice(loc::Tf("training.combo.invalid",error.empty()?creator.steps:error),true); return;
    }
    // The line is shown back as it was understood.
    creator.steps=combo::JoinSteps(made.steps);
    // Without a pack of this fighter's, one named after the fighter takes it.
    AddCombos({combo::Pack{pack?pack->name:fighter?std::string(fighter->name)+" combos":DefaultPack,{made}}});
}
void HandleCombo(const MenuAction& a,const training::View& view,const TrainingSubmit& submit) {
    auto* pack=CurrentPack();
    if(a.kind==MenuAction::Adjust) {
        const int way=a.delta>0?1:-1;
        // Left and Right pass over what is another fighter's.
        if(a.id=="cb-pack"&&pack) {
            for(int i=creator.pack+way;i>=0&&i<static_cast<int>(creator.packs.size());i+=way) if(FighterPack(creator.packs[i])) {
                creator.pack=i; creator.entry=(std::max)(0,FighterEntry(creator.packs[i])); ShowCombo(); break;
            }
        }
        else if(a.id=="cb-combo"&&pack) { const int next=FighterEntry(*pack,creator.entry+way,way); if(next>=0) { creator.entry=next; ShowCombo(); } }
        else if(a.id=="cb-fighter") { creator.fighter=(creator.fighter+(a.delta>0?1:selection::FighterCount-1))%selection::FighterCount; SelectFighterPack(); }
        else if(a.id=="cb-side") { creator.side=(creator.side+3+way)%3; SavePractice(); }
        // The dummy starts on the right facing left, the player on the left facing right.
        else if(a.id=="cb-replay-by") { creator.replayBy=!creator.replayBy; creator.replayFacingRight=!creator.replayBy; }
        else if(a.id=="cb-replay-facing") creator.replayFacingRight=!creator.replayFacingRight;
        else if(a.id=="cb-replay-gap") creator.replayOffset=(std::max)(training::MinOffset,(std::min)(training::MaxOffset,creator.replayOffset+a.delta));
        else if(a.id=="cb-reset-before") creator.resetBeforeReplay=a.delta>0;
        else if(a.id=="cb-capture-drop") { creator.captureStopOnDrop=a.delta>0; SavePractice(); }
        else if(a.id=="cb-reply"||a.id=="cb-reply-slot"||a.id=="cb-reply-chance"||a.id=="cb-reply-timing"||a.id=="cb-vary-stance") {
            const int step=a.delta>0?1:-1;
            if(a.id=="cb-reply") creator.plan.when=(creator.plan.when+5+step)%5;
            else if(a.id=="cb-reply-slot") creator.plan.slot=(creator.plan.slot+SlotCount+step)%SlotCount;
            else if(a.id=="cb-reply-chance") creator.plan.chance=(std::max)(25,(std::min)(100,creator.plan.chance+25*step));
            else if(a.id=="cb-reply-timing") creator.plan.timing=(std::max)(-MaxReplyTiming,(std::min)(MaxReplyTiming,creator.plan.timing+step));
            else creator.plan.varyStance=a.delta>0;
            SavePractice();
            if(!SendPlan(view,submit)) ComboNotice(loc::T("training.command_rejected"),true);
        }
        else if(a.id.compare(0,7,"cb-key-")==0) {
            // The keys Ember and Steam leave free; one key does one thing.
            static const int choices[]={-1,0,1,2,3,8,10};
            const int which=a.id[7]-'0', count=static_cast<int>(sizeof(choices)/sizeof(*choices));
            int at=0;
            for(int i=0;i<count;++i) if(choices[i]==creator.keys[which]) at=i;
            for(int tries=0;tries<count;++tries) {
                at=(at+count+(a.delta>0?1:-1))%count;
                bool taken=false;
                for(int other=0;other<6;++other) taken=taken||(other!=which&&choices[at]>=0&&creator.keys[other]==choices[at]);
                if(!taken) break;
            }
            creator.keys[which]=choices[at]; SavePractice();
        }
        else if(a.id.compare(0,9,"cb-setup-")==0) {
            const std::string which=a.id.substr(9);
            if(which=="action") creator.setup.action=StepSetup(0,creator.setup.action,a.delta);
            else if(which=="guard") creator.setup.guard=StepSetup(1,creator.setup.guard,a.delta);
            else if(which=="counter") creator.setup.counterHit=StepSetup(2,creator.setup.counterHit,a.delta);
            else if(which=="quick") creator.setup.quickStand=StepSetup(3,creator.setup.quickStand,a.delta);
            else if(which=="super") creator.setup.super=StepSetup(4,creator.setup.super,a.delta);
            else if(which=="revenge") creator.setup.revenge=StepSetup(4,creator.setup.revenge,a.delta);
            // Into the game now, and kept with the selected combo.
            ApplySetup(view,submit);
            if(auto* shown=CurrentCombo()) { shown->setup=creator.setup; SaveCombos(); }
        }
        return;
    }
    if(a.kind==MenuAction::TextAccepted) {
        if(a.id=="cb-name") creator.name=combo::Clean(a.text);
        else if(a.id=="cb-add") { creator.name=combo::Clean(a.text); AddTyped(); }
        else if(a.id=="cb-steps") creator.steps=combo::JoinSteps(combo::Tokens(a.text));
        else if(a.id=="cb-notes") creator.notes=combo::Clean(a.text);
        else if(a.id=="cb-reply-moves") {
            std::vector<std::string> steps; std::string error; const auto text=combo::Clean(a.text);
            if(!text.empty()&&!combo::ParseSteps(text,"",steps,error)) { ComboNotice(loc::Tf("training.combo.invalid",error.empty()?text:error),true); return; }
            creator.replyMoves=text; SavePractice();
            if(!SendPlan(view,submit)) ComboNotice(loc::T("training.command_rejected"),true);
        }
        else if(a.id=="cb-new-pack"&&!combo::Clean(a.text).empty()) {
            const auto name=combo::Clean(a.text);
            auto found=std::find_if(creator.packs.begin(),creator.packs.end(),[&](const combo::Pack& p){return p.name==name;});
            if(found==creator.packs.end()&&creator.packs.size()<combo::MaxPacks) { creator.packs.push_back({name,{}}); found=creator.packs.end()-1; SaveCombos(); }
            if(found!=creator.packs.end()) { creator.pack=static_cast<int>(found-creator.packs.begin()); creator.entry=0; }
        }
        return;
    }
    if(a.kind!=MenuAction::Activate) return;
    std::string error;
    if(a.id=="cb-add") AddTyped();
    else if(a.id=="cb-save"&&CurrentCombo()) {
        // The fields as typed go over the selected combo; its place stays.
        auto* shown=CurrentCombo();
        const auto* fighter=selection::FindFighter(creator.fighter);
        combo::Combo made{creator.name,fighter?fighter->code:"",creator.notes,{},creator.setup,shown->placed,shown->place};
        if(!combo::ParseSteps(creator.steps,made.character,made.steps,error)||made.steps.empty()||made.steps.size()>combo::MaxSteps) {
            ComboNotice(loc::Tf("training.combo.invalid",error.empty()?creator.steps:error),true); return;
        }
        *shown=made; creator.steps=combo::JoinSteps(made.steps); SaveCombos();
        ComboNotice(loc::T("training.combo.saved"));
    } else if(a.id=="cb-duplicate"&&pack&&CurrentCombo()) {
        // A copy right after the original, selected, so a variant can be tuned.
        combo::Combo copy=*CurrentCombo(); copy.name=combo::Clean(copy.name+" "+loc::T("training.combo.copy_suffix"));
        if(pack->combos.size()>=combo::MaxCombos) { ComboNotice(loc::T("training.combo.save_failed"),true); return; }
        pack->combos.insert(pack->combos.begin()+creator.entry+1,copy); ++creator.entry; ShowCombo(); SaveCombos();
        ComboNotice(loc::T("training.combo.duplicated"));
    } else if(a.id=="cb-replay") {
        // The typed line when there is one, else the selected combo.
        std::vector<std::string> steps;
        const auto* fighter=selection::FindFighter(creator.fighter);
        if(!creator.steps.empty()) { if(!combo::ParseSteps(creator.steps,fighter?fighter->code:"",steps,error)) { ComboNotice(loc::Tf("training.combo.invalid",error),true); return; } }
        else if(CurrentCombo()) steps=CurrentCombo()->steps;
        Replay(steps,view,submit);
    } else if(a.id=="cb-paste") {
        std::vector<combo::Pack> incoming;
        const char* text=ImGui::GetClipboardText();
        if(!text||!combo::Import(text,incoming,error)) { ComboNotice(loc::Tf("training.combo.invalid",error),true); return; }
        // A lone combo arrives without a pack: it joins the selected one.
        if(incoming.size()==1&&incoming[0].name.empty()) incoming[0].name=pack?pack->name:DefaultPack;
        AddCombos(incoming); ShowCombo();
    } else if(a.id=="cb-capture") {
        Command command; command.action=view.capturing?Action::CaptureStop:Action::CaptureStart; command.generation=view.generation;
        command.value=creator.captureStopOnDrop;
        if(submit&&submit(command)) { captureWanted=true; ComboNotice(loc::T(view.capturing?"training.combo.capture_wait":"training.combo.capture_started")); }
        else ComboNotice(loc::T("training.command_rejected"),true);
    } else if(a.id=="cb-save-pos") {
        Command command; command.action=Action::Save; command.generation=view.generation;
        const bool sent=submit&&submit(command);
        if(sent) { creator.checkpointPlaced=true; creator.checkpointPlace[0]=view.x[0]; creator.checkpointPlace[1]=view.x[1]; }
        // The place goes with the shown combo, so it comes back with it.
        if(auto* shown=CurrentCombo(); sent&&shown) { shown->placed=true; shown->place[0]=view.x[0]; shown->place[1]=view.x[1]; SaveCombos(); }
        ComboNotice(loc::T(!sent?"training.command_rejected":CurrentCombo()?"training.combo.position_kept":"training.combo.position_saved"),!sent);
    } else if(a.id=="cb-reset-pos") {
        // The saved position, then Player 1 onto the preferred side; either alone will do.
        Command command;
        const bool placed=PlaceCommand(view,command);
        const bool sent=(placed||creator.side)&&(!placed||(submit&&submit(command)))&&SendSide(view,true,submit);
        ComboNotice(loc::T(!sent?"training.command_rejected":"training.combo.position_reset"),!sent);
    } else if(a.id=="cb-start-trial") StartTrial(view,submit);
    else if(a.id=="cb-stop-trial") {
        Command command; command.action=Action::StopTrial; command.generation=view.generation;
        const bool sent=submit&&submit(command);
        ComboNotice(loc::T(sent?"training.combo.run.stopped":"training.command_rejected"),!sent);
    } else if(a.id=="cb-import-trials") ImportTrials();
    else if(a.id=="cb-export-trial") ExportTrial();
    else if(a.id=="cb-delete"&&CurrentCombo()) {
        pack->combos.erase(pack->combos.begin()+(CurrentCombo()-pack->combos.data()));
        if(pack->combos.empty()) creator.packs.erase(creator.packs.begin()+(pack-creator.packs.data()));
        creator.entry=0; ComboNotice({}); SaveCombos();
    } else if(a.id=="cb-copy-combo"||a.id=="cb-copy-pack"||a.id=="cb-copy-all") {
        if(!pack||(a.id=="cb-copy-combo"&&!CurrentCombo())) return;
        ImGui::SetClipboardText((a.id=="cb-copy-combo"?combo::Export(*CurrentCombo()):a.id=="cb-copy-pack"?combo::Export(*pack):combo::Export(creator.packs)).c_str());
        ComboNotice(loc::T("training.combo.copied"));
    }
}
}
// A battle with another fighter as Player 1: the creator takes that fighter,
// and a pack that has combos for it, the selected pack first. A combo of
// that fighter already shown stays. Without such a pack the game's own
// trials for the fighter are read in as its first packs.
void FollowBattle(const training::View& view) {
    static int followed=-1;
    if(view.fighters[0]<0||view.fighters[0]==followed) return;
    followed=view.fighters[0];
    LoadCombos();
    const auto* fighter=selection::FindFighter(followed);
    if(!fighter) return;
    creator.fighter=followed;
    if(SelectFighterPack()) return;
    // No pack for this fighter yet: the game's own trials are its first.
    int read=0;
    if(!AddGameTrials(*fighter,read)) return;
    SelectGameTrials(*fighter); SaveCombos(); ShowCombo();
    ComboNotice(loc::Tf("training.combo.trials_loaded",fighter->name,read));
}
void SetComboBookDirectory(std::wstring directory) { if(creator.directory.empty()) creator.directory=std::move(directory); }
void SetComboMoves(const std::string& line) { creator.steps=combo::JoinSteps(combo::Tokens(line)); }
void TrainingHotkeys(const training::View& view,const TrainingSubmit& submit,bool padSelect) {
    if(!view.available||ImGui::GetIO().WantTextInput||ImGui::GetIO().KeyAlt) return;
    LoadCombos();
    FollowBattle(view);
    // A capture stopped by F4 becomes the typed line here, as the combo screen does it.
    TakeCapture(view);
    // The plan is the player's, so every battle gets it again.
    static std::uint64_t sent=0;
    if(sent!=view.generation&&SendPlan(view,submit)) sent=view.generation;
    const auto row=[&](const char* id) { MenuAction a; a.kind=MenuAction::Activate; a.id=id; HandleCombo(a,view,submit); };
    const auto pressed=[](int which) { return creator.keys[which]>=0&&ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1+creator.keys[which]),false); };
    if(pressed(0)) ReplayOrStop(TimingSteps(),view,submit);
    if(pressed(1)) row("cb-reset-pos");
    if(pressed(2)) row(view.trialSteps.empty()?"cb-start-trial":"cb-stop-trial");
    if(pressed(3)) row("cb-capture");
    if(pressed(5)) row("cb-save-pos");
    // The pad's Select: a tap puts the fighters back, and held for half a
    // second it saves where they stand. Where that is, is taken as the button
    // goes down, in case the game moves them on the press.
    static double downAt=-1; static bool saved=false; static float down[2]={0,0};
    const double now=ImGui::GetTime();
    if(padSelect&&downAt<0) { downAt=now; saved=false; down[0]=view.x[0]; down[1]=view.x[1]; }
    if(padSelect&&!saved&&now-downAt>=.5) {
        saved=true;
        Command place; place.action=Action::Place; place.generation=view.generation; place.place[0]=down[0]; place.place[1]=down[1];
        if(submit) submit(place);
        row("cb-save-pos");
        if(creator.checkpointPlaced) { creator.checkpointPlace[0]=down[0]; creator.checkpointPlace[1]=down[1]; }
    }
    if(!padSelect&&downAt>=0) { if(!saved&&now-downAt<.5) row("cb-reset-pos"); downAt=-1; }
    // Save: the Moves line becomes a new combo of the pack. It never writes over one.
    if(pressed(4)) {
        const auto* shown=CurrentCombo();
        if(creator.steps.empty()) ComboNotice(loc::T("training.combo.key.save.none"),true);
        else if(shown&&combo::JoinSteps(shown->steps)==creator.steps) ComboNotice(loc::T("training.combo.saved"));
        else {
            // The selected combo's name stays its own; the new one shows its moves until it is named.
            if(shown&&creator.name==shown->name) creator.name.clear();
            row("cb-add");
        }
    }
}
bool TrainingHotkeyBound(int fromF1) { return std::find(std::begin(creator.keys),std::end(creator.keys),fromF1)!=std::end(creator.keys)&&fromF1>=0; }
namespace {
// Player 1 standing right of the other fighter presses every direction the
// other way round: the lists then draw their arrows so, and the trial's list
// stands on that side. Decided between attempts, so a cross-up inside a
// combo does not turn the lists over.
bool PlayerOneOnRight(const training::View& view) {
    static bool right=false;
    if(view.trialSteps.empty()||view.trial.current<=0||view.trial.complete) right=view.x[0]>view.x[1];
    return right;
}
// The combo in hand as one line above the HUD's chips: the running trial's
// moves from the one being waited for, else the typed line or the selected
// combo; under it what the last hotkey or attempt did. It stays beside
// either trial list: the eye is on the meter while a combo is timed.
void ComboStrip(const training::View& view,float width) {
    LoadCombos();
    const bool running=!view.trialSteps.empty()&&view.trialSteps.size()==view.trial.steps.size();
    const bool mirror=PlayerOneOnRight(view);
    const auto steps=running?view.trialSteps:TimingSteps();
    if(steps.empty()&&creator.notice.empty()) return;
    const float h=ImGui::GetTextLineHeight();
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::PushClipRect(origin,ImVec2(origin.x+width,origin.y+h),true);
    float x=0;
    for(std::size_t i=running?static_cast<std::size_t>((std::max)(0,view.trial.current-1)):0;i<steps.size()&&x<width;++i) {
        const auto state=running?view.trial.steps[i]:TrialStepState::Waiting;
        const ImU32 tint=!running?ImGui::GetColorU32(ImGuiCol_Text):state==TrialStepState::Done?IM_COL32(118,224,160,255):state==TrialStepState::Out?palette::Ember:
            static_cast<int>(i)==view.trial.current?ImGui::GetColorU32(ImGuiCol_Text):ImGui::GetColorU32(ImGuiCol_TextDisabled);
        x+=DrawComboStep(steps[i],ImVec2(origin.x+x,origin.y),tint,mirror)+h*.5f;
    }
    ImGui::PopClipRect();
    ImGui::Dummy(ImVec2(width,h));
    const auto tally=running?loc::Tf("training.combo.run.count",view.trial.successes,view.trial.attempts,view.trial.attempts?100*view.trial.successes/view.trial.attempts:0):std::string();
    const auto status=FitLabel(creator.notice.empty()?tally.empty()?" ":tally:creator.notice,width);
    if(creator.failed&&!creator.notice.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(255,121,129,255)),"%s",status.c_str());
    else ImGui::TextDisabled("%s",status.c_str());
}
}

// The recording library: slots saved by name beside the combo book.
std::vector<std::string> recordings; std::size_t recordingChoice=0;
std::string savePending; std::uint64_t exportSeen=0;
std::filesystem::path RecordingFolder() { return creator.directory/"recordings"; }
void ListRecordings() {
    recordings.clear(); std::error_code ignored;
    if(creator.directory.empty()) return;
    for(const auto& entry:std::filesystem::directory_iterator(RecordingFolder(),ignored))
        if(entry.path().extension()==".json") recordings.push_back(entry.path().stem().string());
    std::sort(recordings.begin(),recordings.end());
    if(recordingChoice>=recordings.size()) recordingChoice=0;
}
// The runtime handed over the slot asked for: write it under the name typed.
void TakeExport(const training::View& v) {
    if(savePending.empty()||v.exportId==exportSeen) return;
    exportSeen=v.exportId; const auto name=savePending; savePending.clear();
    std::error_code ignored; std::filesystem::create_directories(RecordingFolder(),ignored);
    std::string error;
    if(!netplay::json_file::Publish(RecordingFolder(),std::filesystem::path(name+".json").wstring(),nlohmann::json::parse(training::ExportRecording(v.exported),nullptr,false),error))
        ComboNotice(loc::T("training.combo.save_failed"),true);
    else ListRecordings();
}
// Save as, choose, load: the slot goes to a file by name, a file into the selected slot.
bool HandleRecordingLibrary(const MenuAction& a,const training::View& v,const TrainingSubmit& submit) {
    if(a.kind==MenuAction::TextAccepted&&a.id=="save-recording") {
        const auto name=combo::Clean(a.text);
        if(name.empty()||name.find_first_of("\\/:*?\"<>|")!=std::string::npos) { ComboNotice(loc::Tf("training.combo.invalid",name),true); return true; }
        Command command; command.action=Action::ExportSlot; command.generation=v.generation;
        if(submit&&submit(command)) { savePending=name; ComboNotice(loc::Tf("training.recording_saved",name)); }
        else ComboNotice(loc::T("training.command_rejected"),true);
        return true;
    }
    if(a.id!="load-recording") return false;
    if(a.kind==MenuAction::Adjust&&!recordings.empty()) { recordingChoice=(recordingChoice+recordings.size()+(a.delta>0?1:recordings.size()-1))%recordings.size(); return true; }
    if(a.kind!=MenuAction::Activate||recordings.empty()) return true;
    const auto name=recordings[(std::min)(recordingChoice,recordings.size()-1)];
    std::string bytes,error; bool missing=false; std::vector<training::Input> frames;
    if(!netplay::json_file::ReadBytes(RecordingFolder()/(name+".json"),bytes,missing,error)||missing||!training::ImportRecording(bytes,frames,error)) {
        ComboNotice(loc::Tf("training.combo.invalid",error),true); return true;
    }
    Command load; load.action=Action::Load; load.generation=v.generation; load.value=1; load.frames=frames;
    if(submit&&submit(load)) ComboNotice(loc::Tf("training.recording_loaded",name,v.selected+1));
    else ComboNotice(loc::T("training.command_rejected"),true);
    return true;
}
// offerOverwrite: F7 on a recorded slot opened the recordings to ask about overwriting.
namespace { GameMenu trainingMenu; bool showRecordings=false, offerOverwrite=false; }
MenuNavigation& TrainingNavigation() { return trainingMenu.navigation; }
void ShowTrainingRecordings() {showRecordings=true;}
void DrawTrainingFlyout(const training::View& view,const TrainingSubmit& submit) {
    if(!view.available)return;
    SetMenuInput({0,ImGui::GetTime()});
    SetMenuGlyphs(input::PadKeyboard,0,0);
    const auto* vp=ImGui::GetMainViewport();
    // The move editor's three columns get a wider and taller panel.
    const bool editor=trainingMenu.navigation.Screen()=="combo-moves";
    const ImVec2 size((std::min)((editor?1240:820)*Scale(),vp->Size.x*.8f),(std::min)((editor?780:600)*Scale(),vp->Size.y*.8f));
    // Compact typography independently of global DPI when the viewport cannot
    // accommodate the preferred panel. Other Ember windows keep their scale.
    const float unit=(std::min)(Scale(),(std::min)(size.x/500.f,size.y/500.f));
    // Top centre at first, then wherever it was dragged, so it can be moved
    // off the fight while a replay or trial runs under it.
    // A panel that changes size goes back there: it would not stay centred, or on the screen, otherwise.
    static bool wasEditor=false;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x*.5f,vp->Pos.y+8*unit),editor!=wasEditor?ImGuiCond_Always:ImGuiCond_FirstUseEver,ImVec2(.5f,0));
    // What the other size held says nothing of this one: no scroll range for the frame between.
    if(editor!=wasEditor) ImGui::SetNextWindowContentSize(ImVec2(size.x-32*unit,size.y-24*unit));
    wasEditor=editor;
    ImGui::SetNextWindowSize(size,ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.075f,.07f,.065f,.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(1,.53f,.22f,.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16*unit,12*unit));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10*unit,6*unit));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8*unit,6*unit));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,unit);
    const auto trainingWindow=std::string(loc::T("training.controls"))+"###TrainingControls";
    if(ImGui::Begin(trainingWindow.c_str(),nullptr,ImGuiWindowFlags_NoDecoration|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs)) {
        // Dragged out of the viewport, it comes back to the edge.
        const ImVec2 pos=ImGui::GetWindowPos();
        const ImVec2 kept((std::max)(vp->Pos.x,(std::min)(pos.x,vp->Pos.x+vp->Size.x-size.x)),(std::max)(vp->Pos.y,(std::min)(pos.y,vp->Pos.y+vp->Size.y-size.y)));
        if(kept.x!=pos.x||kept.y!=pos.y) ImGui::SetWindowPos(kept);
        ImGui::SetWindowFontScale(unit/Scale());
        // Capture remains global even though presentation is only a flyout.
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        ImGui::SetNextFrameWantCaptureMouse(true);
        DrawTrainingPanel(view,submit);
        ImGui::SetWindowFontScale(1);
    }
    ImGui::End();ImGui::PopStyleVar(4);ImGui::PopStyleColor(2);
}
void DrawTrainingPanel(const training::View& v,const TrainingSubmit& submit) {
 using namespace training;if(!v.available)return;
 auto& nav=trainingMenu.navigation;
 static std::uint64_t generation=0,nextRequest=1,pending=0;
 static bool returnAfter=false;
 static std::string error;
 static int lastFrame=-2;
 if(lastFrame!=ImGui::GetFrameCount()-1){pending=0;returnAfter=false;nav.Cancel();}
 lastFrame=ImGui::GetFrameCount();
 if(generation!=v.generation){generation=v.generation;nav.Home();nav.Cancel();pending=0;error.clear();}
 if(showRecordings){showRecordings=false;nav.Home();nav.Push("recording");offerOverwrite=true;ListRecordings();}
 if(pending&&v.commandId==pending){
  pending=0;
  if(v.commandAccepted){error.clear();if(returnAfter){ForwardMenuAction({MenuAction::Close});return;}}
  else error=v.commandError;
 }
 // Its root is the training lab; Back from there closes the controls and
 // returns to the game.
 trainingMenu.rootName=loc::T("training.lab");trainingMenu.exitName=loc::T("screen.game");
 trainingMenu.backHint=nav.Screen()==nav.Root()?loc::T("training.close_controls"):"";
 const auto screen=nav.Screen();std::vector<MenuEntry> rows;
 const bool ready=v.ready&&!pending;
 if(screen=="home"){
  rows={Row("recording",loc::T("training.dummy_recording"),loc::T("training.dummy_recording.detail")),
   Row("history",loc::T("training.input_history"),loc::T("training.input_history.detail")),
   Row("combos",loc::T("training.combo.title"),loc::T("training.combo.title.detail")),
   InfoRow("about",loc::T("training.guide"),"",loc::T("training.home.guide")),
   Row("return",loc::T("training.close_controls"),loc::T("training.close_controls.detail"))};
 }else if(screen=="recording"){
  rows.push_back(InfoRow("about",loc::T("training.guide"),"",loc::T("training.recording.guide")));
  for(int slot=0;slot<SlotCount;++slot)rows.push_back(Row("slot-"+std::to_string(slot),loc::Tf(slot==v.selected?"training.slot_selected":"training.slot",slot+1),
   loc::Tf("training.recorded_frames",v.lengths[slot]),ready&&v.mode==Mode::Idle));
  auto record=Row("record",loc::T("training.record"),loc::T(v.lengths[v.selected]?"training.record.overwrite":"training.record.detail"),ready);
  record.confirm=v.lengths[v.selected]>0;rows.push_back(record);
  rows.push_back(Row("play",loc::T("training.play"),loc::T(v.lengths[v.selected]?"training.play.detail":"training.slot_empty"),ready&&v.lengths[v.selected]>0));
  rows.push_back(Row("stop",loc::T("training.stop"),loc::T("training.stop.detail"),!pending));
  rows.push_back(Value("loop",loc::T("training.loop"),loc::T(v.loop?"common.on":"common.off"),loc::T("training.loop.detail"),!pending));
  rows.push_back(ConfirmRow("clear",loc::T("training.clear_recording"),loc::T("training.clear_recording.detail"),ready&&v.mode==Mode::Idle&&v.lengths[v.selected]>0));
  TakeExport(v);
  rows.push_back(TextRow("save-recording",loc::T("training.save_recording"),"",48,v.lengths[v.selected]>0&&!creator.directory.empty()));
  rows.back().detail=loc::T("training.save_recording.detail");
  rows.push_back(Value("load-recording",loc::T("training.load_recording"),recordings.empty()?loc::T("training.recording_none"):recordings[(std::min)(recordingChoice,recordings.size()-1)],loc::T("training.load_recording.detail"),!recordings.empty()&&ready&&v.mode==Mode::Idle));
 }else if(screen=="combos"){
  TakeCapture(v);
  rows=ComboRows(v,!v.trialSteps.empty());
 }else if(screen=="combo-timing"){
  TickTune(v,submit);
  if(RowIndex(nav.Focus(),"ct-")>=0) rowEdit.row=RowIndex(nav.Focus(),"ct-");
  rows=TimingRows(v);
 }else if(screen=="combo-moves"){
  if(RowIndex(nav.Focus(),"cm-")>=0) rowEdit.row=RowIndex(nav.Focus(),"cm-");
  rows=MoveRows();
 }else if(screen=="combo-blocks"){
  rows=PatternRows(pattern);
 }else{
  // The history is longer than the detail pane, so Select opens it in a reader.
  rows={InfoRow("about",loc::T("training.guide"),"",loc::T("training.history.guide")),
        Row("p1",loc::T("training.player_one"),loc::T("training.history.detail")),
        Row("p2",loc::T("training.player_two"),loc::T("training.history.detail")),
        ConfirmRow("clear-history",loc::T("training.clear_history"),loc::T("training.clear_history.detail"),!pending)};
  rows[1].reading=rows[2].reading=true;
 }
 // F7 on a recorded slot lands on Record with its overwrite question open,
 // answered Cancel until the player chooses otherwise.
 if(offerOverwrite){
  offerOverwrite=false;
  if(screen=="recording"){nav.Focus("record",rows);nav.Choose(rows);}
 }
 const char* modes[]={"training.practice_ready","training.recording_suspended","training.playback_suspended"};
 std::string status=pending?loc::T("training.applying"):!error.empty()?error:!v.ready?loc::T("training.waiting_battle"):loc::T(modes[static_cast<int>(v.mode)]);
 Tone statusTone=pending?Tone::Pending:!error.empty()?Tone::Error:!v.ready?Tone::Pending:Tone::Neutral;
 if((screen=="combos"||screen=="recording"||screen=="combo-timing"||screen=="combo-moves"||screen=="combo-blocks")&&!creator.notice.empty()){status=creator.notice;statusTone=creator.failed?Tone::Error:Tone::Success;}
 const auto a=trainingMenu.Draw(loc::T("training.title"),rows,status.c_str(),[&](const std::string& id){
  if(screen=="history"&&(id=="p1"||id=="p2")){
   for(const auto& run:v.history[id=="p1"?0:1])ImGui::TextWrapped("%u f  %s",run.frames,Buttons(run.buttons).c_str());
  }
  if(id=="cb-tree"){
   if(creator.tree.empty())ImGui::TextUnformatted(loc::T("training.combo.tree.empty"));
   for(const auto& fighter:creator.tree){ImGui::TextUnformatted(FighterName(fighter.first));for(const auto& child:fighter.second.children)DrawTreeNode(child,1);}
  }
  // The selected combo and the typed line, drawn as the game's Trial screen shows commands.
  if(id=="cb-combo"){if(const auto* shown=CurrentCombo())ComboLine(shown->steps,ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_Text));}
  if(RowIndex(id,"ct-")>=0||RowIndex(id,"cm-")>=0){const auto steps=TimingSteps();const auto i=static_cast<std::size_t>((std::max)(RowIndex(id,"ct-"),RowIndex(id,"cm-")));
   if(i<steps.size())ComboLine({steps[i]},ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_Text));}
  // The whole combo under the editor's other rows, so an edit shows at once.
  else if(id.compare(0,3,"cm-")==0)ComboLine(TimingSteps(),ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_Text));
  if(id=="cb-steps"){std::vector<std::string> steps;std::string error;const auto* fighter=selection::FindFighter(creator.fighter);
   if(combo::ParseSteps(creator.steps,fighter?fighter->code:"",steps,error))ComboLine(steps,ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_Text));}
 },1,{},screen=="combo-blocks"?GameMenu::Body([&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction&,float height,const MenuVisualFeedback&){
  if(DrawPattern(pattern,entries,navigation,height,ImGui::GetFontSize()/ImGui::GetFont()->FontSize)) StorePattern();
 }):screen=="combo-moves"?GameMenu::Body([&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& made,float height,const MenuVisualFeedback&){
  DrawMoveEditor(entries,navigation,made,height,v,submit);
 }):GameMenu::Body{},ImGui::GetFontSize()/ImGui::GetFont()->FontSize,100,false,statusTone);
 if(a.kind==MenuAction::Close||a.id=="return"){ForwardMenuAction({MenuAction::Close});return;}
 // F8 on the combo and timing screens replays the moves as they are now;
 // on a timing row, only up to that move, so one rep can be tuned at a time.
 if((screen=="combos"||screen=="combo-timing"||screen=="combo-moves")&&!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_F8,false)){
  auto steps=TimingSteps();
  if(screen=="combo-timing"&&RowIndex(nav.Focus(),"ct-")>=0){
   const auto upTo=static_cast<std::size_t>(RowIndex(nav.Focus(),"ct-"));
   if(upTo+1<steps.size())steps.resize(upTo+1);
  }
  ReplayOrStop(steps,v,submit);return;
 }
 // On a move's own row the keys do what the rows above the list do.
 if((screen=="combo-timing"||screen=="combo-moves")&&!ImGui::GetIO().WantTextInput&&!nav.Editing()&&!nav.Confirming()&&
  RowIndex(nav.Focus(),screen=="combo-moves"?"cm-":"ct-")>=0){
  const std::string prefix=screen=="combo-moves"?"cm-":"ct-";
  if(ImGui::IsKeyPressed(ImGuiKey_Insert,false)){nav.Focus(prefix+"add",rows);nav.Choose(rows);return;}
  const char* key=ImGui::IsKeyPressed(ImGuiKey_Delete,false)?"cm-remove":ImGui::IsKeyPressed(ImGuiKey_D,false)?"cm-duplicate":
   screen=="combo-moves"&&ImGui::IsKeyPressed(ImGuiKey_G,false)?"cm-group":nullptr;
  if(key){MenuAction edit;edit.kind=MenuAction::Activate;edit.id=key;HandleMoves(edit,v,submit);return;}
 }
 if(a.kind==MenuAction::Activate&&screen=="home"&&a.id!="about"){if(a.id=="recording")ListRecordings();nav.Push(a.id);return;}
 if(screen=="recording"&&HandleRecordingLibrary(a,v,submit))return;
 if(screen=="combos"&&a.kind==MenuAction::Activate&&a.id=="cb-moves"){rowEdit=RowEdit{};nav.Push("combo-moves");return;}
 if(screen=="combo-moves"){HandleMoves(a,v,submit);return;}
 if(screen=="combos"&&a.kind==MenuAction::Activate&&a.id=="cb-timing"){nav.Push("combo-timing");return;}
 if(screen=="combos"&&a.kind==MenuAction::Activate&&a.id=="cb-blocks"){pattern=TimingSteps();nav.Push("combo-blocks");return;}
 if(screen=="combo-blocks"){HandlePattern(a,v,submit);return;}
 if(screen=="combos"){HandleCombo(a,v,submit);return;}
 if(screen=="combo-timing"){HandleTiming(a,v,submit);return;}
 if(a.kind!=MenuAction::Activate&&a.kind!=MenuAction::Adjust)return;
 Command command;command.generation=v.generation;command.requestId=nextRequest++;
 if(a.id.compare(0,5,"slot-")==0){command.action=Action::Select;command.value=std::stoi(a.id.substr(5));}
 else if(a.id=="record")command.action=Action::Record;
 else if(a.id=="play")command.action=Action::Play;
 else if(a.id=="stop")command.action=Action::Stop;
 else if(a.id=="clear")command.action=Action::Clear;
 else if(a.id=="loop"){command.action=Action::Loop;command.value=a.delta>0;}
 else if(a.id=="clear-history")command.action=Action::ClearHistory;
 else return;
 if(submit&&submit(command)){
  pending=command.requestId;
  returnAfter=command.action==Action::Record||command.action==Action::Play;
 }else error=loc::T("training.command_rejected");
}
// The running trial as a list beside the fight: every step with its state,
// then the tally and how the last attempt ended. Passive like the meter. Each
// state has its own mark, so the list reads without its colours.
void TrialList(const training::View& view, float hudScale) {
    const auto& trial = view.trial;
    const int count = static_cast<int>((std::min)(view.trialSteps.size(), trial.steps.size()));
    if (!count || view.nativeTrialList) return;
    const auto* vp = ImGui::GetMainViewport();
    // Below the game's health bars and portraits, at the edge Player 1 stands
    // on; on the right that is under the game's damage panel.
    const bool right = PlayerOneOnRight(view);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * (right ? .98f : .02f), vp->Pos.y + vp->Size.y * (right ? .40f : .24f)), ImGuiCond_Always, ImVec2(right ? 1.f : 0.f, 0));
    ImGui::SetNextWindowBgAlpha(.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * hudScale, 6 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training trial", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.9f * hudScale / Scale());
        const float width = (std::min)(320 * hudScale, vp->Size.x * .3f);
        const ImVec4 good = ImGui::ColorConvertU32ToFloat4(IM_COL32(118, 224, 160, 255));
        const ImVec4 bad = ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 121, 129, 255));
        // A long combo shows the steps around the one being waited for, as
        // many as fit above the frame meter.
        const int shown = (std::max)(3, (std::min)(12, static_cast<int>(vp->Size.y * .35f / ImGui::GetTextLineHeightWithSpacing()) - 2));
        const int first = (std::max)(0, (std::min)(trial.current - shown / 2, count - shown));
        for (int step = first; step < count && step < first + shown; ++step) {
            const auto state = trial.steps[step];
            const bool current = step == trial.current;
            const char* mark = state == TrialStepState::Done ? "[x]" : state == TrialStepState::Unchecked ? "[?]" : current ? "[>]" : "[ ]";
            const ImVec4 colour = state == TrialStepState::Done ? good : state == TrialStepState::Out ? ImGui::ColorConvertU32ToFloat4(palette::Ember) :
                current ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(colour, "%s", mark);
            ImGui::SameLine();
            // The step as the game's Trial screen shows a command, clipped to the list's width.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float room = width - (at.x - ImGui::GetWindowPos().x), h = ImGui::GetTextLineHeight();
            const ImU32 tint = ImGui::ColorConvertFloat4ToU32(colour);
            ImGui::PushClipRect(at, ImVec2(at.x + room, at.y + h), true);
            float used = DrawComboStep(view.trialSteps[step], at, tint, right);
            // Where the attempts broke, as a count after the move.
            if (step < static_cast<int>(trial.drops.size()) && trial.drops[step])
                used += h * .3f + glyphs::Word(ImGui::GetWindowDrawList(), ImVec2(at.x + used + h * .3f, at.y), h, ("x" + std::to_string(trial.drops[step])).c_str(), ImGui::ColorConvertFloat4ToU32(bad));
            if (state == TrialStepState::Unchecked)
                used += h * .3f + glyphs::Word(ImGui::GetWindowDrawList(), ImVec2(at.x + used + h * .3f, at.y), h, loc::Tf("training.combo.run.unchecked", "").c_str(), tint);
            ImGui::PopClipRect();
            ImGui::Dummy(ImVec2((std::min)(used, room), h));
        }
        ImGui::TextUnformatted(FitLabel(loc::Tf("training.combo.run.count", trial.successes, trial.attempts, trial.attempts ? 100 * trial.successes / trial.attempts : 0), width).c_str());
        // Last and never empty, so the steps above do not move between
        // attempts. The reason is a sentence and wraps instead of being cut.
        const char* reasons[] = {"", "training.combo.run.wrong_move", "training.combo.run.whiffed", "training.combo.run.dropped"};
        const int reason = static_cast<int>(trial.lastFailure);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        if (trial.complete) ImGui::TextColored(good, "%s", loc::T("training.combo.run.complete"));
        else if (reason > 0 && reason < 4) ImGui::TextColored(bad, "%s",
            loc::Tf("training.combo.run.failed", trial.failedStep + 1, loc::T(reasons[reason])).c_str());
        else ImGui::TextUnformatted(" ");
        ImGui::PopTextWrapPos();
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
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
// A Training table's shared reset and save, for the match's own HUD: the
// keys the player chose for them in Training, and the pad's Select, tapped
// to reset and held half a second to save. Returns the PracticeReset and
// PracticeSave bits asked for this frame, and draws the line that names the keys.
unsigned MatchPracticeKeys(bool padSelect) {
    if(ImGui::GetIO().WantTextInput||ImGui::GetIO().KeyAlt) return 0;
    LoadCombos();
    unsigned asked=0;
    const auto pressed=[](int which) { return creator.keys[which]>=0&&ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1+creator.keys[which]),false); };
    if(pressed(1)) asked|=training::PracticeReset;
    if(pressed(5)) asked|=training::PracticeSave;
    static double downAt=-1; static bool saved=false;
    const double now=ImGui::GetTime();
    if(padSelect&&downAt<0) { downAt=now; saved=false; }
    if(padSelect&&!saved&&now-downAt>=.5) { saved=true; asked|=training::PracticeSave; }
    if(!padSelect&&downAt>=0) { if(!saved&&now-downAt<.5) asked|=training::PracticeReset; downAt=-1; }
    const auto* vp=ImGui::GetMainViewport();
    const float hudScale=(std::max)(1.f,(std::min)(1.5f,vp->Size.y/900.f));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x/2,vp->Pos.y+vp->Size.y*TrainingHudBottom+4*hudScale),ImGuiCond_Always,ImVec2(.5f,0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(4*hudScale,3*hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);
    if(ImGui::Begin("Match practice keys",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f*hudScale/Scale());
        ImGui::TextDisabled("%s %s   %s %s",KeyName(1,"-").c_str(),loc::T("training.combo.reset_pos"),KeyName(5,"-").c_str(),loc::T("training.combo.save_pos"));
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return asked;
}
TrainingHudInput DrawTrainingHud(const training::View& view) {
    if (!view.available) return {};
    ChallengerBanner(view);
    const auto* vp = ImGui::GetMainViewport();
    // Size the passive HUD to the game viewport; menu/DPI scaling should not
    // turn it into a large panel over the fight.
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    TrialList(view, hudScale);
    // Wide, so a frame is a cell the eye can pick out.
    const float width = (std::min)(900 * hudScale, vp->Size.x * .75f);
    // The game's super meters and their SUPER! banners start about 17% above
    // the bottom edge and scale with the height, so the meter sits just above them.
    const float hudBottom = vp->Pos.y + vp->Size.y * TrainingHudBottom;
    const ImVec2 hudTop = MeterWindow(view.meter, hudScale, width, hudBottom);
    // The one input this HUD takes: a chip that opens the controls for a
    // mouse, as F6 does from the keyboard. It captures the mouse only while
    // the pointer is over it, so the passive meter above never does. With
    // the combo's line it hangs under the meter, between the game's two
    // super gauges, so the middle of the screen stays the fight's.
    TrainingHudInput input;
    ImGui::SetNextWindowPos(ImVec2(hudTop.x, hudBottom + 4 * hudScale), ImGuiCond_Always, ImVec2(0, 0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training shortcuts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        ComboStrip(view, width - 8 * hudScale);
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
        // The combo keys where the chips leave room for them inside the HUD's width.
        const auto keys = loc::Tf("training.combo.keys",KeyName(0,"-"),KeyName(1,"-"),KeyName(2,"-"),KeyName(3,"-"));
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(keys.c_str()).x <= width) ImGui::TextDisabled("%s", keys.c_str());
        const auto save = loc::Tf("training.combo.keys.save",KeyName(4,"-"))+"  "+KeyName(5,"-")+" "+loc::T("training.combo.save_pos");
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(save.c_str()).x <= width) ImGui::TextDisabled("%s", save.c_str());
        else ImGui::NewLine();
        ImGui::SetWindowFontScale(1.f);
        input.pointer = ImGui::IsWindowHovered();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return input;
}
} }
