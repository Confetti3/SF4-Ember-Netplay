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
                ?std::vector<std::string>{"online","selection","profile","identity","settings","offline","training","replays"}
                :std::vector<std::string>{"player","online","selection","profile","identity","settings","offline","training","replays"};
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
        if(std::string(screen)=="dummy"||std::string(screen)=="reply")Require(trainingRows.size()==6,"Dummy or Reply page lost one of its own rows");
        if(std::string(screen)=="tools")Require(trainingRows.size()==4,"Position page lost one of its own rows");
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
    training.meter.frames.clear();
    for(int frame=0;frame<1200;++frame) {
        auto samples=fighters;
        for(auto& sample:samples){sample.valid=true;sample.status=frame<1080?19:16;sample.action=1;}
        training.meter.frames.push_back({samples,frame});
    }
    draw();
    const ImU32 oldColor=IM_COL32(232,91,103,215),newColor=(palette::Ember&~IM_COL32_A_MASK)|(215u<<IM_COL32_A_SHIFT);
    int cells=0;
    for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer){Require(vertex.col!=oldColor,"Meter drew frames older than the newest 120");if(vertex.col==newColor)++cells;}
    Require(cells==2*120*4,"Meter did not draw exactly 120 cells per player");
    // The F6 colour key must describe the bars: each entry's colour is the one
    // the meter draws for a status of that phase, and no two entries share one.
    {
        const auto keyEntries=TrainingColorKeyEntries();
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
                training.meter.frames.push_back({samples,frame});
            }
            draw();
            int drawn=0;
            for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer)if(vertex.col==entry.color)++drawn;
            Require(drawn>=2*120*4,"Colour key entry is not the colour the bars draw for its phase");
        }
    }
    training.meter=savedMeter;
    mode=6;training.watching=true;draw();
    Require(FindWindow("Training frame meter")->Size.x<=620*hudScale+1,"Match meter did not use Stable width");
    training.watching=false;mode=2;draw();
    SetMenuGlyphs(4,0,0);draw("training-hud-directinput");
    Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"DirectInput HUD captured input");
    SetMenuGlyphs(0,0,0);draw("training-hud-keyboard");SetMenuGlyphs(3,0x40000,0x20000);
}
}
