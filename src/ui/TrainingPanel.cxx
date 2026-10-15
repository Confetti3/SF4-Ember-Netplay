#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "MenuGlyphs.hxx"
#include "../common/Localization.hxx"
#include "../netplay/JsonFileStore.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/RecordingFile.hxx"
#include "../training/PracticeSettings.hxx"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
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
    // The last reply typed that is no preset, which Custom on the pick list
    // brings back; pickCustom: Custom is picked with nothing typed yet.
    std::string customMoves; bool pickCustom=false;
    // How the frame meter is drawn (the Frame data page's two options).
    training::MeterOptions meter;
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
    const nlohmann::json practice{{"reply",{{"when",lab.plan.when},{"chance",lab.plan.chance},{"timing",lab.plan.timing},
        {"vary_stance",lab.plan.varyStance},{"moves",lab.replyMoves},{"custom",lab.customMoves}}},{"keys",{{"reset_position",lab.keys[ResetKey]},{"save_position",lab.keys[SaveKey]}}},
        {"meter",MeterOptionsJson(lab.meter)}};
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
// Not until the directory is known, so a lookup before it is set (the match
// meter, a test) does not stop the file being read once it is.
void LoadPractice() {
    if(lab.loaded||lab.directory.empty()) return;
    lab.loaded=true;
    std::string bytes,error; bool missing=false;
    nlohmann::json practice;
    if(!netplay::json_file::ReadBytes(lab.directory/PracticeFile,bytes,missing,error)||missing||!netplay::json_file::Parse(bytes,practice,error)||!practice.is_object()) return;
    const auto reply=practice.value("reply",nlohmann::json::object());
    const auto number=[&](const char* name,int low,int high,int fallback) { const auto at=reply.find(name); return at!=reply.end()&&at->is_number_integer()&&*at>=low&&*at<=high?at->get<int>():fallback; };
    // An older file's "slot" is left unread: a recorded reply plays the slots that are on.
    if(reply.is_object()) {
        lab.plan.when=number("when",0,4,0); lab.plan.chance=number("chance",25,100,100);
        lab.plan.timing=number("timing",-MaxReplyTiming,MaxReplyTiming,0); lab.plan.varyStance=reply.value("vary_stance",nlohmann::json(false))==true;
        if(reply.contains("moves")&&reply["moves"].is_string()) lab.replyMoves=MigrateReplyMoves(reply["moves"].get<std::string>());
        if(reply.contains("custom")&&reply["custom"].is_string()) lab.customMoves=MigrateReplyMoves(reply["custom"].get<std::string>());
        else if(!lab.replyMoves.empty()&&ReplyPresetIndex(lab.replyMoves)<0) lab.customMoves=lab.replyMoves;
    }
    // An older file lists six keys in a row; these two were its second and its last.
    const auto keys=practice.value("keys",nlohmann::json::object());
    lab.keys=ReadPositionKeys(keys);
    lab.meter=ReadMeterOptions(practice.value("meter",nlohmann::json::object()));
}
// A queued save or reset can still be refused by the game, or fail there, so
// its notice waits for the result. The latest attempt owns the notice, so an
// older request's result arriving later is not shown over it.
void SendPosition(const training::View& view,const TrainingSubmit& submit,Action action) {
    lab.position=0;
    Command command; command.action=action; command.generation=view.generation; command.requestId=nextRequest++;
    // Whether a reset has a saved position to go to is the session's to say
    // when it reaches it (Session::Apply), after any save queued before it.
    if(!submit||!submit(command)) { Notice(loc::T("training.command_rejected"),true); return; }
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
// Empty until the game's own setting has been read. The action's last three
// are the pause menu's own and are named but never picked here.
const char* DummyLabel(int kind,int value) {
    if(kind==0&&value>=4&&value<=6) return loc::T(value==4?"training.dummy.human":value==5?"training.record":"training.play");
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
std::string KeyName(int which) { return lab.keys[which]<0?loc::T("common.off"):TrainingKeyName(lab.keys[which]); }
constexpr const char* ReplyNames[]={"common.off","training.reply.hit","training.reply.block","training.reply.rise","training.reply.any"};
// Where the reply's pick list stands: 0 the recordings that are on (no
// moves), then the presets, then Custom for anything else. A saved reply that
// spells a preset shows as that preset.
constexpr int ReplyCustom=ReplyPresetCount+1;
int ReplyPick() {
    if(lab.replyMoves.empty()) return lab.pickCustom?ReplyCustom:0;
    const int preset=ReplyPresetIndex(lab.replyMoves);
    return preset<0?ReplyCustom:preset+1;
}
std::string ReplyPickName(int pick) {
    return pick==0?loc::T("training.reply.recordings"):pick==ReplyCustom?loc::T("training.reply.custom"):ReplyPresets[pick-1];
}
// Select on the pick list opens Reply moves' field on the next frame, once its row exists.
bool openReplyEditor=false;
// Left and Right step the pick list. A preset goes through the same parser and
// checks as typed text; Custom brings back the last reply typed that is no
// preset, or none until one is typed.
void PickReply(const training::View& view,const TrainingSubmit& submit,int step) {
    const int count=ReplyCustom+1, pick=(ReplyPick()+count+step)%count;
    const std::string text=pick==0?std::string():pick==ReplyCustom?lab.customMoves:std::string(ReplyPresets[pick-1]);
    DummyPlan plan; std::string error;
    if(!BuildReplyPlan(text,lab.plan,plan,error)) { Notice(loc::Tf("training.invalid",error),true); return; }
    lab.pickCustom=pick==ReplyCustom; lab.replyMoves=text; SavePractice();
    SendPlan(view,submit);
}
std::vector<MenuEntry> ToolRows(const training::View& view,const std::string& screen) {
    LoadPractice();
    auto detailed=[](MenuEntry e,const char* detail){e.detail=detail;return e;};
    auto opening=[](MenuEntry e){e.opens=true;return e;};
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
        opening(Value("reply-move",loc::T("training.reply.move"),ReplyPickName(ReplyPick()),loc::T("training.reply.move.detail"))),
        detailed(TextRow("reply-moves",loc::T("training.reply.moves"),lab.replyMoves,256),loc::T("training.reply.moves.detail")),
        Value("reply-timing",loc::T("training.reply.timing"),(lab.plan.timing>0?"+":"")+std::to_string(lab.plan.timing)+" f",loc::T("training.reply.timing.detail"),lab.plan.when!=0),
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
            if(a.id=="reply-move") { PickReply(view,submit,step); return; }
            if(a.id=="reply") lab.plan.when=(lab.plan.when+5+step)%5;
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
        lab.replyMoves=text; lab.pickCustom=false;
        if(!text.empty()&&ReplyPresetIndex(text)<0) lab.customMoves=text;
        SavePractice();
        SendPlan(view,submit);
        return;
    }
    if(a.kind!=MenuAction::Activate) return;
    if(a.id=="reply-move") openReplyEditor=true;
    else if(a.id=="save-pos") SavePosition(view,submit);
    else if(a.id=="reset-pos") ResetPosition(view,submit);
}
}
void SetTrainingDirectory(std::wstring directory) { if(lab.directory.empty()) lab.directory=std::move(directory); }
void TrainingPadPosition(const training::View& view,const TrainingSubmit& submit,const input::TrainingPadEvent& pad) {
    if(!view.available||pad.generation!=view.generation) return;
    using Kind=input::TrainingPadEvent::Kind;
    LoadPractice();
    if(pad.kind==Kind::Save) {
        Command place; place.action=Action::Place; place.generation=pad.generation; place.place[0]=pad.place[0]; place.place[1]=pad.place[1];
        if(submit) submit(place);
        SavePosition(view,submit);
    }
    else if(pad.kind==Kind::Reset) ResetPosition(view,submit);
    // The chord on standby (TrainingPadInput): the recording starts, as F7's second press starts it.
    else if(pad.kind==Kind::Advance) {
        Command advance; advance.action=Action::AdvanceRecording; advance.generation=pad.generation;
        if(!submit||!submit(advance)) Notice(loc::T("training.command_rejected"),true);
    }
}
void TrainingHotkeys(const training::View& view,const TrainingSubmit& submit) {
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
}
training::MeterOptions TrainingMeterOptions() { LoadPractice(); return lab.meter; }
void SetTrainingMeterOptions(const training::MeterOptions& options) { lab.meter=options; }
void SetTrainingPositionKeys(int reset,int save) { LoadPractice(); lab.keys={{reset,save}}; }
std::string TrainingKeyName(int fromF1) { return "F"+std::to_string(fromF1+1); }
gamepads::PositionNames TrainingPadNames() {
    return gamepads::ButtonPositionNames(MenuPromptDevice()==input::PadDirectInput?input::PadDirectInput:input::PadXInput,MenuPromptBrand());
}
std::string TrainingPadChord() { const auto names=TrainingPadNames(); return std::string(names.view)+" + "+names.start; }
bool TrainingHotkeyBound(int fromF1) { return fromF1>=0&&(lab.keys[ResetKey]==fromF1||lab.keys[SaveKey]==fromF1); }
std::vector<TrainingKeyHint> TrainingPositionKeys() {
    LoadPractice();
    std::vector<TrainingKeyHint> keys;
    const char* const names[]={"training.hint.reset","training.hint.save"};
    for(const int which:{ResetKey,SaveKey}) if(lab.keys[which]>=0) keys.push_back({TrainingKeyName(lab.keys[which]),names[which]});
    return keys;
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
    if(!netplay::json_file::Publish(RecordingFolder(),std::filesystem::path(name+".json").wstring(),nlohmann::json::parse(training::ExportRecording(v.exported),nullptr,false),error,training::RecordingFileBytes))
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
    if(!netplay::json_file::ReadBytes(RecordingFolder()/(name+".json"),bytes,missing,error,training::RecordingFileBytes)||missing||!training::ImportRecording(bytes,frames,error)) {
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
bool OpenTrainingRecordings(input::TrainingControls& controls, const input::TrainingControls::Token& token) {
 if(!controls.Open(token)) return false;
 showRecordings=true;
 return true;
}
void DrawTrainingFlyout(const training::View& view,const TrainingSubmit& submit) {
    if(!view.available)return;
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
 const char* sizes[HudSizes]={"size.small","size.standard","size.large"};
 const bool recordingGrid=screen=="recording"&&ImGui::GetContentRegionAvail().x>=640*ImGui::GetFontSize()/ImGui::GetFont()->FontSize;
 if(screen=="home"){
  rows={Row("recording",loc::T("training.dummy_recording"),loc::T("training.dummy_recording.detail")),
   Row("history",loc::T("training.input_history"),loc::T("training.input_history.detail")),
   Row("frame-data",loc::T("training.frame_data"),loc::T("training.frame_data.detail")),
   Row("hud",loc::T("training.hud"),loc::T("training.hud.detail")),
   Row("dummy",loc::T("training.dummy"),loc::T("training.dummy.detail")),
   Row("reply",loc::T("training.reply"),loc::T("training.reply.detail")),
   Row("tools",loc::T("training.position"),std::string(loc::T("training.position.save.detail"))+"\n"+loc::T("training.position.reset.detail")),
   Row("return",loc::T("training.close_controls"),loc::T("training.close_controls.detail"))};
 }else if(screen=="recording"){
  // The slots are a list of what plays, as SF6's are: Select turns a
  // recorded slot on or off for Play and the dummy reply (Left and Right
  // too, where a list's rows allow it), and chooses an empty one for Record.
  for(int slot=0;slot<SlotCount;++slot){
   const bool recorded=v.lengths[slot]>0;
   rows.push_back(Value("slot-"+std::to_string(slot),loc::Tf(slot==v.selected?"training.slot_selected":"training.slot",slot+1),
    recorded?loc::T(v.on[slot]?"common.on":"common.off"):loc::T("training.slot.empty"),
    recorded?loc::Tf("training.slot.detail",v.lengths[slot]):loc::T("training.slot.empty_detail"),ready&&v.mode==Mode::Idle));
   // In the grid Left and Right move between the cells.
   rows.back().adjustable=recorded&&!recordingGrid;rows.back().opens=true;
  }
  // Left and Right choose the slot Record, Clear, Save and Mirror act on.
  auto record=Value("record",loc::T("training.record"),loc::Tf("training.slot",v.selected+1),
   std::string(loc::T("training.record.choose"))+"\n"+(v.lengths[v.selected]?std::string(loc::T("training.record.overwrite")):
    loc::Tf("training.record.standby_detail",TrainingKeyName(TrainingRecordKey),TrainingPadChord())),ready);
  record.opens=true;record.confirm=v.lengths[v.selected]>0;rows.push_back(record);
  // Select plays a slot that is on; Left and Right choose once or looped.
  rows.push_back(Value("play",loc::T("training.play"),loc::T(v.loop?"training.loop":"training.play.once"),
   loc::T(v.playable?"training.play.on_detail":"training.no_recordings_on"),ready&&v.playable));
  rows.back().opens=true;
  rows.push_back(Value("order",loc::T("training.order"),loc::T(v.order==Order::InOrder?"training.order.in_order":"training.order.random"),loc::T("training.order.detail")));
  rows.push_back(ConfirmRow("clear",loc::T("training.clear_recording"),loc::T("training.clear_recording.detail"),ready&&v.mode==Mode::Idle&&v.lengths[v.selected]>0));
  TakeExport(v);
  rows.push_back(TextRow("save-recording",loc::T("training.save_recording"),"",48,v.lengths[v.selected]>0&&!lab.directory.empty()));
  rows.back().detail=loc::T("training.save_recording.detail");
  rows.push_back(Value("load-recording",loc::T("training.load_recording"),recordings.empty()?loc::T("training.recording_none"):recordings[(std::min)(recordingChoice,recordings.size()-1)],loc::T("training.load_recording.detail"),!recordings.empty()&&ready&&v.mode==Mode::Idle));
  rows.back().opens=true;
  // A file that comes out the wrong way round is turned in one press.
  rows.push_back(Row("mirror",loc::T("training.mirror"),loc::T("training.mirror.detail"),ready&&v.mode==Mode::Idle&&v.lengths[v.selected]>0));
  for(std::size_t i=0;i<rows.size();++i){rows[i].wide=i>=SlotCount;rows[i].height=recordingGrid?30:0;}
 }else if(screen=="tools"||screen=="dummy"||screen=="reply"){
  rows=ToolRows(v,screen);
 }else if(screen=="frame-data"){
  LoadPractice();
  const auto onOff=[](bool on){return std::string(loc::T(on?"common.on":"common.off"));};
  rows={Row("p1",loc::T("training.player_one"),TrainingFrameData(v.meter,0)),
        Row("p2",loc::T("training.player_two"),TrainingFrameData(v.meter,1)),
        Row("color-key",loc::T("training.color_key"),""),
        Value("meter-style",loc::T("training.meter.style"),loc::T(lab.meter.style==MeterStyle::Classic?"training.meter.style.classic":lab.meter.style==MeterStyle::Flat?"training.meter.style.flat":"training.meter.style.angled"),loc::T("training.meter.style.detail")),
        Value("frames-shown",loc::T("training.meter.frames_shown"),std::to_string(lab.meter.shown),loc::T("training.meter.frames_shown.detail")),
        Value("hud-recovery",loc::T("training.meter.hud_recovery"),onOff(lab.meter.recovery),loc::T("training.meter.hud_recovery.detail"))};
  rows[0].reading=rows[1].reading=rows[2].reading=true;
 }else if(screen=="hud"){
  LoadPractice();
  rows.push_back(Value("hud-size",loc::T("training.meter.hud_size"),loc::T(sizes[lab.meter.size]),loc::T("training.meter.hud_size.detail")));
  for(const auto& part:HudParts)rows.push_back(Value(part.key,loc::T(part.label),loc::T(lab.meter.*part.shown?"common.on":"common.off"),loc::T("training.meter.hud_part.detail")));
  rows.push_back(Value("hud-inputs",loc::T("training.meter.hud_inputs"),loc::T(InputDisplayChoices[static_cast<int>(lab.meter.inputs)].label),loc::T("training.meter.hud_part.detail")));
  // The meter's own settings, Ember's beside the HUD's, on a page of their own.
  rows.push_back(Row("meter-settings",loc::T("training.meter.settings"),loc::T("training.meter.settings.detail")));
 }else if(screen=="meter-settings"){
  LoadPractice();
  rows={Value("meter-position",loc::T("training.meter.position"),std::to_string(lab.meter.position)+"%",loc::T("training.meter.position.detail")),
        Value("meter-history",loc::T("training.meter.history"),loc::Tf("training.meter.history_seconds",lab.meter.history/60),loc::T("training.meter.history.detail")),
        Value("meter-after-pause",loc::T("training.meter.after_pause"),loc::T(lab.meter.continuous?"training.meter.after_pause.continue":"training.meter.after_pause.start_over"),loc::T("training.meter.after_pause.detail")),
        Value("meter-scroll",loc::T("training.meter.scroll"),loc::T(lab.meter.smooth?"training.meter.scroll.smooth":"training.meter.scroll.pages"),loc::T("training.meter.scroll.detail"))};
  if(lab.meter.style==MeterStyle::Classic)rows.push_back(Row("meter-classic",loc::T("training.meter.classic"),loc::T("training.meter.classic.detail")));
 }else if(screen=="meter-classic"){
  LoadPractice();
  // A bar height for each size name, so the heights are named as the HUD's sizes are.
  static_assert(sizeof(ClassicBarHeights)/sizeof(*ClassicBarHeights)==HudSizes,"Each bar height needs a size name");
  const auto bar=std::find(std::begin(ClassicBarHeights),std::end(ClassicBarHeights),lab.meter.barHeight)-std::begin(ClassicBarHeights);
  rows.push_back(Value("bar-height",loc::T("training.meter.bar_height"),loc::T(sizes[bar]),loc::T("training.meter.bar_height.detail")));
  for(const auto& part:ClassicParts)rows.push_back(Value(part.key,loc::T(part.label),loc::T(lab.meter.*part.shown?"common.on":"common.off"),loc::T("training.meter.hud_part.detail")));
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
 // Select on the reply's pick list (Custom above all) opens the typed field.
 if(openReplyEditor){
  openReplyEditor=false;
  if(screen=="reply"){nav.Focus("reply-moves",rows);nav.Choose(rows);}
 }
 std::string modeStatus;
 switch(v.mode){
 case Mode::Idle: modeStatus=loc::T("training.practice_ready"); break;
 case Mode::Standby: modeStatus=loc::Tf("training.standby_status",TrainingKeyName(TrainingRecordKey),TrainingPadChord()); break;
 case Mode::Recording: modeStatus=loc::T("training.recording_suspended"); break;
 case Mode::Playback: modeStatus=loc::T("training.playback_suspended"); break;
 }
 std::string status=pending?loc::T("training.applying"):!error.empty()?error:!v.ready?loc::T("training.waiting_battle"):modeStatus;
 Tone statusTone=pending?Tone::Pending:!error.empty()?Tone::Error:!v.ready?Tone::Pending:Tone::Neutral;
 if((screen=="tools"||screen=="dummy"||screen=="reply"||screen=="recording")&&!lab.notice.empty()){status=lab.notice;statusTone=lab.failed?Tone::Error:Tone::Success;}
 // A recorded reply with no slot that can be one would do nothing; the page says so.
 else if(screen=="reply"&&lab.plan.when!=0&&lab.replyMoves.empty()&&!v.replyable){status=loc::T("training.reply.no_recordings_on");statusTone=Tone::Pending;}
 trainingMenu.wideListShare=screen=="recording"?.65f:screen=="dummy"||screen=="reply"||screen=="tools"||screen=="hud"||screen=="meter-settings"||screen=="meter-classic"?.58f:.5f;
 const auto a=trainingMenu.Draw(loc::T("training.title"),rows,status.c_str(),[&](const std::string& id){
  if(screen=="history"&&(id=="p1"||id=="p2")){
   for(const auto& run:v.history[id=="p1"?0:1])ImGui::TextWrapped("%u f  %s",run.frames,Buttons(run.buttons).c_str());
  }
  if(screen=="frame-data"&&id=="color-key")DrawTrainingColorKey(lab.meter.style!=MeterStyle::Flat);
 },recordingGrid?2:1,{},GameMenu::Body{},ImGui::GetFontSize()/ImGui::GetFont()->FontSize,recordingGrid?30:100,false,statusTone);
 if(a.kind==MenuAction::Close||a.id=="return"){ForwardMenuAction({MenuAction::Close});return;}
 if(a.kind==MenuAction::Activate&&screen=="home"){if(a.id=="recording")ListRecordings();nav.Push(a.id);return;}
 if(screen=="recording"&&HandleRecordingLibrary(a,v,submit))return;
 if(screen=="tools"||screen=="dummy"||screen=="reply"){HandleTools(a,v,submit);return;}
 // The HUD's size, Small to Large, and a part of the HUD: Off, then On.
 if(a.kind==MenuAction::Activate&&(a.id=="meter-settings"||a.id=="meter-classic")){nav.Push(a.id);return;}
 if(screen=="meter-settings"&&a.kind==MenuAction::Adjust&&a.id=="meter-history"){Step(lab.meter.history,{std::begin(MeterHistoryChoices),std::end(MeterHistoryChoices)},a.delta);SavePractice();return;}
 if(screen=="meter-settings"&&a.kind==MenuAction::Adjust&&a.id=="meter-scroll"){lab.meter.smooth=a.delta>0;SavePractice();return;}
 if(screen=="meter-settings"&&a.kind==MenuAction::Adjust&&a.id=="meter-after-pause"){lab.meter.continuous=a.delta>0;SavePractice();return;}
 if(screen=="meter-settings"&&a.kind==MenuAction::Adjust&&a.id=="meter-position"){Step(lab.meter.position,{std::begin(MeterPositions),std::end(MeterPositions)},a.delta);SavePractice();return;}
 if(screen=="meter-classic"&&a.kind==MenuAction::Adjust&&a.id=="bar-height"){Step(lab.meter.barHeight,{std::begin(ClassicBarHeights),std::end(ClassicBarHeights)},a.delta);SavePractice();return;}
 if(screen=="hud"&&a.kind==MenuAction::Adjust&&a.id=="hud-size"){lab.meter.size=(std::max)(0,(std::min)(HudSizes-1,lab.meter.size+a.delta));SavePractice();return;}
 if(screen=="hud"&&a.kind==MenuAction::Adjust&&a.id=="hud-inputs"){Step(lab.meter.inputs,{0,1,2,3},a.delta);SavePractice();return;}
 const auto turn=[&](const auto& parts){for(const auto& part:parts)if(a.id==part.key){lab.meter.*part.shown=a.delta>0;SavePractice();return true;}return false;};
 if(screen=="hud"&&a.kind==MenuAction::Adjust&&turn(HudParts))return;
 if(screen=="meter-classic"&&a.kind==MenuAction::Adjust&&turn(ClassicParts))return;
 if(screen=="frame-data"&&a.kind==MenuAction::Adjust&&(a.id=="meter-style"||a.id=="frames-shown"||a.id=="hud-recovery")){
  // Angled, Flat, then Classic; 60, 90, then 120 frames; Recovery Off, then On.
  if(a.id=="meter-style")lab.meter.style=static_cast<MeterStyle>((std::max)(0,(std::min)(MeterStyles-1,static_cast<int>(lab.meter.style)+a.delta)));
  else if(a.id=="frames-shown")Step(lab.meter.shown,{std::begin(MeterShownChoices),std::end(MeterShownChoices)},a.delta);
  else lab.meter.recovery=a.delta>0;
  SavePractice();return;
 }
 if(a.kind!=MenuAction::Activate&&a.kind!=MenuAction::Adjust)return;
 Command command;command.generation=v.generation;command.requestId=nextRequest++;
 if(a.id.compare(0,5,"slot-")==0){
  const int slot=std::stoi(a.id.substr(5));
  command.slot=slot;
  if(a.kind==MenuAction::Adjust){command.action=Action::SlotOn;command.on=a.delta>0;}
  else if(v.lengths[slot]>0){command.action=Action::SlotOn;command.on=!v.on[slot];}
  else command.action=Action::Select;
 }
 else if(a.id=="order"&&a.kind==MenuAction::Adjust){command.action=Action::PlayOrder;command.order=a.delta>0?Order::InOrder:Order::Random;}
 else if(a.id=="record"&&a.kind==MenuAction::Adjust){command.action=Action::Select;command.slot=(v.selected+SlotCount+(a.delta>0?1:-1))%SlotCount;}
 else if(a.id=="record")command.action=Action::AdvanceRecording;
 else if(a.id=="play"&&a.kind==MenuAction::Adjust){command.action=Action::Loop;command.loop=a.delta>0;}
 else if(a.id=="play")command.action=Action::Play;
 else if(a.id=="clear")command.action=Action::Clear;
 else if(a.id=="mirror")command.action=Action::Mirror;
 else if(a.id=="clear-history")command.action=Action::ClearHistory;
 else return;
 if(submit&&submit(command)){
  pending=command.requestId;
  returnAfter=command.action==Action::AdvanceRecording||command.action==Action::Play;
 }else error=loc::T("training.command_rejected");
}
} }
