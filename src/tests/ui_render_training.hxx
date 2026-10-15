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
#include <cmath>
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
// retheme(dpi) applies the theme at that DPI and rebuilds the renderer's atlas.
template<class Size,class Draw,class Retheme>
void ShootTraining(sf4e::training::View& training,const std::array<sf4e::training::FighterSample,2>& fighters,
    sf4e::training::Command& trainingCommand,bool& acceptTraining,int& mode,const Size& size,const Draw& draw,ImGuiIO& io,const Retheme& retheme) {
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
    // A blocked attack, then another move that whiffed: the page gives the
    // whiff's own result beside its frames and the block apart, as the last advantage.
    {
        auto mixed=resultMeter;mixed.lastAdvantage=resultMeter.advantage;mixed.advantage=training::FrameAdvantage{};
        const auto text=TrainingFrameData(mixed,0);
        const auto label=[](const char* id,const std::string& value){return std::string(loc::T(id))+": "+value;};
        Require(text.find(label("training.result",loc::T("training.meter.whiff")))!=std::string::npos&&
            text.find(label("training.result",loc::T("training.meter.block")))==std::string::npos&&
            text.find(label("training.last_advantage",std::string(loc::T("training.meter.block"))+" -2"))!=std::string::npos,
            "Frame data told a whiff's frames with the blocked attack before it");
    }
    Require(TrainingNavigation().Focus()=="recording","Removed practice position still occupies training root");
    // The dummy rows name the game's own settings, as the adapter reads them.
    training.dummy.action=1;training.dummy.guard=2;training.dummy.counterHit=1;training.dummy.quickStand=3;training.dummy.super=5;training.dummy.revenge=7;
    std::vector<MenuEntry> trainingRows;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){trainingRows=rows;});
    for(const char* screen:{"home","recording","history","frame-data","hud","meter-settings","meter-classic","dummy","reply","tools"}){
        TrainingNavigation().Home();if(std::string(screen)!="home")TrainingNavigation().Push(screen);draw((std::string("training-")+screen).c_str());
        for(const auto& row:trainingRows)Require(row.id!="about","Training guide row survived the page split");
        if(std::string(screen)=="dummy")Require(trainingRows.size()==6,"Dummy page lost one of its own rows");
        if(std::string(screen)=="reply")Require(trainingRows.size()==6,"Reply page lost one of its own rows");
        if(std::string(screen)=="tools")Require(trainingRows.size()==4,"Position page lost one of its own rows");
        if(std::string(screen)=="frame-data")Require(trainingRows.size()==6,"Frame data page lost one of its own rows");
        if(std::string(screen)=="hud")Require(trainingRows.size()==8&&trainingRows[0].id=="hud-size"&&trainingRows[7].id=="meter-settings","HUD page lost its size or one of its parts");
        // The reply plays the slots that are on, so its page has no slot of its own.
        if(std::string(screen)=="reply")for(const auto& row:trainingRows)Require(row.id!="reply-slot","The reply page kept a slot of its own");
        // Each slot says whether it is on, or that it is empty, with its frames in the detail.
        if(std::string(screen)=="recording"){
            Require(trainingRows.size()==training::SlotCount+7&&trainingRows[0].value==loc::T("common.on")&&trainingRows[2].value==loc::T("common.off")&&
                trainingRows[1].value==loc::T("training.slot.empty")&&trainingRows[0].detail.find("120")!=std::string::npos&&!trainingRows[1].adjustable,
                "The recording page did not show each slot on, off or empty");
            const auto order=std::find_if(trainingRows.begin(),trainingRows.end(),[](const MenuEntry& row){return row.id=="order";});
            Require(order!=trainingRows.end()&&order->value==loc::T("training.order.random"),"The recording page lost its order row");
        }
        if(std::string(screen)=="home") {
            const char* ids[]={"recording","history","frame-data","hud","dummy","reply","tools","return"};
            Require(trainingRows.size()==8,"Training home lost a tool page");
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
        Require(TrainingMeterOptions().style==training::MeterStyle::Angled&&!TrainingMeterOptions().recovery&&TrainingMeterOptions().shown==60,"Meter options are not Angled, Off and 60 frames at first");
        TrainingNavigation().Home();TrainingNavigation().Push("frame-data");draw();
        for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
        const auto focus=[&](const char* id){
            for(int i=0;i<20&&TrainingNavigation().Focus()!=id;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(TrainingNavigation().Focus()==id,"Frame data row unreachable");};
        focus("color-key");draw("training-color-key");
        focus("meter-style");draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().style==training::MeterStyle::Flat&&!TrainingMeterOptions().recovery,"Meter style row did not pick Flat");
        focus("frames-shown");draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().shown==90,"Frames shown row did not step to 90");
        draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().shown==120,"Frames shown row went past 120");
        focus("hud-recovery");draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(TrainingMeterOptions().style==training::MeterStyle::Flat&&TrainingMeterOptions().recovery,"Recovery row did not turn the HUD number on");
        draw("training-meter-options");
        for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
        focus("color-key");draw("training-color-key-flat");
        SetTrainingMeterOptions({});
    }
    // The HUD page: standard size and every part on at first. Left on the
    // size makes the HUD smaller, and on a part turns it off.
    TrainingNavigation().Home();TrainingNavigation().Push("hud");draw();
    for(int i=0;i<6;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
    Require(TrainingMeterOptions().size==1,"The HUD is not standard size at first");
    draw(nullptr,MenuInput::Left,1);draw(nullptr,0,1);
    Require(TrainingMeterOptions().size==0,"HUD page did not make the HUD smaller");
    draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Left,1);draw(nullptr,0,1);
    Require(!TrainingMeterOptions().numbers&&TrainingMeterOptions().bars,"HUD page did not turn its first part off");
    SetTrainingMeterOptions({});
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
        Require(pickValue()==loc::T("training.reply.recordings"),"Reply pick list did not start on the recordings");
        acceptTraining=true;trainingCommand={};draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);
        Require(pickValue()=="623P"&&trainingCommand.action==training::Action::DummyPlan,"Reply preset was not sent as a plan");
        draw("training-reply-presets");
        draw(nullptr,MenuInput::Left,1);draw(nullptr,0,1);
        Require(pickValue()==loc::T("training.reply.recordings"),"Left did not return the pick list to the recordings");
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
    training.ready=true;training.mode=training::Mode::Recording;draw("training-recording-suspended");
    training.mode=training::Mode::Standby;draw("training-standby");training.mode=training::Mode::Idle;
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
    const float hudScale=TrainingHudScale();
    auto* hud=FindWindow("Training frame meter");Require(hud->Size.x<=620*hudScale+1&&hud->Size.x<=size.w*.76f&&hud->Size.y<size.h*.17f,"Passive HUD too large");
    Require(hud->Pos.y+hud->Size.y<=size.h*.82f+1,"Training HUD covers the game's super meters");
    auto* chips=FindWindow("Training shortcuts");
    Require(chips->Pos.y+chips->Size.y<=hud->Pos.y-3*hudScale,"Training shortcuts are not entirely above the meter");
    for(const auto* window:GImGui->Windows)if(window->LastFrameActive==ImGui::GetFrameCount()&&window->Name[0]!='#'&&std::string(window->Name)!="Debug##Default")
        Require(window->Pos.y+window->Size.y<=hud->Pos.y+hud->Size.y+1,"Training window sits between meter and super gauges");
    const auto savedMeter=training.meter;
    // Both styles, at each frames-shown value, draw exactly the page with the
    // newest frame and no earlier one, a full page and one just begun, keep
    // the meter's size, and at the default zoom give each cell room to count
    // by eye. Flat keeps Stable's colours exactly: an attack in Ember, at
    // Stable's alpha.
    {
        training::FighterSample down,attack;down.valid=attack.valid=true;down.status=19;attack.status=16;down.action=attack.action=1;
        Require(TrainingCellColor(training::MeterCellOf(attack),false)==((palette::Ember&~IM_COL32_A_MASK)|(215u<<IM_COL32_A_SHIFT))&&
            TrainingCellColor(training::MeterCellOf(down),false)==IM_COL32(178,123,210,215),"Flat bars left Stable's colours");
        std::map<std::string,std::pair<ImVec2,ImVec2>> bars;
        SetMenuCardProbe([&](const char* id,ImVec2 from,ImVec2 to){bars[id]={from,to};});
        for(const int shown:training::MeterShownChoices)for(bool angled:{true,false})for(const int last:{shown,7}) {
            training::MeterOptions options;options.style=angled?training::MeterStyle::Angled:training::MeterStyle::Flat;options.shown=shown;SetTrainingMeterOptions(options);
            // Ten pages, then the last frames on a page of their own unless they fill one.
            const int total=10*shown+(last==shown?0:last);
            training.meter.frames.clear();training.meter.length=0;
            for(int frame=0;frame<total;++frame)training::AppendMeterFrame(training.meter.frames,{{frame<total-last?down:attack,frame<total-last?down:attack}},frame);
            draw();
            const ImU32 oldColor=TrainingCellColor(training::MeterCellOf(down),angled),newColor=TrainingCellColor(training::MeterCellOf(attack),angled);
            int cells=0;
            for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer){Require(vertex.col!=oldColor,"Meter drew frames of an earlier page");if(vertex.col==newColor)++cells;}
            Require(cells==2*last*4,"Meter did not draw exactly the newest frame's page for each player");
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
        // A held meter steps back a page a notch of the mouse wheel over the
        // bars, no further than its oldest frame; Frames shown changed while
        // it is stepped back keeps it within its frames; running again shows
        // the newest frame's page.
        training.meter.frames.clear();
        for(int frame=0;frame<120;++frame)training::AppendMeterFrame(training.meter.frames,{{frame<60?down:attack,frame<60?down:attack}},frame);
        training.meter.length=120;training.meter.frozen=true;draw();
        SetMenuCardProbe([&](const char* id,ImVec2 from,ImVec2 to){bars[id]={from,to};});draw();SetMenuCardProbe({});
        const auto& bar=bars.at("training-bar-1");
        const auto count=[&](ImU32 colour){int n=0;for(const auto& vertex:FindWindow("Training frame meter")->DrawList->VtxBuffer)if(vertex.col==colour)++n;return n;};
        const ImU32 downColor=TrainingCellColor(training::MeterCellOf(down),true),attackColor=TrainingCellColor(training::MeterCellOf(attack),true);
        Require(count(downColor)==0&&count(attackColor)==2*60*4,"A held meter did not show its newest frame's page");
        io.AddMousePosEvent((bar.first.x+bar.second.x)/2,(bar.first.y+bar.second.y)/2);io.AddMouseWheelEvent(0,6);draw();
        Require(count(downColor)==2*60*4&&count(attackColor)==0,"Stepping a held meter back did not stop at its first page");
        io.AddMouseWheelEvent(0,-1);draw();
        Require(count(downColor)==0&&count(attackColor)==2*60*4,"A notch forward did not return to the newest frame's page");
        io.AddMouseWheelEvent(0,1);draw();
        {training::MeterOptions wide;wide.shown=120;SetTrainingMeterOptions(wide);draw();}
        Require(count(downColor)==2*60*4&&count(attackColor)==2*60*4,"Frames shown changed while stepped back left the frames kept");
        SetTrainingMeterOptions({});io.AddMouseWheelEvent(0,1);draw();training.meter.frozen=false;draw();
        Require(count(attackColor)==2*60*4,"A running meter stayed stepped back");
        io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);draw();training.meter.length=0;
    }
    // The F6 colour key must describe the flat bars: each entry's colour is the
    // one they draw for a status of that phase, and no two entries share one.
    {
        SetTrainingMeterOptions({training::MeterStyle::Flat,false});
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
    for(const auto& shot:{std::make_pair("training-hud-flat",training::MeterOptions{training::MeterStyle::Flat,false,60}),std::make_pair("training-hud-120",training::MeterOptions{training::MeterStyle::Angled,false,120}),
        std::make_pair("training-hud-recovery",training::MeterOptions{training::MeterStyle::Angled,true,60})}) {
        SetTrainingMeterOptions(shot.second);draw(shot.first);
        hud=FindWindow("Training frame meter");Require(hud->Size.x<=620*hudScale+1&&hud->Size.y<size.h*.17f,"Meter option made the HUD too large");
        Require(hud->Pos.y+hud->Size.y<=size.h*.82f+1,"Meter option moved the HUD over the game's super meters");
    }
    SetTrainingMeterOptions({});
    // Standby, a recording and a playback add their strip, and the meter stays
    // clear of the gauges. The typed reply plays from a slot of its own and adds none.
    const float whole=hud->Size.y;
    training.active=0;training.activeLength=120;
    for(const auto& shot:{std::make_pair("training-hud-standby",training::Mode::Standby),std::make_pair("training-hud-recording",training::Mode::Recording),
        std::make_pair("training-hud-playback",training::Mode::Playback)}) {
        training.mode=shot.second;training.cursor=shot.second==training::Mode::Standby?0:45;draw(shot.first);
        Require(hud->Size.y>whole&&hud->Size.y<size.h*.17f&&hud->Pos.y+hud->Size.y<=size.h*.82f+1,"Recording strip is missing or made the HUD too large");
    }
    training.active=training::ReplySlot;draw();
    Require(std::fabs(hud->Size.y-whole)<1,"The typed reply drew a playback strip");
    training.mode=training::Mode::Idle;training.cursor=0;training.active=-1;training.activeLength=0;
    // The HUD's parts: the bars alone are a smaller meter with no chip over it, and with every part off nothing is drawn.
    training::MeterOptions parts;parts.numbers=parts.state=parts.shortcuts=false;SetTrainingMeterOptions(parts);draw("training-hud-bars");
    Require(hud->Size.y<whole*.7f&&chips->LastFrameActive!=ImGui::GetFrameCount(),"HUD parts did not turn off");
    parts.bars=false;SetTrainingMeterOptions(parts);draw();
    Require(hud->LastFrameActive!=ImGui::GetFrameCount(),"HUD drew with every part off");
    SetTrainingMeterOptions({});
    // The input display: one pad a side, on the screen, beside the meter where there is room.
    parts=training::MeterOptions{};
    for(const auto inputs:{training::InputDisplay::Stick,training::InputDisplay::Arrows,training::InputDisplay::Both}){
        parts.inputs=inputs;SetTrainingMeterOptions(parts);
        draw(inputs==training::InputDisplay::Stick?"training-hud-inputs-stick":inputs==training::InputDisplay::Arrows?"training-hud-inputs-arrows":"training-hud-inputs-both");
        for(const char* name:InputDisplayWindows){
            auto* pad=FindWindow(name);
            Require(pad&&pad->LastFrameActive==ImGui::GetFrameCount()&&pad->Pos.x>=0&&pad->Pos.x+pad->Size.x<=size.w+1&&pad->Pos.y+pad->Size.y<=size.h*.82f+1,"Input display left the screen or covers the super meters");
            if(size.w>=1280)Require(pad->Pos.x+pad->Size.x<=hud->Pos.x||pad->Pos.x>=hud->Pos.x+hud->Size.x,"Input display covers the meter");
        }
    }
    SetTrainingMeterOptions({});
    // A small window: the HUD shrinks with it and keeps its place over the
    // gauges, at every size and while it records.
    {
        const char* names[training::HudSizes][2]={{"training-hud-small-small","training-hud-small-small-recording"},
            {"training-hud-small","training-hud-small-recording"},{"training-hud-small-large","training-hud-small-large-recording"}};
        training::MeterOptions sized;
        for(int hudSize=0;hudSize<training::HudSizes;++hudSize)for(const bool recording:{false,true}){
            sized.size=hudSize;SetTrainingMeterOptions(sized);training.mode=recording?training::Mode::Recording:training::Mode::Idle;
            io.DisplaySize=ImVec2(640,480);draw(names[hudSize][recording]);
            Require(hud->Size.x<=640*.76f&&hud->Size.y<480*.17f&&hud->Pos.y+hud->Size.y<=480*.82f+1,"Training HUD does not fit a small window");
        }
        training.mode=training::Mode::Idle;SetTrainingMeterOptions({});
    }
    io.DisplaySize=ImVec2(static_cast<float>(size.w),static_cast<float>(size.h));draw();
    // Each size is larger than the one before, and the largest still fits
    // over the gauges with a recording's strip.
    {
        training::MeterOptions sized;float previous=0;
        for(int hudSize=0;hudSize<training::HudSizes;++hudSize){
            sized.size=hudSize;SetTrainingMeterOptions(sized);
            draw(hudSize==0?"training-hud-size-small":hudSize==2?"training-hud-size-large":nullptr);
            Require(TrainingHudScale()>previous&&hud->Size.x<=size.w*.76f&&hud->Size.y<size.h*.17f,"HUD sizes do not grow or the largest is too large");
            previous=TrainingHudScale();
        }
        training.mode=training::Mode::Recording;draw("training-hud-size-large-recording");
        Require(hud->Size.y<size.h*.17f&&hud->Pos.y+hud->Size.y<=size.h*.82f+1,"The largest HUD with a recording's strip is too large");
        training.mode=training::Mode::Idle;SetTrainingMeterOptions({});draw();
    }
    // Run lengths and counting marks: P1's jab and a special it cancels into
    // each count their own startup, active and recovery; P2's two hits of the
    // combo count apart; every fifth and tenth cell boundary is marked. At 60
    // frames shown the bars show the second page, the cancel and the combo.
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
        training.meter=runs;SetTrainingMeterOptions({training::MeterStyle::Angled,true,60});draw("training-hud-markers");
        SetTrainingMeterOptions({training::MeterStyle::Angled,true,120});draw("training-hud-runs");
        // The next exchange is being measured: the last reading stays, dimmed,
        // and Frame data still gives it.
        runs.lastAdvantage=runs.advantage;runs.advantage=training::FrameAdvantage{};runs.advantage.pending=true;runs.advantage.attacker=1;
        runs.advantage.unavailable=training::MeasurementUnavailable::MeasuringRecovery;
        training.meter=runs;SetTrainingMeterOptions({training::MeterStyle::Angled,true,120});draw("training-hud-measuring");
        Require(TrainingFrameData(runs,0).find(std::string(loc::T("training.meter.hit"))+" +3")!=std::string::npos,"Frame data dropped the last reading while the next was measured");
        // A focus cancel: the HUD and Frame data both give the move it
        // cancelled, its recovery cancelled and the dash, from the one reading.
        {
            auto cancel=runs;cancel.advantage=training::FrameAdvantage{};cancel.advantage.valid=true;cancel.advantage.blocked=true;
            cancel.advantage.attacker=0;cancel.advantage.frames={{-2,2}};cancel.startupFrames={{-1,-1}};
            cancel.startupUnavailable[0]=training::MeasurementUnavailable::WaitingForAttackBoundary;
            auto& chain=cancel.advantage.chain;chain.valid=chain.recoveryCancelled=true;chain.side=0;chain.startup=3;chain.active=2;
            chain.provenance=training::BoundaryProvenance::BacActionHeader;chain.startupUnavailable=training::MeasurementUnavailable::None;
            chain.dash=training::ActionClass::ForwardDash;cancel.lastAdvantage=cancel.advantage;
            training.meter=cancel;SetTrainingMeterOptions({training::MeterStyle::Angled,true,60});draw("training-hud-focus-cancel");
            const auto text=TrainingFrameData(cancel,0);
            const auto label=[](const char* id,const std::string& value){return std::string(loc::T(id))+": "+value;};
            Require(text.find(label("training.startup","3"))!=std::string::npos&&text.find(label("training.meter.recovery",loc::T("training.meter.cancelled")))!=std::string::npos&&
                text.find(loc::T("training.meter.focus_forward"))!=std::string::npos&&text.find(label("training.result",std::string(loc::T("training.meter.block"))+" -2"))!=std::string::npos,
                "Frame data did not read the focus cancel");
            // The next exchange is being measured: the kept reading keeps its cancel.
            cancel.advantage=training::FrameAdvantage{};cancel.advantage.pending=true;cancel.advantage.attacker=0;
            Require(TrainingFrameData(cancel,0).find(label("training.last_focus_forward",std::string(loc::T("training.meter.block"))+" -2"))!=std::string::npos,
                "Frame data's last reading lost its cancel");
        }
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
    SetMenuGlyphs(0,0,0);draw("training-hud-keyboard");
    // While a slot plays F8 stops it, and the strip says so.
    training.mode=training::Mode::Playback;training.active=0;training.activeLength=120;training.cursor=40;draw("training-hud-playing");
    training.mode=training::Mode::Idle;training.cursor=0;training.active=-1;training.activeLength=0;
    // A failed strip check names what the strip showed.
    const auto strip=[](const char* what){
        std::string told=std::string(what)+" (" +loc::Tag(loc::Active())+", "+std::to_string(LastTrainingStrip().textPixels)+" px):";
        for(const auto& piece:LastTrainingStrip().pieces)told+=" ["+piece+"]";
        return told;
    };
    // The strip says what each key does now: F7 and F8 read Stop while they
    // record and play, the position keys are the player's and one set to Off
    // is left out, and a pad keeps its own buttons with F7 and F8. Drawn in
    // a 1080p window at the standard size, where every translation's line
    // has room for all but Hide HUD, which is last and may be left out.
    {
        // English shows the whole line; a translation may leave out the
        // hints after the first `least` pieces.
        const bool english=std::string(loc::T("training.record"))=="Record";
        const auto names=[&](std::vector<std::string> expected,std::size_t least,const char* what){
            const auto& shown=LastTrainingStrip().pieces;
            const bool prefix=shown.size()<=expected.size()&&std::equal(shown.begin(),shown.end(),expected.begin());
            Require(prefix&&shown.size()>=(english?expected.size():least),strip(what).c_str());
            Require(LastTrainingStrip().width<=LastTrainingStrip().room+.5f,"The training strip is wider than the meter");
        };
        const std::string chip=loc::T("training.controls_chip"),record=loc::T("training.record"),play=loc::T("training.play"),stop=loc::T("training.stop"),
            reset=loc::T("training.hint.reset"),save=loc::T("training.hint.save"),hide=loc::T("training.hide_hint");
        io.DisplaySize=ImVec2(1920,1080);SetTrainingMeterOptions({});
        SetTrainingPositionKeys(1,10);draw();
        names({"F6",chip,"F7",record,"F8",play,"F2",reset,"F11",save,"F5",hide},8,"The strip did not name the keyboard's keys at rest");
        // On standby F7 starts the recording; recording, it stops it.
        training.mode=training::Mode::Standby;draw();
        names({"F6",chip,"F7",record,"F8",play,"F2",reset,"F11",save,"F5",hide},8,"F7 did not read Record on standby");
        training.mode=training::Mode::Recording;draw();
        names({"F6",chip,"F7",stop,"F8",play,"F2",reset,"F11",save,"F5",hide},8,"F7 did not read Stop while recording");
        training.mode=training::Mode::Playback;draw();
        names({"F6",chip,"F7",record,"F8",stop,"F2",reset,"F11",save,"F5",hide},8,"F8 did not read Stop while playing");
        training.mode=training::Mode::Idle;
        SetTrainingPositionKeys(3,-1);draw();
        names({"F6",chip,"F7",record,"F8",play,"F4",reset,"F5",hide},8,"The strip did not follow a picked reset key and an Off save key");
        SetTrainingPositionKeys(-1,8);draw();
        names({"F6",chip,"F7",record,"F8",play,"F9",save,"F5",hide},8,"The strip did not follow an Off reset key and a picked save key");
        SetTrainingPositionKeys(1,10);
        for(const int device:{input::PadXInput,input::PadDirectInput}) {
            SetMenuGlyphs(device,device==input::PadXInput?input::xinput::A:0,device==input::PadXInput?input::xinput::B:0);
            NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);
            const std::string back=device==input::PadXInput?"Back":"Select";
            training.mode=training::Mode::Recording;draw();
            names({back,"+","Start",chip,back,loc::T("training.pad_hint"),"F7",stop,"F8",play},8,"The pad's strip lost its buttons or F7");
            training.mode=training::Mode::Playback;draw();
            names({back,"+","Start",chip,back,loc::T("training.pad_hint"),"F7",record,"F8",stop},8,"The pad's strip lost its buttons or F7");
            training.mode=training::Mode::Idle;
        }
        io.AddKeyEvent(ImGuiKey_F5,true);draw(nullptr,0,1);NoteMenuDevice(0);io.AddKeyEvent(ImGuiKey_F5,false);draw(nullptr,0,1);
        SetMenuGlyphs(0,0,0);
        io.DisplaySize=ImVec2(static_cast<float>(size.w),static_cast<float>(size.h));draw();
    }
    // The strip's text is never under 15 pixels, and its whole line stays
    // within the meter, in the smallest window at the smallest size with
    // the theme at 300%, for the keyboard and both pads. In Latin American
    // Spanish too, whose chip and pad hint are among the longest.
    if(size.w==1280&&size.dpi==1) {
        const bool english=std::string(loc::T("training.record"))=="Record";
        for(int pass=0;pass<(english?2:1);++pass) {
            if(pass==1)loc::SetActive(loc::Locale::Es419);
            retheme(3.f);
            training::MeterOptions smallest;smallest.size=0;SetTrainingMeterOptions(smallest);
            io.DisplaySize=ImVec2(640,480);
            for(const int device:{input::PadKeyboard,input::PadXInput,input::PadDirectInput}) {
                SetMenuGlyphs(device,device==input::PadXInput?input::xinput::A:0,device==input::PadXInput?input::xinput::B:0);
                if(device!=input::PadKeyboard){NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);}
                else{io.AddKeyEvent(ImGuiKey_F5,true);draw(nullptr,0,1);NoteMenuDevice(0);io.AddKeyEvent(ImGuiKey_F5,false);draw(nullptr,0,1);}
                draw();
                const auto* meter=FindWindow("Training frame meter");const auto* shortcuts=FindWindow("Training shortcuts");
                const auto& shown=LastTrainingStrip();
                Require(!shown.pieces.empty()&&std::fabs(shown.textPixels-15)<.05f,strip("The training strip's text is not held at 15 pixels").c_str());
                Require(shown.width<=shown.room+.5f&&shortcuts->Pos.x>=meter->Pos.x-1&&shortcuts->Pos.x+shortcuts->Size.x<=meter->Pos.x+meter->Size.x+1,
                    strip("The training strip runs past the meter in a small window at 300%").c_str());
            }
            if(pass==1)loc::SetActive(loc::Locale::En);
        }
        SetMenuGlyphs(0,0,0);SetTrainingMeterOptions({});retheme(size.dpi);
        io.DisplaySize=ImVec2(static_cast<float>(size.w),static_cast<float>(size.h));draw();
    }
    // With the shortcuts off, a position key's notice is all the strip shows.
    {
        training::MeterOptions quiet;quiet.shortcuts=false;SetTrainingMeterOptions(quiet);
        draw();Require(LastTrainingStrip().pieces.empty(),strip("The strip drew with the shortcuts off and nothing to tell").c_str());
        input::TrainingPadEvent press;press.kind=input::TrainingPadEvent::Kind::Reset;press.generation=training.generation;
        TrainingPadPosition(training,[](training::Command){return false;},press);
        draw();
        // Whole, or shortened to the meter's width.
        const auto& told=LastTrainingStrip().pieces;const std::string rejected=loc::T("training.command_rejected");
        const bool shortened=told.size()==1&&told[0].size()>3&&told[0].compare(told[0].size()-3,3,"...")==0&&rejected.compare(0,told[0].size()-3,told[0],0,told[0].size()-3)==0;
        Require(told.size()==1&&(told[0]==rejected||shortened),strip("The notice did not stand alone with the shortcuts off").c_str());
        // The notice lasts three seconds.
        draw(nullptr,0,200);
        Require(LastTrainingStrip().pieces.empty(),strip("The notice outlived its three seconds").c_str());
        // A position command the game takes clears the notice, so the next
        // size's new clock does not bring it back.
        TrainingPadPosition(training,[](training::Command){return true;},press);
        SetTrainingMeterOptions({});draw();
    }
    SetMenuGlyphs(3,0x40000,0x20000);
}
}
