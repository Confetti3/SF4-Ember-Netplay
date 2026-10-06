#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include "../common/FighterCatalog.hxx"
#include "../netplay/JsonFileStore.hxx"
#include "../training/ComboTrial.hxx"
#include "ComboGlyphs.hxx"
#include "ComboBlocks.hxx"
#include "../training/RecordingFile.hxx"
#include "../training/ComboReplay.hxx"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
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
    // The dummy and gauge settings being edited: the selected combo's, or the
    // next combo's when none is selected.
    combo::Setup setup;
} creator;
constexpr const wchar_t* ComboBookFile=L"combos.json";
// The pack a combo goes to when none exists yet. Saved, so it stays English.
constexpr const char* DefaultPack="My combos";
const char* FighterName(const std::string& code) {
    for(int id=0;id<selection::FighterCount;++id) if(const auto* fighter=selection::FindFighter(id)) if(code==fighter->code) return fighter->name;
    return code.c_str();
}
combo::Pack* CurrentPack() { return creator.packs.empty()?nullptr:&creator.packs[(std::min)(creator.pack,static_cast<int>(creator.packs.size())-1)]; }
combo::Combo* CurrentCombo() {
    auto* pack=CurrentPack();
    return !pack||pack->combos.empty()?nullptr:&pack->combos[(std::min)(creator.entry,static_cast<int>(pack->combos.size())-1)];
}
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
void LoadCombos() {
    if(creator.loaded) return;
    creator.loaded=true;
    if(creator.directory.empty()) return;
    std::string bytes,error; bool missing=false;
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
// The game's own trials for the selected fighter, as a pack named after it.
void ImportTrials() {
    const auto* fighter=selection::FindFighter(creator.fighter);
    combo::Fighter files; clg::File trials; std::filesystem::path from;
    if(!fighter||!LoadGameTrials(*fighter,files,trials,from)) return;
    std::vector<std::string> issues;
    const auto pack=combo::ReadTrials(trials,files,fighter->code,fighter->name,issues);
    const int read=static_cast<int>(pack.combos.size()), added=read?combo::Merge(creator.packs,{pack}):0;
    for(std::size_t i=0;i<creator.packs.size();++i) if(creator.packs[i].name==pack.name) { creator.pack=static_cast<int>(i); creator.entry=0; }
    if(added) SaveCombos();
    ComboNotice(loc::Tf("training.combo.trials_imported",read,static_cast<int>(trials.levels.size()),added),!read);
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
bool ResetPosition(const training::View& view,const TrainingSubmit& submit) {
    Command restore;
    if(!creator.resetBeforeReplay||!submit||!PlaceCommand(view,restore)) return true;
    return submit(restore);
}
void StartTrial(const training::View& view,const TrainingSubmit& submit) {
    const auto* shown=CurrentCombo();
    if(!shown) return;
    combo::Fighter files; std::string error;
    if(!combo::LoadFighter(GameFolder(),shown->character,files,error)) { ComboNotice(loc::Tf("training.combo.game_file",error),true); return; }
    Command command; command.action=Action::StartTrial; command.generation=view.generation;
    command.trial=combo::PracticeTrial(files,*shown); command.trialSteps=shown->steps;
    command.trialResetOnDrop=creator.resetBeforeReplay;
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
    if(!ApplySetup(view,submit)||!ResetPosition(view,submit)) { ComboNotice(loc::T("training.command_rejected"),true); return; }
    Command load; load.action=Action::Load; load.generation=view.generation; load.value=creator.replayBy;
    load.frames=combo::Synthesize(steps,creator.replayFacingRight,creator.replayOffset);
    Command play; play.action=Action::Play; play.generation=view.generation;
    if(!submit||!submit(load)||!submit(play)) { ComboNotice(loc::T("training.command_rejected"),true); return; }
    ComboNotice(loc::Tf("training.combo.replay.started",view.selected+1));
}
// The moves the timing screen and a replay work on: the typed line when
// there is one, else the selected combo.
std::vector<std::string> TimingSteps() {
    std::vector<std::string> steps; std::string error;
    const auto* fighter=selection::FindFighter(creator.fighter);
    if(!creator.steps.empty()) { combo::ParseSteps(creator.steps,fighter?fighter->code:"",steps,error); return steps; }
    return CurrentCombo()?CurrentCombo()->steps:steps;
}
// The timing screen: those moves one by one, each with its "@" offset to
// nudge and what the last replay saw for it.
struct Tune { bool on=false, started=false; std::size_t step=0; int best=training::MinOffset-1, candidate=0, settle=0, start=0; } tune;
// How far below a move's starting offset the tuner looks before giving up on it.
constexpr int TuneReach=6;
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
std::vector<MenuEntry> TimingRows(const training::View& view) {
    const auto steps=TimingSteps();
    std::vector<MenuEntry> rows{Row("ct-replay",loc::T("training.combo.replay"),loc::T("training.combo.timing.replay.detail"),!steps.empty()),
        Row("ct-tune",loc::T(tune.on?"training.combo.tune.stop":"training.combo.tune"),loc::T("training.combo.tune.detail"),!steps.empty())};
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
    if(a.kind!=MenuAction::Adjust||a.id.compare(0,3,"ct-")!=0) return;
    const auto index=static_cast<std::size_t>(std::atoi(a.id.c_str()+3));
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
void StorePattern() {
    auto* shown=CurrentCombo();
    const bool shownLine=shown&&(creator.steps.empty()||combo::JoinSteps(shown->steps)==creator.steps);
    if(!creator.steps.empty()||!shown) creator.steps=combo::JoinSteps(pattern);
    if(shownLine) { shown->steps=pattern; SaveCombos(); }
}
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
    // Each move keeps the frame it began on, relative to the first, as "#N",
    // so the replay presses on the recorded timing.
    std::vector<std::string> steps; int unnamed=0, first=-1;
    for(const auto& event:view.captured) {
        auto text=combo::ActionStep(files.moves,event.action,event.cancel&&!steps.empty());
        if(text.empty()) { ++unnamed; continue; }
        if(first<0) first=event.frame;
        combo::Step step; std::string ignored;
        if(combo::ParseStep(text,step,ignored)) {
            step.at=(std::min)(combo::MaxAtFrame,event.frame-first);
            if(event.offset!=training::NoOffset) step.offset=(std::max)(training::MinOffset,(std::min)(training::MaxOffset,event.offset));
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
std::vector<MenuEntry> ComboRows(const training::View& view,bool trialRunning) {
    LoadCombos();
    const auto* pack=CurrentPack(); const auto* shown=CurrentCombo();
    const auto* fighter=selection::FindFighter(creator.fighter);
    const char* none=loc::T("training.combo.none");
    // Pack and combo names are the player's own text and may be elided.
    auto userText=[](MenuEntry e){e.userText=true;return e;};
    auto reading=[](MenuEntry e){e.reading=true;return e;};
    return {
        userText(Value("cb-pack",loc::T("training.combo.pack"),pack?pack->name:none,loc::Tf("training.combo.pack.detail",pack?static_cast<int>(pack->combos.size()):0),creator.packs.size()>1)),
        TextRow("cb-new-pack",loc::T("training.combo.new_pack"),"",64),
        userText(Value("cb-combo",loc::T("training.combo.combo"),shown?shown->name.empty()?combo::JoinSteps(shown->steps):shown->name:none,
            shown?std::string(FighterName(shown->character))+": "+combo::JoinSteps(shown->steps)+(shown->notes.empty()?"":"\n"+shown->notes):loc::T("training.combo.tree.empty"),pack&&pack->combos.size()>1)),
        Row("cb-start-trial",loc::T("training.combo.run.start"),loc::T("training.combo.run.start.detail"),shown!=nullptr),
        Row("cb-stop-trial",loc::T("training.combo.run.stop"),loc::T("training.combo.run.stop.detail"),trialRunning),
        Value("cb-fighter",loc::T("training.combo.fighter"),fighter?fighter->name:"",loc::T("training.combo.fighter.detail")),
        TextRow("cb-name",loc::T("training.combo.name"),creator.name,64),
        TextRow("cb-steps",loc::T("training.combo.steps"),creator.steps,4096),
        TextRow("cb-notes",loc::T("training.combo.notes"),creator.notes,256),
        Row("cb-add",loc::T("training.combo.add"),loc::T("training.combo.add.detail"),!creator.steps.empty()),
        Row("cb-save",loc::T("training.combo.save"),loc::T("training.combo.save.detail"),shown!=nullptr&&!creator.steps.empty()),
        Row("cb-duplicate",loc::T("training.combo.duplicate"),loc::T("training.combo.duplicate.detail"),shown!=nullptr),
        Row("cb-capture",loc::T(view.capturing?"training.combo.capture_stop":"training.combo.capture"),loc::T("training.combo.capture.detail"),view.ready),
        Row("cb-replay",loc::T("training.combo.replay"),loc::T("training.combo.replay.detail"),!creator.steps.empty()||shown!=nullptr),
        Row("cb-timing",loc::T("training.combo.timing"),loc::T("training.combo.timing.detail"),!creator.steps.empty()||shown!=nullptr),
        Row("cb-blocks",loc::T("training.combo.blocks"),loc::T("training.combo.blocks.detail")),
        Row("cb-save-pos",loc::T("training.combo.save_pos"),loc::T("training.combo.save_pos.detail"),view.ready),
        Row("cb-reset-pos",loc::T("training.combo.reset_pos"),loc::T("training.combo.reset_pos.detail"),view.ready&&(view.checkpoint||(shown&&shown->placed))),
        Value("cb-reset-before",loc::T("training.combo.reset_before"),loc::T(creator.resetBeforeReplay?"common.on":"common.off"),loc::T("training.combo.reset_before.detail")),
        Value("cb-setup-action",loc::T("training.combo.setup.action"),SetupLabel(0,creator.setup.action),loc::T("training.combo.setup.detail")),
        Value("cb-setup-guard",loc::T("training.combo.setup.guard"),SetupLabel(1,creator.setup.guard),loc::T("training.combo.setup.detail")),
        Value("cb-setup-counter",loc::T("training.combo.setup.counter"),SetupLabel(2,creator.setup.counterHit),loc::T("training.combo.setup.detail")),
        Value("cb-setup-quick",loc::T("training.combo.setup.quick"),SetupLabel(3,creator.setup.quickStand),loc::T("training.combo.setup.detail")),
        Value("cb-setup-super",loc::T("training.combo.setup.super"),SetupLabel(4,creator.setup.super),loc::T("training.combo.setup.detail")),
        Value("cb-setup-revenge",loc::T("training.combo.setup.revenge"),SetupLabel(4,creator.setup.revenge),loc::T("training.combo.setup.detail")),
        Value("cb-replay-by",loc::T("training.combo.replay.by"),loc::T(creator.replayBy?"training.combo.replay.by.dummy":"training.combo.replay.by.me"),loc::T("training.combo.replay.by.detail")),
        Value("cb-replay-facing",loc::T("training.combo.replay.facing"),loc::T(creator.replayFacingRight?"training.combo.replay.facing.right":"training.combo.replay.facing.left"),loc::T("training.combo.replay.facing.detail")),
        Value("cb-replay-gap",loc::T("training.combo.replay.gap"),(creator.replayOffset>0?"+":"")+std::to_string(creator.replayOffset),loc::T("training.combo.replay.gap.detail")),
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
void HandleCombo(const MenuAction& a,const training::View& view,const TrainingSubmit& submit) {
    auto* pack=CurrentPack();
    if(a.kind==MenuAction::Adjust) {
        if(a.id=="cb-pack"&&pack) { creator.pack=(std::max)(0,(std::min)(static_cast<int>(creator.packs.size())-1,creator.pack+a.delta)); creator.entry=0; ShowCombo(); }
        else if(a.id=="cb-combo"&&pack) { creator.entry=(std::max)(0,(std::min)(static_cast<int>(pack->combos.size())-1,creator.entry+a.delta)); ShowCombo(); }
        else if(a.id=="cb-fighter") creator.fighter=(creator.fighter+(a.delta>0?1:selection::FighterCount-1))%selection::FighterCount;
        // The dummy starts on the right facing left, the player on the left facing right.
        else if(a.id=="cb-replay-by") { creator.replayBy=!creator.replayBy; creator.replayFacingRight=!creator.replayBy; }
        else if(a.id=="cb-replay-facing") creator.replayFacingRight=!creator.replayFacingRight;
        else if(a.id=="cb-replay-gap") creator.replayOffset=(std::max)(training::MinOffset,(std::min)(training::MaxOffset,creator.replayOffset+a.delta));
        else if(a.id=="cb-reset-before") creator.resetBeforeReplay=a.delta>0;
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
        else if(a.id=="cb-steps") creator.steps=combo::Clean(a.text);
        else if(a.id=="cb-notes") creator.notes=combo::Clean(a.text);
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
    if(a.id=="cb-add") {
        const auto* fighter=selection::FindFighter(creator.fighter);
        combo::Combo made{creator.name,fighter?fighter->code:"",creator.notes,{},creator.setup};
        if(!combo::ParseSteps(creator.steps,made.character,made.steps,error)||made.steps.empty()||made.steps.size()>combo::MaxSteps) {
            ComboNotice(loc::Tf("training.combo.invalid",error.empty()?creator.steps:error),true); return;
        }
        // The line is shown back as it was understood.
        creator.steps=combo::JoinSteps(made.steps);
        AddCombos({combo::Pack{pack?pack->name:DefaultPack,{made}}});
    } else if(a.id=="cb-save"&&CurrentCombo()) {
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
        if(submit&&submit(command)) { captureWanted=true; ComboNotice(loc::T(view.capturing?"training.combo.capture_wait":"training.combo.capture_started")); }
        else ComboNotice(loc::T("training.command_rejected"),true);
    } else if(a.id=="cb-save-pos") {
        Command command; command.action=Action::Save; command.generation=view.generation;
        const bool sent=submit&&submit(command);
        // The place goes with the shown combo, so it comes back with it.
        if(auto* shown=CurrentCombo(); sent&&shown) { shown->placed=true; shown->place[0]=view.x[0]; shown->place[1]=view.x[1]; SaveCombos(); }
        ComboNotice(loc::T(!sent?"training.command_rejected":CurrentCombo()?"training.combo.position_kept":"training.combo.position_saved"),!sent);
    } else if(a.id=="cb-reset-pos") {
        Command command;
        const bool sent=submit&&PlaceCommand(view,command)&&submit(command);
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
void SetComboBookDirectory(std::wstring directory) { if(creator.directory.empty()) creator.directory=std::move(directory); }

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
    const ImVec2 size((std::min)(820*Scale(),vp->Size.x*.8f),(std::min)(600*Scale(),vp->Size.y*.8f));
    // Compact typography independently of global DPI when the viewport cannot
    // accommodate the preferred panel. Other Ember windows keep their scale.
    const float unit=(std::min)(Scale(),(std::min)(size.x/500.f,size.y/500.f));
    // Top centre at first, then wherever it was dragged, so it can be moved
    // off the fight while a replay or trial runs under it.
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x*.5f,vp->Pos.y+8*unit),ImGuiCond_FirstUseEver,ImVec2(.5f,0));
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
  rows.push_back(TextRow("save-recording",loc::T("training.save_recording"),"",48,v.lengths[v.selected]>0&&!creator.directory.empty()));
  rows.back().detail=loc::T("training.save_recording.detail");
  rows.push_back(Value("load-recording",loc::T("training.load_recording"),recordings.empty()?loc::T("training.recording_none"):recordings[(std::min)(recordingChoice,recordings.size()-1)],loc::T("training.load_recording.detail"),!recordings.empty()&&ready&&v.mode==Mode::Idle));
 }else if(screen=="combos"){
  TakeCapture(v);
  rows=ComboRows(v,!v.trialSteps.empty());
 }else if(screen=="combo-timing"){
  TickTune(v,submit);
  rows=TimingRows(v);
 }else if(screen=="combo-blocks"){
  rows=PatternRows(pattern);
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
 if((screen=="combos"||screen=="recording"||screen=="combo-timing"||screen=="combo-blocks")&&!creator.notice.empty()){status=creator.notice;statusTone=creator.failed?Tone::Error:Tone::Success;}
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
  if(id.compare(0,3,"ct-")==0&&id!="ct-replay"&&id!="ct-none"){const auto steps=TimingSteps();const auto i=static_cast<std::size_t>(std::atoi(id.c_str()+3));
   if(i<steps.size())ComboLine({steps[i]},ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_Text));}
  if(id=="cb-steps"){std::vector<std::string> steps;std::string error;const auto* fighter=selection::FindFighter(creator.fighter);
   if(combo::ParseSteps(creator.steps,fighter?fighter->code:"",steps,error))ComboLine(steps,ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_Text));}
 },1,{},screen=="combo-blocks"?GameMenu::Body([&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction&,float height,const MenuVisualFeedback&){
  if(DrawPattern(pattern,entries,navigation,height,ImGui::GetFontSize()/ImGui::GetFont()->FontSize)) StorePattern();
 }):GameMenu::Body{},ImGui::GetFontSize()/ImGui::GetFont()->FontSize,100,false,statusTone);
 if(a.kind==MenuAction::Close||a.id=="return"){ForwardMenuAction({MenuAction::Close});return;}
 // F8 on the combo and timing screens replays the moves as they are now;
 // on a timing row, only up to that move, so one rep can be tuned at a time.
 if((screen=="combos"||screen=="combo-timing")&&!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_F8,false)){
  auto steps=TimingSteps();
  const auto& focus=nav.Focus();
  if(screen=="combo-timing"&&focus.compare(0,3,"ct-")==0&&focus!="ct-replay"&&focus!="ct-none"){
   const auto upTo=static_cast<std::size_t>(std::atoi(focus.c_str()+3));
   if(upTo+1<steps.size())steps.resize(upTo+1);
  }
  Replay(steps,v,submit);return;
 }
 if(a.kind==MenuAction::Activate&&screen=="home"){if(a.id=="recording")ListRecordings();nav.Push(a.id);return;}
 if(screen=="recording"&&HandleRecordingLibrary(a,v,submit))return;
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
    // Below the game's health bars and portraits, at the left edge.
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .02f, vp->Pos.y + vp->Size.y * .24f), ImGuiCond_Always);
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
        const int shown = (std::max)(3, (std::min)(12, static_cast<int>(vp->Size.y * .4f / ImGui::GetTextLineHeightWithSpacing()) - 2));
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
            float used = DrawComboStep(view.trialSteps[step], at, tint);
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
TrainingHudInput DrawTrainingHud(const training::View& view) {
    if (!view.available) return {};
    const auto* vp = ImGui::GetMainViewport();
    // Size the passive HUD to the game viewport; menu/DPI scaling should not
    // turn it into a large panel over the fight.
    const float hudScale = (std::max)(1.f, (std::min)(1.5f, vp->Size.y / 900.f));
    TrialList(view, hudScale);
    const float width = (std::min)(620 * hudScale, vp->Size.x * .75f);
    // The game's super meters and their SUPER! banners start about 17% above
    // the bottom edge and scale with the height, so the meter sits just above them.
    const float hudBottom = vp->Pos.y + vp->Size.y * TrainingHudBottom;
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
        Meter(view.meter, hudScale);
        ImGui::TextDisabled("%s", loc::Tf("training.frame_advantage",
            loc::T(view.meter.advantage.pending ? "training.measuring" : view.meter.advantage.knockdown ? "training.wakeup" : view.meter.frozen ? "training.held" : "training.live")).c_str());
        // The HUD is a NoInputs window, so IsItemHovered is always false here;
        // test the pointer against the item rectangle instead. The tooltip is
        // its own window and does not make the HUD capture input.
        if (!view.meter.advantage.valid && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false))
            ImGui::SetTooltip("%s", Unavailable(loc::T("training.advantage"), view.meter.advantage.unavailable).c_str());
        // Always one line, so the HUD does not jump as measurements come and go.
        const auto reason=MeasurementReason(view.meter);
        ImGui::TextDisabled("%s", FitLabel(reason.empty()?" ":reason,ImGui::GetContentRegionAvail().x).c_str());
        hudTop = ImGui::GetWindowPos();
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
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
        // F8 as a chip: play the selected slot again, or stop it.
        const bool playing = view.mode == training::Mode::Playback;
        if (playing || view.lengths[view.selected] > 0) {
            ImGui::SameLine();
            if (playing) input.stop = ImGui::SmallButton(loc::T("training.stop_chip"));
            else input.replay = ImGui::SmallButton(loc::Tf("training.replay_chip", view.selected + 1).c_str());
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", loc::T("training.hide_hint"));
        ImGui::SetWindowFontScale(1.f);
        input.pointer = ImGui::IsWindowHovered();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return input;
}
} }
