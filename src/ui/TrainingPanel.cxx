#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <string>

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
    const float label = measure("P2") + gap + measure("+999 f") + gap + measure("Start 999 f") + gap;
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
            const int index = i - (120 - static_cast<int>(meter.frames.size()));
            if (index >= 0) {
                const auto& frame = meter.frames[index];
                const auto& sample = frame.fighters[side];
                const auto phase = sample.valid ? ClassifyStatus(sample.status) : Phase::Unknown;
                const ImU32 color =
                    (PhaseColor(phase) & ~IM_COL32_A_MASK) | (static_cast<ImU32>(phase == Phase::Neutral ? 130 : 215) << IM_COL32_A_SHIFT);
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

namespace { GameMenu trainingMenu; bool showRecordings=false; }
MenuNavigation& TrainingNavigation() { return trainingMenu.navigation; }
void ShowTrainingRecordings() {showRecordings=true;}
void DrawTrainingFlyout(const training::View& view,const TrainingSubmit& submit) {
    if(!view.available)return;
    SetMenuInput({0,ImGui::GetTime()});
    SetMenuGlyphs(1,0,0);
    const auto* vp=ImGui::GetMainViewport();
    const ImVec2 size((std::min)(820*Scale(),vp->Size.x*.8f),(std::min)(600*Scale(),vp->Size.y*.8f));
    // Compact typography independently of global DPI when the viewport cannot
    // accommodate the preferred panel. Other Ember windows keep their scale.
    const float unit=(std::min)(Scale(),(std::min)(size.x/500.f,size.y/500.f));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x*.5f,vp->Pos.y+vp->Size.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(size,ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.075f,.07f,.065f,.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(1,.53f,.22f,.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16*unit,12*unit));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10*unit,6*unit));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8*unit,6*unit));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,unit);
    const auto trainingWindow=std::string(loc::T("training.controls"))+"###TrainingControls";
    if(ImGui::Begin(trainingWindow.c_str(),nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs)) {
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
 if(showRecordings){showRecordings=false;nav.Home();nav.Push("recording");}
 if(pending&&v.commandId==pending){
  pending=0;
  if(v.commandAccepted){error.clear();if(returnAfter){RequestMenuReturn();return;}}
  else error=v.commandError;
 }
 const auto screen=nav.Screen();std::vector<MenuEntry> rows;
 const bool ready=v.ready&&!pending;
 if(screen=="home"){
  rows={Row("recording",loc::T("training.dummy_recording"),loc::T("training.dummy_recording.detail")),
   Row("history",loc::T("training.input_history"),loc::T("training.input_history.detail")),
   Row("return",loc::T("training.close_controls"),loc::T("training.close_controls.detail"))};
 }else if(screen=="recording"){
  for(int slot=0;slot<SlotCount;++slot)rows.push_back(Row("slot-"+std::to_string(slot),loc::Tf(slot==v.selected?"training.slot_selected":"training.slot",slot+1),
   loc::Tf("training.recorded_frames",v.lengths[slot]),ready&&v.mode==Mode::Idle));
  auto record=Row("record",loc::T("training.record"),loc::T(v.lengths[v.selected]?"training.record.overwrite":"training.record.detail"),ready);
  record.confirm=v.lengths[v.selected]>0;rows.push_back(record);
  rows.push_back(Row("play",loc::T("training.play"),loc::T(v.lengths[v.selected]?"training.play.detail":"training.slot_empty"),ready&&v.lengths[v.selected]>0));
  rows.push_back(Row("stop",loc::T("training.stop"),loc::T("training.stop.detail"),!pending));
  rows.push_back(Value("loop",loc::T("training.loop"),loc::T(v.loop?"common.on":"common.off"),loc::T("training.loop.detail"),!pending));
  rows.push_back(ConfirmRow("clear",loc::T("training.clear_recording"),loc::T("training.clear_recording.detail"),ready&&v.mode==Mode::Idle&&v.lengths[v.selected]>0));
 }else{
  rows={Row("p1",loc::T("training.player_one"),loc::T("training.history.detail")),
        Row("p2",loc::T("training.player_two"),loc::T("training.history.detail")),
        ConfirmRow("clear-history",loc::T("training.clear_history"),loc::T("training.clear_history.detail"),!pending)};
 }
 const char* modes[]={"training.practice_ready","training.recording_suspended","training.playback_suspended"};
 std::string status=pending?loc::T("training.applying"):!error.empty()?error:!v.ready?loc::T("training.waiting_battle"):loc::T(modes[static_cast<int>(v.mode)]);
 const Tone statusTone=pending?Tone::Pending:!error.empty()?Tone::Error:!v.ready?Tone::Pending:Tone::Neutral;
 const auto a=trainingMenu.Draw(loc::T("training.title"),rows,status.c_str(),[&](const std::string& id){
  if(screen=="history"&&(id=="p1"||id=="p2")){
   for(const auto& run:v.history[id=="p1"?0:1])ImGui::TextWrapped("%u f  %s",run.frames,Buttons(run.buttons).c_str());
  }
 },1,{},{},ImGui::GetFontSize()/ImGui::GetFont()->FontSize,100,false,statusTone);
 if(a.kind==MenuAction::Close||a.id=="return"){RequestMenuReturn();return;}
 if(a.kind==MenuAction::Activate&&screen=="home"){nav.Push(a.id);return;}
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
void DrawTrainingHud(const training::View& view) {
    if (!view.available) return;
    const auto* vp = ImGui::GetMainViewport();
    // Size the passive HUD to the game viewport; menu/DPI scaling should not
    // turn it into a large panel over the fight.
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    const float width = (std::min)(620 * hudScale, vp->Size.x * .75f);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y - 8 * hudScale),
        ImGuiCond_Always, ImVec2(.5f, 1));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * hudScale, 6 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training frame meter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        Meter(view.meter, hudScale);
        ImGui::TextDisabled("%s", loc::Tf("training.frame_advantage",
            loc::T(view.meter.advantage.pending ? "training.measuring" : view.meter.advantage.knockdown ? "training.wakeup" : view.meter.frozen ? "training.held" : "training.live")).c_str());
        // The HUD is a NoInputs window, so IsItemHovered is always false here;
        // test the pointer against the item rectangle instead. The tooltip is
        // its own window and does not make the HUD capture input.
        if (!view.meter.advantage.valid && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false))
            ImGui::SetTooltip("%s", Unavailable(loc::T("training.advantage"), view.meter.advantage.unavailable).c_str());
        // Training shortcuts are keyboard-only; this passive HUD never captures input.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(0,0));
        DrawTrainingOpenPrompt();
        ImGui::PopStyleVar();
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}
} }
