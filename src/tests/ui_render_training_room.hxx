#pragma once
// Waiting in Training from a room: the room line over the battle and the
// call's banner with go now, through the production OverlayLayers entry point.
#include "ui_render_support.hxx"
#include "../common/Localization.hxx"
#include "../ui/GameMenu.hxx"
#include "../ui/OverlayLayers.hxx"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <string>
#include <utility>
namespace {
template<class Size,class Draw>
void ShootTrainingRoom(sf4e::training::View& training,sf4e::ui::OverlayLayersView& layers,int& mode,const Size& size,const Draw& draw) {
    using namespace sf4e;using namespace ui;
    const auto saved=training;
    room::Snapshot s;s.roomEpoch=7;s.name="Friday Night Fights";s.localMember=1;
    for(std::size_t i=0;i<s.tables.size();++i)s.tables[i].id=static_cast<std::uint8_t>(i);
    room::Member me,alex,sam;me.id=1;me.name="Ember Player";alex.id=2;alex.name="Alex";alex.fighter=0;sam.id=3;sam.name="Sam";
    s.members={me,alex,sam};
    mode=7;layers={};layers.trainingHud=true;
    training.available=true;training.watching=false;training.leavingIn=0;
    // Second in table 2's queue, with chat waiting.
    s.tables[1].queue={3,1};
    layers.trainingRoom=DescribeTrainingRoom(s,3);draw("training-room-status");
    Require(!ImGui::GetIO().WantCaptureKeyboard&&!ImGui::GetIO().WantCaptureMouse,"The room line captured input");
    const auto clear=[&]{
        const auto* status=FindWindow("Training room status");const auto* meter=FindWindow("Training frame meter");
        const auto* chips=FindWindow("Training shortcuts");
        Require(WindowDrawn("Training room status")&&WindowDrawn("Training frame meter")&&WindowDrawn("Training shortcuts"),"The room line or the training HUD was not drawn");
        // Under the game's timer and name logos, above the training chips
        // and meter, inside the screen.
        Require(status->Pos.y>=size.h*TrainingRoomStatusTop-1&&status->Pos.y+status->Size.y<=chips->Pos.y&&chips->Pos.y<meter->Pos.y,
            "The room line is not between the game's top gauges and the training chips");
        Require(status->Pos.x>=0&&status->Pos.x+status->Size.x<=size.w,"The room line left the screen");
    };
    clear();
    // Seated, nobody opposite yet.
    s.tables[1].queue.clear();s.tables[1].p1=1;s.members[0].table=1;s.members[0].seat=0;
    layers.trainingRoom=DescribeTrainingRoom(s,0);draw("training-room-status-waiting");clear();
    // A long room name gives way to Ember's own words.
    s.name=std::string(64,'W');layers.trainingRoom=DescribeTrainingRoom(s,12);draw("training-room-status-long");clear();
    // Both players' names at their longest (a room name of 64, a display name
    // of 31): they give way so the line stays within two fifths of the screen,
    // clear of the gauges at the sides. Where Ember's own words already fill
    // that (a narrow screen, a long translation), the long names take no more
    // room than one-letter names would with their "...". Each keeps some of
    // itself, and the line stays on screen.
    {
        const auto savedMembers=s.members;const auto savedTable=s.tables[1];const auto savedName=s.name;
        s.tables[1].queue.clear();s.tables[1].p1=1;s.tables[1].p2=2;s.members[0].table=s.members[1].table=1;s.members[0].seat=0;s.members[1].seat=1;
        s.name="W";s.members[1].name="W";
        layers.trainingRoom=DescribeTrainingRoom(s,12);draw();
        const float shortest=FindWindow("Training room status")->Size.x;
        s.name=std::string(64,'W');s.members[1].name=std::string(31,'W');
        layers.trainingRoom=DescribeTrainingRoom(s,12);
        Require(layers.trainingRoom.opponent==s.members[1].name,"The longest opponent name was not kept apart");
        draw("training-room-status-longest");clear();
        const auto* status=FindWindow("Training room status");
        const float ellipses=2*ImGui::CalcTextSize("...").x*status->FontWindowScale;
        Require(status->Size.x<=(std::max)(size.w*.4f+2*status->WindowPadding.x,shortest+ellipses)+1,"Two long names pushed the room line past two fifths of the screen");
        s.members=savedMembers;s.tables[1]=savedTable;s.name=savedName;
    }
    s.name="Friday Night Fights";
    // Alex sits down: the call's banner names them and their fighter, and
    // offers go now with the keyboard's key, then the pad's.
    s.tables[1].p2=2;s.members[1].table=1;s.members[1].seat=1;
    input::CallIdentity call;call.roomEpoch=s.roomEpoch;call.table=1;call.opponent=2;call.generation=training.generation;
    layers.trainingRoom=DescribeTrainingRoom(s,0);layers.challenger=DescribeChallenger(s,call);
    Require(layers.challenger.called&&layers.challenger.opponent=="Alex","The banner fixture has no opponent");
    training.leavingIn=72;
    layers.challenger.goNowGlyph="Enter";draw("training-call-go-now-keyboard");
    Require(ImGui::GetForegroundDrawList()->VtxBuffer.Size>0,"The challenger banner was not drawn");
    layers.challenger.goNowGlyph="View";draw("training-call-go-now-pad");
    Require(!ImGui::GetIO().WantCaptureKeyboard&&!ImGui::GetIO().WantCaptureMouse,"The challenger banner captured input");
    // Go now is not offered under an open window: the banner alone.
    layers.challenger.goNowGlyph=nullptr;layers.shellVisible=true;draw();
    Require(!WindowDrawn("Training room status"),"The room line showed under an open window");
    // The whole training HUD of a member called back from a room, at every
    // meter style, frames shown and Recovery setting, with keyboard and pad
    // prompts: the room line, the meter and the call's banner never meet,
    // and each stays on screen, the meter above the super gauges. The chips
    // that open the controls and name the pad's training buttons are not
    // drawn under the call, when neither applies.
    {
        layers.shellVisible=false;
        const auto apart=[](ImVec2 aMin,ImVec2 aMax,ImVec2 bMin,ImVec2 bMax){
            return aMax.x<=bMin.x+.5f||bMax.x<=aMin.x+.5f||aMax.y<=bMin.y+.5f||bMax.y<=aMin.y+.5f;};
        const auto box=[](const ImGuiWindow* w){return std::make_pair(w->Pos,ImVec2(w->Pos.x+w->Size.x,w->Pos.y+w->Size.y));};
        const auto onScreen=[&](ImVec2 min,ImVec2 max){return min.x>=-.5f&&min.y>=-.5f&&max.x<=size.w+.5f&&max.y<=size.h+.5f;};
        const auto savedOptions=TrainingMeterOptions();
        const bool keyboardBefore=MenuPromptDevice()==input::PadKeyboard;
        for(const bool pad:{false,true}){
            // The prompts as the overlay picks them: the keyboard, then an
            // Xbox pad pressed last, whose go now says View.
            if(pad){SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B);NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);}
            else SetMenuGlyphs(input::PadKeyboard,0,0);
            Require(MenuPromptDevice()==(pad?input::PadXInput:input::PadKeyboard),"The HUD's prompts did not follow the device");
            layers.challenger.goNowGlyph=GoNowGlyph(MenuPromptDevice());
            for(const int shown:training::MeterShownChoices)for(const bool flat:{false,true})for(const bool recovery:{false,true}){
                training::MeterOptions options;options.flat=flat;options.recovery=recovery;options.shown=shown;SetTrainingMeterOptions(options);
                const bool shot=shown==60&&!recovery;
                draw(shot?(pad?(flat?"training-call-hud-flat-pad":"training-call-hud-angled-pad"):(flat?"training-call-hud-flat":"training-call-hud-angled")):nullptr);
                Require(WindowDrawn("Training room status")&&WindowDrawn("Training frame meter"),"The called member's training HUD was not drawn whole");
                Require(!WindowDrawn("Training shortcuts"),"The training chips showed under the call");
                const auto status=box(FindWindow("Training room status")),meter=box(FindWindow("Training frame meter"));
                // The banner is the foreground layer's only drawing here.
                const auto& vertices=ImGui::GetForegroundDrawList()->VtxBuffer;
                Require(vertices.Size>0,"The call's banner was not drawn over the training HUD");
                ImVec2 bannerMin(FLT_MAX,FLT_MAX),bannerMax(-FLT_MAX,-FLT_MAX);
                for(const auto& vertex:vertices){
                    bannerMin.x=(std::min)(bannerMin.x,vertex.pos.x);bannerMin.y=(std::min)(bannerMin.y,vertex.pos.y);
                    bannerMax.x=(std::max)(bannerMax.x,vertex.pos.x);bannerMax.y=(std::max)(bannerMax.y,vertex.pos.y);
                }
                Require(status.first.y>=size.h*TrainingRoomStatusTop-1,"The room line rose into the game's timer and name logos");
                Require(apart(status.first,status.second,meter.first,meter.second),"The room line and the meter overlap");
                Require(apart(bannerMin,bannerMax,status.first,status.second)&&apart(bannerMin,bannerMax,meter.first,meter.second),
                    "The call's banner covers the room line or the meter");
                Require(onScreen(status.first,status.second)&&onScreen(meter.first,meter.second)&&onScreen(bannerMin,bannerMax),
                    "Part of the called member's training HUD left the screen");
                Require(meter.second.y<=size.h*TrainingHudBottom+1,"The meter covers the game's super meters");
            }
        }
        SetTrainingMeterOptions(savedOptions);SetMenuGlyphs(3,0x40000,0x20000);
        // Back to the keyboard prompts the fixture came with: a function key
        // pressed last, as the training fixture leaves them.
        if(keyboardBefore){
            auto& io=ImGui::GetIO();
            io.AddKeyEvent(ImGuiKey_F5,true);draw(nullptr,0,1);NoteMenuDevice(0);io.AddKeyEvent(ImGuiKey_F5,false);draw(nullptr,0,1);
            Require(MenuPromptDevice()==input::PadKeyboard,"The fixture's keyboard prompts were not restored");
        }
    }
    training=saved;layers={};mode=2;draw();
}
}
