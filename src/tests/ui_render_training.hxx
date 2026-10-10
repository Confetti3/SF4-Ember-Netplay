#pragma once
// Home and Training render scenarios; draw uses the production OverlayLayers
// entry point for passive UI and alerts.
#include "ui_render_support.hxx"
#include "ui_render_match_hud.hxx"
#include "../common/Localization.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/TrainingPanel.hxx"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace {
template<class Size,class Draw>
void CheckHomeRendering(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,int& mode,const Size& size,const Draw& draw) {
    using namespace sf4e;using namespace ui;
    if(size.w==1920&&size.h==1080) {
        mode=0;shell.Navigation().Home();
        std::map<std::string,std::pair<ImVec2,ImVec2>> homeCards;
        std::vector<MenuEntry> homeRows;
        SetMenuCardProbe([&](const char* id,ImVec2 from,ImVec2 to){homeCards[id]={from,to};});
        SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){homeRows=rows;});
        for(bool controllerReady:{true,false}) {
            homeCards.clear();view.controllerReady=controllerReady;draw();
            const auto* list=FindWindow("Menu list");
            Require(list->ScrollMax.y==0,"Home requires scrolling at 1080p");
            const std::vector<std::string> expected=controllerReady
                ?std::vector<std::string>{"online","selection","profile","settings","offline","replays"}
                :std::vector<std::string>{"player","online","selection","profile","settings","offline","replays"};
            Require(homeRows.size()==expected.size(),"Home row count wrong");
            for(std::size_t i=0;i<expected.size();++i)Require(homeRows[i].id==expected[i],"Home row order wrong");
            for(const auto& row:homeRows) {
                const auto& rect=homeCards.at(row.id);
                Require(rect.first.y>=list->InnerRect.Min.y&&rect.second.y<=list->InnerRect.Max.y,"Home row is outside the list");
            }
        }
        SetMenuCardProbe({});SetMenuEntriesProbe({});view.controllerReady=true;
    }
}
template<class Size,class Draw>
void ShootTraining(sf4e::training::View& training,const std::array<sf4e::training::FighterSample,2>& fighters,
    sf4e::training::Command& trainingCommand,bool& acceptTraining,int& mode,const Size& size,const Draw& draw,ImGuiIO& io) {
    using namespace sf4e;using namespace ui;
    // The keyboard's prompts until the pad's own shots below.
    SetMenuGlyphs(input::PadKeyboard,0,0);
    mode=1;TrainingNavigation().Home();draw("training-home");
    // The reader labels the outcome as clearly as Startup and Recovery, in
    // every locale exercised by this render sweep.
    auto resultMeter=training.meter;
    resultMeter.moves[0].seen=true;resultMeter.moves[0].live=false;
    resultMeter.advantage.attacker=-1;resultMeter.advantage.pending=false;
    Require(TrainingFrameData(resultMeter,0).find(std::string(loc::T("training.result"))+": "+loc::T("training.meter.whiff"))!=std::string::npos,"Frame data whiff lost its Result label");
    resultMeter.advantage.attacker=0;resultMeter.advantage.valid=true;
    for(bool blocked:{false,true}){
        resultMeter.advantage.blocked=blocked;resultMeter.advantage.frames[0]=blocked?-2:3;
        const auto result=std::string(loc::T("training.result"))+": "+loc::T(blocked?"training.meter.block":"training.meter.hit")+(blocked?" -2":" +3");
        Require(TrainingFrameData(resultMeter,0).find(result)!=std::string::npos,"Frame data hit or block lost its labeled advantage");
    }
    Require(TrainingNavigation().Focus()=="recording","Removed practice position still occupies training root");
    // The dummy rows name the game's own settings, as the adapter reads them.
    training.dummy.action=1;training.dummy.guard=2;training.dummy.counterHit=1;training.dummy.quickStand=3;training.dummy.super=5;training.dummy.revenge=7;
    std::vector<MenuEntry> trainingRows;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){trainingRows=rows;});
    for(const char* screen:{"home","recording","history","frame-data","dummy","reply","tools"}){
        TrainingNavigation().Home();if(std::string(screen)!="home")TrainingNavigation().Push(screen);draw((std::string("training-")+screen).c_str());
        for(const auto& row:trainingRows)Require(row.id!="about","Training guide row survived the page split");
        if(std::string(screen)=="dummy")Require(trainingRows.size()==6,"Dummy page lost one of its own rows");
        if(std::string(screen)=="reply")Require(trainingRows.size()==7,"Reply page lost one of its own rows");
        if(std::string(screen)=="tools")Require(trainingRows.size()==4,"Position page lost one of its own rows");
        if(std::string(screen)=="frame-data")Require(trainingRows.size()==6,"Frame data page lost one of its own rows");
        if(std::string(screen)=="reply")for(const auto& row:trainingRows)if(row.id=="reply-slot"){
            Require(row.value==loc::Tf("training.slot",1),"Reply slot value includes recorded frame count");
            Require(row.detail.find(loc::Tf("training.recorded_frames",120))!=std::string::npos,"Reply slot detail lost recorded frame count");
        }
        if(std::string(screen)=="home") {
            const char* ids[]={"recording","history","frame-data","dummy","reply","tools","return"};
            Require(trainingRows.size()==7,"Training home lost a tool page");
            for(std::size_t i=0;i<trainingRows.size();++i)Require(trainingRows[i].id==ids[i],"Training home page order wrong");
        }
        if(size.w==1920&&size.h==1080&&size.dpi==1.5f) {
            auto* list=FindWindow("Menu list");
            if(list->ScrollMax.y!=0)throw std::runtime_error(std::string("Training page requires scrolling at 1080p / 150% DPI: ")+screen);
            const auto* flyout=FindWindow("###TrainingControls");
            for(const auto* child:GImGui->Windows)if(child->LastFrameActive==ImGui::GetFrameCount()&&child->RootWindow==flyout)
                Require(child->ScrollMax.y==0,"Training page child requires scrolling at 1080p / 150% DPI");
        }
    }
    SetMenuEntriesProbe({});
    // The meter's style and the HUD's recovery number are Frame data rows,
    // Angled and Off at first; Right picks Flat and turns the number on, and
    // the colour key names what the chosen style draws.
    {
        Require(!TrainingMeterOptions().flat&&!TrainingMeterOptions().recovery&&TrainingMeterOptions().shown==60,"Meter options are not Angled, Off and 60 frames at first");
        TrainingNavigation().Home();TrainingNavigation().Push("frame-data");draw();
        for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
        const auto focus=[&](const char* id){
            for(int i=0;i<20&&TrainingNavigation().Focus()!=id;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(TrainingNavigation().Focus()==id,"Frame data row unreachable");};
        focus("color-key");draw("training-color-key");
        focus("meter-style");draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().flat&&!TrainingMeterOptions().recovery,"Meter style row did not pick Flat");
        focus("frames-shown");draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().shown==90,"Frames shown row did not step to 90");
        draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().shown==120,"Frames shown row went past 120");
        focus("hud-recovery");draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().flat&&TrainingMeterOptions().recovery,"Recovery row did not turn the HUD number on");
        draw("training-meter-options");
        for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
        focus("color-key");draw("training-color-key-flat");
        SetTrainingMeterOptions({});
    }
    // The reply's pick list: Right from the reply slot picks the first preset,
    // which is sent to the dummy as typed notation is; Left goes back.
    {
        std::vector<MenuEntry> replyRows;
        SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){replyRows=rows;});
        TrainingNavigation().Home();TrainingNavigation().Push("reply");draw();
        for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
        for(int i=0;i<20&&TrainingNavigation().Focus()!="reply-move";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
        Require(TrainingNavigation().Focus()=="reply-move","Reply pick list unreachable");
        const auto pickValue=[&]{for(const auto& row:replyRows)if(row.id=="reply-move")return row.value;return std::string();};
        Require(pickValue()==loc::T("training.reply.slot"),"Reply pick list did not start on the reply slot");
        acceptTraining=true;trainingCommand={};draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(pickValue()=="623P"&&trainingCommand.action==training::Action::DummyPlan,"Reply preset was not sent as a plan");
        draw("training-reply-presets");
        draw(nullptr,MenuInput::Left,1);draw(nullptr,0,1);
        Require(pickValue()==loc::T("training.reply.slot"),"Left did not return the pick list to the reply slot");
        acceptTraining=false;
        SetMenuEntriesProbe({});
    }
    TrainingNavigation().Home();TrainingNavigation().Push("recording");draw();
    // Returning restores the prior selection, which may be below Record.
    for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
    for(int i=0;i<30&&TrainingNavigation().Focus()!="record";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
    Require(TrainingNavigation().Focus()=="record","Recording action unreachable");
    draw(nullptr,MenuInput::Select,1);draw("training-overwrite-confirmation");
    Require(TrainingNavigation().Confirming()&&!TrainingNavigation().ConfirmSelected(),"Training overwrite default is not Cancel");
    draw(nullptr,MenuInput::Back,1);draw();
    Require(TrainingNavigation().Screen()=="recording"&&!TrainingNavigation().Confirming(),"Training Back did more than cancel");
    acceptTraining=true;draw(nullptr,MenuInput::Down,1);draw();draw(nullptr,MenuInput::Select,1);draw("training-pending");
    training.acks.Note(trainingCommand.requestId,false);
    draw("training-command-error");Require((TakeForwardedMenuAction().kind!=MenuAction::Close),"Failed training command closed flyout");
    training.ready=false;TrainingNavigation().Home();TrainingNavigation().Push("recording");draw("training-unavailable");
    training.ready=true;training.mode=training::Mode::Recording;draw("training-recording-suspended");training.mode=training::Mode::Idle;
    // An Xbox pad's press turns the controls' legend to its buttons.
    SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B);
    // A new battle clears the failed command left by the shots above.
    ++training.generation;TrainingNavigation().Home();draw();draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);
    Require(!KeyboardPrompts(),"A pad press left the training controls on keyboard prompts");
    draw("training-flyout-pad");--training.generation;
    draw();draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);
    Require(TrainingNavigation().Focus()=="recording","The pad shot left the training root off its first row");
    SetMenuGlyphs(input::PadKeyboard,0,0);
    mode=2;draw("training-hud");
    Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Passive training HUD captured input");
    CheckMatchHudScales();
    const float hudScale=(std::max)(1.f,(std::min)(1.5f,size.h/900.f));
    auto* hud=FindWindow("Training frame meter");Require(hud->Size.x<=620*hudScale+1&&hud->Size.x<=size.w*.76f&&hud->Size.y<size.h*.17f,"Passive HUD too large");
    Require(hud->Pos.y+hud->Size.y<=size.h*.82f+1,"Training HUD covers the game's super meters");
    auto* chips=FindWindow("Training shortcuts");
    Require(chips->Pos.y+chips->Size.y<=hud->Pos.y-3*hudScale,"Training shortcuts are not entirely above the meter");
    for(const auto* window:GImGui->Windows)if(window->LastFrameActive==ImGui::GetFrameCount()&&window->Name[0]!='#'&&std::string(window->Name)!="Debug##Default")
        Require(window->Pos.y+window->Size.y<=hud->Pos.y+hud->Size.y+1,"Training window sits between meter and super gauges");
    const auto savedMeter=training.meter;
    // Both styles, at each frames-shown value, draw exactly the newest frames
    // it names and no older one, keep the meter's size, and at the default
    // zoom give each cell room to count by eye. Flat keeps Stable's colours
    // exactly: an attack in Ember, at Stable's alpha.
    {
        training::FighterSample down,attack;down.valid=attack.valid=true;down.status=19;attack.status=16;down.action=attack.action=1;
        Require(TrainingCellColor(training::MeterCellOf(attack),false)==((palette::Ember&~IM_COL32_A_MASK)|(215u<<IM_COL32_A_SHIFT))&&
            TrainingCellColor(training::MeterCellOf(down),false)==IM_COL32(178,123,210,215),"Flat bars left Stable's colours");
        std::map<std::string,std::pair<ImVec2,ImVec2>> bars;
        SetMenuCardProbe([&](const char* id,ImVec2 from,ImVec2 to){bars[id]={from,to};});
        for(const int shown:training::MeterShownChoices)for(bool angled:{true,false}) {
            training::MeterOptions options;options.flat=!angled;options.shown=shown;SetTrainingMeterOptions(options);
            training.meter.frames.clear();
            for(int frame=0;frame<1200;++frame)training::AppendMeterFrame(training.meter.frames,{{frame<1200-shown?down:attack,frame<1200-shown?down:attack}},frame);
            draw();
            const ImU32 oldColor=TrainingCellColor(training::MeterCellOf(down),angled),newColor=TrainingCellColor(training::MeterCellOf(attack),angled);
            int cells=0;
            for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer){Require(vertex.col!=oldColor,"Meter drew frames older than the newest it shows");if(vertex.col==newColor)++cells;}
            Require(cells==2*shown*4,"Meter did not draw exactly the frames shown for each player");
            hud=FindWindow("Training frame meter");Require(hud->Size.x<=620*hudScale+1&&hud->Size.y<size.h*.17f,"Frames shown changed the meter's size");
            Require(hud->Pos.y+hud->Size.y<=size.h*.82f+1,"Frames shown moved the meter over the game's super meters");
            const auto& bar=bars.at("training-bar-1");
            const float cell=(bar.second.x-bar.first.x-(angled?(bar.second.y-bar.first.y)*.35f:0))/shown;
            // Where the meter has its full width (not capped by a narrow
            // viewport), the default zoom gives at least 7 px a cell at 720p
            // (7.6 in English), more as the HUD scales (9.1 at 1080p).
            if(shown==60&&size.w*.75f>=620*hudScale&&std::string(loc::T("common.on"))=="On"){
                Require(cell>=7*hudScale,"Default zoom leaves frame cells too narrow to count");
            }
        }
        SetMenuCardProbe({});SetTrainingMeterOptions({});
        // A held meter scrolls back with the mouse wheel over the bars: six
        // notches show the oldest 60 frames it keeps, and running again
        // shows the newest.
        training.meter.frames.clear();
        for(int frame=0;frame<121;++frame)training::AppendMeterFrame(training.meter.frames,{{frame<60?down:attack,frame<60?down:attack}},frame);
        training.meter.frozen=true;draw();
        SetMenuCardProbe([&](const char* id,ImVec2 from,ImVec2 to){bars[id]={from,to};});draw();SetMenuCardProbe({});
        const auto& bar=bars.at("training-bar-1");
        io.AddMousePosEvent((bar.first.x+bar.second.x)/2,(bar.first.y+bar.second.y)/2);io.AddMouseWheelEvent(0,6);draw();
        const auto count=[&](ImU32 colour){int n=0;for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer)if(vertex.col==colour)++n;return n;};
        Require(count(TrainingCellColor(training::MeterCellOf(down),true))==2*59*4&&count(TrainingCellColor(training::MeterCellOf(attack),true))==2*1*4,"Scrolling a held meter back did not show its oldest frames");
        io.AddMouseWheelEvent(0,-6);draw();
        Require(count(TrainingCellColor(training::MeterCellOf(attack),true))==2*60*4,"Scrolling forward did not return to the newest frames");
        io.AddMouseWheelEvent(0,6);draw();training.meter.frozen=false;draw();
        Require(count(TrainingCellColor(training::MeterCellOf(attack),true))==2*60*4,"A running meter stayed scrolled back");
        io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);draw();
    }
    // The F6 colour key must describe the flat bars: each entry's colour is the
    // one they draw for a status of that phase, and no two entries share one.
    {
        SetTrainingMeterOptions({true,false});
        const auto keyEntries=TrainingColorKeyEntries(false);
        Require(keyEntries.size()==6,"Colour key does not list the phases the bars draw");
        std::set<unsigned> keyColors;
        for(const auto& entry:keyEntries) {
            Require(keyColors.insert(entry.color).second,"Two colour key entries share a colour");
            Require(*loc::T(entry.label)!=0,"Colour key entry has no label");
            unsigned status=0;
            while(status<40&&training::ClassifyStatus(status)!=entry.phase)++status;
            Require(status<40,"Colour key phase has no native status to draw");
            training.meter.frames.clear();
            for(int frame=0;frame<120;++frame) {
                auto samples=fighters;
                for(auto& sample:samples){sample.valid=true;sample.status=status;sample.action=1;}
                training::AppendMeterFrame(training.meter.frames,samples,frame);
            }
            draw();
            int drawn=0;
            for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer)if(vertex.col==entry.color)++drawn;
            Require(drawn>=2*TrainingMeterOptions().shown*4,"Colour key entry is not the colour the bars draw for its phase");
        }
        SetTrainingMeterOptions({});
    }
    // And the angled bars: each key entry is the colour they draw for a cell
    // of its kind, and every entry has a colour of its own.
    {
        const auto keyEntries=TrainingColorKeyEntries(true);
        Require(keyEntries.size()==9,"Angled colour key does not list what the angled bars draw");
        std::set<unsigned> keyColors;
        std::vector<training::FighterSample> candidates;
        for(unsigned status=0;status<30;++status) {
            training::FighterSample sample;sample.valid=true;sample.status=status;sample.action=1;
            candidates.push_back(sample);
            sample.firstActiveFrame=10;sample.lastActiveFrame=14;
            for(float frame:{5.f,12.f,20.f}){sample.actionFrame=frame;candidates.push_back(sample);}
        }
        for(const auto& entry:keyEntries) {
            Require(keyColors.insert(entry.color).second,"Two angled colour key entries share a colour");
            Require(*loc::T(entry.label)!=0,"Angled colour key entry has no label");
            const auto found=std::find_if(candidates.begin(),candidates.end(),[&](const training::FighterSample& sample){return training::ClassifyMeter(sample)==entry.kind;});
            Require(found!=candidates.end(),"Angled colour key kind has no sample to draw");
            Require(TrainingCellColor(training::MeterCellOf(*found),true)==entry.color,"Angled colour key entry is not the cell colour of its kind");
            training.meter.frames.clear();
            for(int frame=0;frame<120;++frame) {
                std::array<training::FighterSample,2> samples{{*found,*found}};
                training::AppendMeterFrame(training.meter.frames,samples,frame);
            }
            draw();
            int drawn=0;
            for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer)if(vertex.col==entry.color)++drawn;
            Require(drawn>=2*TrainingMeterOptions().shown*4,"Angled colour key entry is not the colour the bars draw for its kind");
        }
        // Rise is drawn as Knockdown, with no entry of its own.
        training::FighterSample rising;rising.valid=true;rising.status=20;
        Require(TrainingCellColor(training::MeterCellOf(rising),true)==TrainingCellColor(training::MeterCellOf(rising),false),"Getting up left the Knockdown colour");
    }
    training.meter=savedMeter;
    // The default zoom flat, the whole history at once, and the recovery
    // number keep the meter's size.
    for(const auto& shot:{std::make_pair("training-hud-flat",training::MeterOptions{true,false,60}),std::make_pair("training-hud-120",training::MeterOptions{false,false,120}),
        std::make_pair("training-hud-recovery",training::MeterOptions{false,true,60})}) {
        SetTrainingMeterOptions(shot.second);draw(shot.first);
        hud=FindWindow("Training frame meter");Require(hud->Size.x<=620*hudScale+1&&hud->Size.y<size.h*.17f,"Meter option made the HUD too large");
        Require(hud->Pos.y+hud->Size.y<=size.h*.82f+1,"Meter option moved the HUD over the game's super meters");
    }
    SetTrainingMeterOptions({});
    // Run lengths and counting marks: P1's jab and a special it cancels into
    // each count their own startup, active and recovery; P2's two hits of the
    // combo count apart; every fifth and tenth cell boundary is marked.
    {
        training::MeterView runs;
        const auto attack=[](int action,int frame,int first,int last){training::FighterSample s;s.valid=true;s.status=16;s.action=action;
            s.actionFrame=static_cast<float>(frame);s.firstActiveFrame=first;s.lastActiveFrame=last;return s;};
        for(int f=0;f<120;++f) {
            std::array<training::FighterSample,2> s;
            for(auto& fighter:s){fighter.valid=true;fighter.status=0;fighter.action=0;}
            if(f>=12&&f<32)s[0]=attack(200,f-11,5,8);
            if(f>=60&&f<68)s[0]=attack(201,f-59,3,5);
            if(f>=68&&f<107)s[0]=attack(202,f-67,10,14);
            if(f>=18&&f<38){s[1].status=21;s[1].action=300;s[1].actionFrame=static_cast<float>(f-17);s[1].comboDamage=40;}
            if(f>=64&&f<96){s[1].status=21;s[1].action=300;s[1].actionFrame=static_cast<float>(f<80?f-63:f-79);s[1].comboDamage=f<80?50.f:120.f;}
            training::AppendMeterFrame(runs.frames,s,f);
        }
        runs.startupFrames={{5,-1}};runs.moves[0].seen=true;runs.moves[0].active=4;runs.moves[0].recovery=25;
        runs.advantage.valid=true;runs.advantage.frames={{3,-3}};runs.advantage.attacker=0;
        training.meter=runs;SetTrainingMeterOptions({false,true,60});draw("training-hud-markers");
        SetTrainingMeterOptions({false,true,120});draw("training-hud-runs");
        SetTrainingMeterOptions({});training.meter=savedMeter;
    }
    mode=6;training.watching=true;draw();
    Require(FindWindow("Training frame meter")->Size.x<=620*hudScale+1,"Match meter did not use Stable width");
    training.watching=false;mode=2;draw();
    // After a pad press the chip names the pad's Back and Start, and the hint
    // its Back; a key press, or the keyboard as the device, names F6 and F5.
    for(const int device:{input::PadXInput,input::PadDirectInput}) {
        SetMenuGlyphs(device,device==input::PadXInput?input::xinput::A:0,device==input::PadXInput?input::xinput::B:0);
        NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);
        Require(MenuPromptDevice()==device,"A pad press did not turn the HUD's prompts to the pad");
        draw(device==input::PadXInput?"training-hud-pad":"training-hud-directinput");
        Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Pad HUD captured input");
    }
    io.AddKeyEvent(ImGuiKey_F5,true);draw(nullptr,0,1);NoteMenuDevice(0);io.AddKeyEvent(ImGuiKey_F5,false);draw(nullptr,0,1);
    Require(MenuPromptDevice()==input::PadKeyboard,"A function key did not turn the HUD's prompts to the keyboard");
    SetMenuGlyphs(0,0,0);draw("training-hud-keyboard");SetMenuGlyphs(3,0x40000,0x20000);
}
}
