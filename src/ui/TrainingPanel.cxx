#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include "../netplay/JsonFileStore.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/RecordingFile.hxx"
#include "../training/PracticeSettings.hxx"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// The F6 training controls: dummy recording, input history, and the position
// and dummy tools with their two hotkeys. The frame meter and the HUD are in
// TrainingMeter.cxx.
namespace sf4e { namespace ui {
namespace {
using namespace training;
std::string Buttons(unsigned bits) {
    std::string text;
    const unsigned masks[] = {1, 2, 4, 8, 0x10, 0x20, 0x400, 0x40, 0x80, 0x800};
    const char* labels[] = {"U", "D", "L", "R", "LP", "MP", "HP", "LK", "MK", "HK"};
    for (int i = 0; i < 10; ++i) if (bits & masks[i]) {
        if (!text.empty()) text += " + "; text += labels[i];
    }
    return text.empty() ? loc::T("training.neutral") : text;
}
// What the lab keeps between battles. It owns no game state, so it works on
// the overlay thread alone.
struct Lab {
    std::filesystem::path directory;
    bool loaded=false, failed=false;
    // What the last command did, and when, so the HUD can let it fade.
    std::string notice; double noticeAt=0;
    // What the dummy does by itself, and the keys that reset and save the
    // position as offsets from F1 (-1 unbound): the player's, kept in training.json.
    training::DummyPlan plan; std::string replyMoves;
    std::array<int,2> keys{{1,10}};
    // The save or reset sent last, told only once the game says it was done.
    std::uint64_t position=0,positionGeneration=0; bool positionSave=false;
} lab;
// One sequence for every tracked command, so the panel's and the position
// keys' acknowledgements are never confused.
std::uint64_t nextRequest=1;
enum { ResetKey, SaveKey };
constexpr const wchar_t* PracticeFile=L"training.json";
void Notice(std::string text,bool failed=false) { lab.notice=std::move(text); lab.failed=failed; lab.noticeAt=ImGui::GetTime(); }
void SavePractice() {
    if(lab.directory.empty()) return;
    std::string error;
    const nlohmann::json practice{{"reply",{{"when",lab.plan.when},{"slot",lab.plan.slot},{"chance",lab.plan.chance},{"timing",lab.plan.timing},
        {"vary_stance",lab.plan.varyStance},{"moves",lab.replyMoves}}},{"keys",{{"reset_position",lab.keys[ResetKey]},{"save_position",lab.keys[SaveKey]}}}};
    if(!netplay::json_file::Publish(lab.directory,PracticeFile,practice,error)) Notice(loc::T("training.save_failed"),true);
}
// The plan into the game, its typed reply made into input for either facing.
bool SendPlan(const training::View& view,const TrainingSubmit& submit) {
    Command command; command.action=Action::DummyPlan; command.generation=view.generation; command.plan=lab.plan;
    std::string error;
    // Off needs no moves, even when the saved text cannot be read.
    if(!BuildReplyPlan(lab.plan.when==0?std::string():lab.replyMoves,lab.plan,command.plan,error)) { Notice(loc::Tf("training.invalid",error),true); return false; }
    const bool sent=submit&&submit(command);
    if(!sent) Notice(loc::T("training.command_rejected"),true);
    return sent;
}
// The dummy's reply and the hotkeys; anything unreadable keeps its default.
void LoadPractice() {
    if(lab.loaded) return;
    lab.loaded=true;
    if(lab.directory.empty()) return;
    std::string bytes,error; bool missing=false;
    nlohmann::json practice;
    if(!netplay::json_file::ReadBytes(lab.directory/PracticeFile,bytes,missing,error)||missing||!netplay::json_file::Parse(bytes,practice,error)||!practice.is_object()) return;
    const auto reply=practice.value("reply",nlohmann::json::object());
    const auto number=[&](const char* name,int low,int high,int fallback) { const auto at=reply.find(name); return at!=reply.end()&&at->is_number_integer()&&*at>=low&&*at<=high?at->get<int>():fallback; };
    if(reply.is_object()) {
        lab.plan.when=number("when",0,4,0); lab.plan.slot=number("slot",0,SlotCount-1,0); lab.plan.chance=number("chance",25,100,100);
        lab.plan.timing=number("timing",-MaxReplyTiming,MaxReplyTiming,0); lab.plan.varyStance=reply.value("vary_stance",nlohmann::json(false))==true;
        if(reply.contains("moves")&&reply["moves"].is_string()) lab.replyMoves=MigrateReplyMoves(reply["moves"].get<std::string>());
    }
    // An older file lists six keys in a row; these two were its second and its last.
    const auto keys=practice.value("keys",nlohmann::json::object());
    lab.keys=ReadPositionKeys(keys);
}
// A queued save or reset can still be refused by the game, or fail there, so
// its notice waits for the result. The latest attempt owns the notice, so an
// older request's result arriving later is not shown over it.
void SendPosition(const training::View& view,const TrainingSubmit& submit,Action action) {
    lab.position=0;
    Command command; command.action=action; command.generation=view.generation; command.requestId=nextRequest++;
    if((action==Action::Restore&&!view.checkpoint)||!submit||!submit(command)) { Notice(loc::T("training.command_rejected"),true); return; }
    lab.position=command.requestId; lab.positionGeneration=view.generation; lab.positionSave=action==Action::Save;
    lab.notice.clear();
}
void SavePosition(const training::View& view,const TrainingSubmit& submit) { SendPosition(view,submit,Action::Save); }
void ResetPosition(const training::View& view,const TrainingSubmit& submit) { SendPosition(view,submit,Action::Restore); }
void TakePositionResult(const training::View& view) {
    bool accepted=false;
    if(lab.position&&view.generation!=lab.positionGeneration) lab.position=0;
    if(!lab.position||!view.acks.Find(lab.position,accepted)) return;
    lab.position=0;
    Notice(loc::T(!accepted?"training.command_rejected":lab.positionSave?"training.position.saved":"training.position.reset_done"),!accepted);
}
// The dummy's settings as the game's Training menu numbers them, and their
// names: 0 action, 1 guard, 2 counter hit, 3 quick stand, 4 either gauge.
const std::vector<int>& DummyChoices(int kind) {
    static const std::vector<int> choices[5]={{0,1,2,3},{0,1,2,3},{0,1,2},{0,1,2,3},{0,5,7,8}};
    return choices[kind];
}
// Empty until the game's own setting has been read.
const char* DummyLabel(int kind,int value) {
    static const char* const names[5][4]={
        {"training.dummy.stand","training.dummy.crouch","training.dummy.jump","training.dummy.cpu"},
        {"training.dummy.guard_none","training.dummy.guard_first","training.dummy.guard_all","training.dummy.guard_random"},
        {"training.dummy.counter_off","training.dummy.counter_on","training.dummy.counter_random",""},
        {"training.dummy.quick_quick","training.dummy.quick_normal","training.dummy.quick_delayed","training.dummy.quick_random"},
        {"training.dummy.gauge_normal","training.dummy.gauge_max","training.dummy.gauge_infinite","training.dummy.gauge_refill"}};
    const auto& choices=DummyChoices(kind);
    for(std::size_t i=0;i<choices.size();++i) if(choices[i]==value) return loc::T(names[kind][i]);
    return "";
}
// Left and Right step a setting through its choices.
int StepDummy(int kind,int value,int delta) {
    const auto& choices=DummyChoices(kind);
    const auto found=std::find(choices.begin(),choices.end(),value);
    if(found==choices.end()) return choices[0];
    return choices[(found-choices.begin()+choices.size()+(delta>0?1:choices.size()-1))%choices.size()];
}
// A hotkey's key as the hint and its row show it.
std::string KeyName(int which) { return lab.keys[which]<0?loc::T("common.off"):"F"+std::to_string(lab.keys[which]+1); }
constexpr const char* ReplyNames[]={"common.off","training.reply.hit","training.reply.block","training.reply.rise","training.reply.any"};
std::vector<MenuEntry> ToolRows(const training::View& view,const std::string& screen) {
    LoadPractice();
    auto detailed=[](MenuEntry e,const char* detail){e.detail=detail;return e;};
    const auto& dummy=view.dummy;
    if(screen=="dummy") return {
        Value("dummy-action",loc::T("training.dummy.action"),DummyLabel(0,dummy.action),loc::T("training.dummy.detail")),
        Value("dummy-guard",loc::T("training.dummy.guard"),DummyLabel(1,dummy.guard),loc::T("training.dummy.detail")),
        Value("dummy-counter",loc::T("training.dummy.counter"),DummyLabel(2,dummy.counterHit),loc::T("training.dummy.detail")),
        Value("dummy-quick",loc::T("training.dummy.quick"),DummyLabel(3,dummy.quickStand),loc::T("training.dummy.detail")),
        Value("dummy-super",loc::T("training.dummy.super"),DummyLabel(4,dummy.super),loc::T("training.dummy.detail")),
        Value("dummy-revenge",loc::T("training.dummy.revenge"),DummyLabel(4,dummy.revenge),loc::T("training.dummy.detail"))};
    if(screen=="reply") return {
        Value("reply",loc::T("training.reply"),loc::T(ReplyNames[lab.plan.when]),loc::T("training.reply.detail")),
        detailed(TextRow("reply-moves",loc::T("training.reply.moves"),lab.replyMoves,256),loc::T("training.reply.moves.detail")),
        Value("reply-timing",loc::T("training.reply.timing"),(lab.plan.timing>0?"+":"")+std::to_string(lab.plan.timing)+" f",loc::T("training.reply.timing.detail"),lab.plan.when!=0),
        Value("reply-slot",loc::T("training.reply.slot"),loc::Tf("training.slot",lab.plan.slot+1),loc::Tf("training.recorded_frames",view.lengths[lab.plan.slot])+"\n"+loc::T("training.reply.slot.detail"),lab.plan.when!=0&&lab.replyMoves.empty()),
        Value("reply-chance",loc::T("training.reply.chance"),std::to_string(lab.plan.chance)+"%",loc::T("training.reply.chance.detail"),lab.plan.when!=0),
        Value("reply-stance",loc::T("training.reply.vary_stance"),loc::T(lab.plan.varyStance?"common.on":"common.off"),loc::T("training.reply.vary_stance.detail"))};
    return {
        Row("save-pos",loc::T("training.position.save"),loc::T("training.position.save.detail"),view.ready),
        Row("reset-pos",loc::T("training.position.reset"),loc::T("training.position.reset.detail"),view.ready&&view.checkpoint),
        Value("key-0",loc::T("training.key.reset"),KeyName(ResetKey),loc::T("training.key.detail")),
        // Named by the row it presses, so it needs no words of its own.
        Value("key-1",loc::T("training.position.save"),KeyName(SaveKey),loc::T("training.key.detail"))};
}
void HandleTools(const MenuAction& a,const training::View& view,const TrainingSubmit& submit) {
    if(a.kind==MenuAction::Adjust) {
        const int step=a.delta>0?1:-1;
        if(a.id.compare(0,5,"reply")==0) {
            if(a.id=="reply") lab.plan.when=(lab.plan.when+5+step)%5;
            else if(a.id=="reply-slot") lab.plan.slot=(lab.plan.slot+SlotCount+step)%SlotCount;
            else if(a.id=="reply-chance") lab.plan.chance=(std::max)(25,(std::min)(100,lab.plan.chance+25*step));
            else if(a.id=="reply-timing") lab.plan.timing=(std::max)(-MaxReplyTiming,(std::min)(MaxReplyTiming,lab.plan.timing+step));
            else if(a.id=="reply-stance") lab.plan.varyStance=a.delta>0;
            else return;
            SavePractice();
            SendPlan(view,submit);
        }
        else if(a.id=="key-0"||a.id=="key-1") {
            // One key does one thing, so the other row's key is passed over.
            static const int choices[]={-1,0,1,2,3,8,10};
            const int which=a.id[4]-'0', count=static_cast<int>(sizeof(choices)/sizeof(*choices));
            int at=0;
            for(int i=0;i<count;++i) if(choices[i]==lab.keys[which]) at=i;
            do at=(at+count+step)%count; while(choices[at]>=0&&choices[at]==lab.keys[1-which]);
            lab.keys[which]=choices[at]; SavePractice();
        }
        else if(a.id.compare(0,6,"dummy-")==0) {
            const std::string which=a.id.substr(6);
            const auto& now=view.dummy;
            Command command; command.action=Action::DummyState; command.generation=view.generation;
            if(which=="action") command.dummy.action=StepDummy(0,now.action,a.delta);
            else if(which=="guard") command.dummy.guard=StepDummy(1,now.guard,a.delta);
            else if(which=="counter") command.dummy.counterHit=StepDummy(2,now.counterHit,a.delta);
            else if(which=="quick") command.dummy.quickStand=StepDummy(3,now.quickStand,a.delta);
            else if(which=="super") command.dummy.super=StepDummy(4,now.super,a.delta);
            else if(which=="revenge") command.dummy.revenge=StepDummy(4,now.revenge,a.delta);
            else return;
            if(!submit||!submit(command)) Notice(loc::T("training.command_rejected"),true);
        }
        return;
    }
    if(a.kind==MenuAction::TextAccepted&&a.id=="reply-moves") {
        DummyPlan plan; std::string error; const auto text=combo::Clean(a.text);
        if(!BuildReplyPlan(text,lab.plan,plan,error)) { Notice(loc::Tf("training.invalid",error),true); return; }
        lab.replyMoves=text; SavePractice();
        SendPlan(view,submit);
        return;
    }
    if(a.kind!=MenuAction::Activate) return;
    if(a.id=="save-pos") SavePosition(view,submit);
    else if(a.id=="reset-pos") ResetPosition(view,submit);
}
}
void SetTrainingDirectory(std::wstring directory) { if(lab.directory.empty()) lab.directory=std::move(directory); }
void TrainingHotkeys(const training::View& view,const TrainingSubmit& submit,bool padSelect) {
    // Called every frame training is available, so a key's save is told even with the controls closed.
    TakePositionResult(view);
    if(!view.available||ImGui::GetIO().WantTextInput||ImGui::GetIO().KeyAlt) return;
    LoadPractice();
    // The plan is the player's, so every battle gets it again.
    static std::uint64_t sent=0;
    if(sent!=view.generation&&SendPlan(view,submit)) sent=view.generation;
    const auto pressed=[](int which) { return lab.keys[which]>=0&&ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1+lab.keys[which]),false); };
    if(pressed(ResetKey)) ResetPosition(view,submit);
    if(pressed(SaveKey)) SavePosition(view,submit);
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
        SavePosition(view,submit);
    }
    if(!padSelect&&downAt>=0) { if(!saved&&now-downAt<.5) ResetPosition(view,submit); downAt=-1; }
}
bool TrainingHotkeyBound(int fromF1) { return fromF1>=0&&(lab.keys[ResetKey]==fromF1||lab.keys[SaveKey]==fromF1); }
std::string TrainingKeyHints() {
    std::string hints;
    const char* const names[]={"training.position.reset","training.position.save"};
    for(int which=0;which<2;++which) if(lab.keys[which]>=0) hints+=(hints.empty()?"":"  ")+KeyName(which)+" "+loc::T(names[which]);
    return hints;
}
std::string TrainingNotice(bool& failed) {
    failed=lab.failed;
    return ImGui::GetTime()-lab.noticeAt<3?lab.notice:std::string();
}
// The recording library: slots saved by name beside training.json.
std::vector<std::string> recordings; std::size_t recordingChoice=0;
std::string savePending; std::uint64_t exportSeen=0;
std::filesystem::path RecordingFolder() { return lab.directory/"recordings"; }
void ListRecordings() {
    recordings.clear(); std::error_code ignored;
    if(lab.directory.empty()) return;
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
        Notice(loc::T("training.save_failed"),true);
    else ListRecordings();
}
// Save as, choose, load: the slot goes to a file by name, a file into the selected slot.
bool HandleRecordingLibrary(const MenuAction& a,const training::View& v,const TrainingSubmit& submit) {
    if(a.kind==MenuAction::TextAccepted&&a.id=="save-recording") {
        const auto name=combo::Clean(a.text);
        if(name.empty()||name.find_first_of("\\/:*?\"<>|")!=std::string::npos) { Notice(loc::Tf("training.invalid",name),true); return true; }
        Command command; command.action=Action::ExportSlot; command.generation=v.generation;
        if(submit&&submit(command)) { savePending=name; Notice(loc::Tf("training.recording_saved",name)); }
        else Notice(loc::T("training.command_rejected"),true);
        return true;
    }
    if(a.id!="load-recording") return false;
    if(a.kind==MenuAction::Adjust&&!recordings.empty()) { recordingChoice=(recordingChoice+recordings.size()+(a.delta>0?1:recordings.size()-1))%recordings.size(); return true; }
    if(a.kind!=MenuAction::Activate||recordings.empty()) return true;
    const auto name=recordings[(std::min)(recordingChoice,recordings.size()-1)];
    std::string bytes,error; bool missing=false; std::vector<training::Input> frames;
    if(!netplay::json_file::ReadBytes(RecordingFolder()/(name+".json"),bytes,missing,error)||missing||!training::ImportRecording(bytes,frames,error)) {
        Notice(loc::Tf("training.invalid",error),true); return true;
    }
    Command load; load.action=Action::Load; load.generation=v.generation; load.side=1; load.frames=frames;
    if(submit&&submit(load)) Notice(loc::Tf("training.recording_loaded",name,v.selected+1));
    else Notice(loc::T("training.command_rejected"),true);
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
 static std::uint64_t generation=0,pending=0;
 static bool returnAfter=false;
 static std::string error;
 static int lastFrame=-2;
 if(lastFrame!=ImGui::GetFrameCount()-1){pending=0;returnAfter=false;nav.Cancel();}
 lastFrame=ImGui::GetFrameCount();
 if(generation!=v.generation){generation=v.generation;nav.Home();nav.Cancel();pending=0;error.clear();}
 if(showRecordings){showRecordings=false;nav.Home();nav.Push("recording");offerOverwrite=true;ListRecordings();}
 TakePositionResult(v);
 bool accepted=false;
 if(pending&&v.acks.Find(pending,accepted)){
  pending=0;
  if(accepted){error.clear();if(returnAfter){ForwardMenuAction({MenuAction::Close});return;}}
  else error=loc::T("training.command_rejected");
 }
 // Its root is the training lab; Back from there closes the controls and
 // returns to the game.
 trainingMenu.rootName=loc::T("training.lab");trainingMenu.exitName=loc::T("screen.game");
 trainingMenu.backHint=nav.Screen()==nav.Root()?loc::T("training.close_controls"):"";
 const auto screen=nav.Screen();std::vector<MenuEntry> rows;
 const bool ready=v.ready&&!pending;
 const bool recordingGrid=screen=="recording"&&ImGui::GetContentRegionAvail().x>=640*ImGui::GetFontSize()/ImGui::GetFont()->FontSize;
 if(screen=="home"){
  rows={Row("recording",loc::T("training.dummy_recording"),loc::T("training.dummy_recording.detail")),
   Row("history",loc::T("training.input_history"),loc::T("training.input_history.detail")),
   Row("frame-data",loc::T("training.frame_data"),loc::T("training.frame_data.detail")),
   Row("dummy",loc::T("training.dummy"),loc::T("training.dummy.detail")),
   Row("reply",loc::T("training.reply"),loc::T("training.reply.detail")),
   Row("tools",loc::T("training.position"),std::string(loc::T("training.position.save.detail"))+"\n"+loc::T("training.position.reset.detail")),
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
  TakeExport(v);
  rows.push_back(TextRow("save-recording",loc::T("training.save_recording"),"",48,v.lengths[v.selected]>0&&!lab.directory.empty()));
  rows.back().detail=loc::T("training.save_recording.detail");
  rows.push_back(Value("load-recording",loc::T("training.load_recording"),recordings.empty()?loc::T("training.recording_none"):recordings[(std::min)(recordingChoice,recordings.size()-1)],loc::T("training.load_recording.detail"),!recordings.empty()&&ready&&v.mode==Mode::Idle));
  rows.back().opens=true;
  for(std::size_t i=0;i<rows.size();++i){rows[i].wide=i>=SlotCount;rows[i].height=recordingGrid?30:0;}
 }else if(screen=="tools"||screen=="dummy"||screen=="reply"){
  rows=ToolRows(v,screen);
 }else if(screen=="frame-data"){
  rows={Row("p1",loc::T("training.player_one"),TrainingFrameData(v.meter,0)),
        Row("p2",loc::T("training.player_two"),TrainingFrameData(v.meter,1)),
        Row("color-key",loc::T("training.color_key"),"")};
  rows[0].reading=rows[1].reading=rows[2].reading=true;
 }else{
  // The history is longer than the detail pane, so Select opens it in a reader.
  rows={Row("p1",loc::T("training.player_one"),loc::T("training.history.detail")),
        Row("p2",loc::T("training.player_two"),loc::T("training.history.detail")),
        ConfirmRow("clear-history",loc::T("training.clear_history"),loc::T("training.clear_history.detail"),!pending)};
  rows[0].reading=rows[1].reading=true;
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
 if((screen=="tools"||screen=="dummy"||screen=="reply"||screen=="recording")&&!lab.notice.empty()){status=lab.notice;statusTone=lab.failed?Tone::Error:Tone::Success;}
 trainingMenu.wideListShare=screen=="recording"?.65f:screen=="dummy"||screen=="reply"||screen=="tools"?.58f:.5f;
 const auto a=trainingMenu.Draw(loc::T("training.title"),rows,status.c_str(),[&](const std::string& id){
  if(screen=="history"&&(id=="p1"||id=="p2")){
   for(const auto& run:v.history[id=="p1"?0:1])ImGui::TextWrapped("%u f  %s",run.frames,Buttons(run.buttons).c_str());
  }
  if(screen=="frame-data"&&id=="color-key")DrawTrainingColorKey();
 },recordingGrid?2:1,{},GameMenu::Body{},ImGui::GetFontSize()/ImGui::GetFont()->FontSize,recordingGrid?30:100,false,statusTone);
 if(a.kind==MenuAction::Close||a.id=="return"){ForwardMenuAction({MenuAction::Close});return;}
 if(a.kind==MenuAction::Activate&&screen=="home"){if(a.id=="recording")ListRecordings();nav.Push(a.id);return;}
 if(screen=="recording"&&HandleRecordingLibrary(a,v,submit))return;
 if(screen=="tools"||screen=="dummy"||screen=="reply"){HandleTools(a,v,submit);return;}
 if(a.kind!=MenuAction::Activate&&a.kind!=MenuAction::Adjust)return;
 Command command;command.generation=v.generation;command.requestId=nextRequest++;
 if(a.id.compare(0,5,"slot-")==0){command.action=Action::Select;command.slot=std::stoi(a.id.substr(5));}
 else if(a.id=="record")command.action=Action::Record;
 else if(a.id=="play")command.action=Action::Play;
 else if(a.id=="stop")command.action=Action::Stop;
 else if(a.id=="clear")command.action=Action::Clear;
 else if(a.id=="loop"){command.action=Action::Loop;command.loop=a.delta>0;}
 else if(a.id=="clear-history")command.action=Action::ClearHistory;
 else return;
 if(submit&&submit(command)){
  pending=command.requestId;
  returnAfter=command.action==Action::Record||command.action==Action::Play;
 }else error=loc::T("training.command_rejected");
}
} }
