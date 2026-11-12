#pragma once
// The camera panel (F12): shut until F12, open over the Training HUD, F12 closes it, and Training off shuts it.
// Over a replay its Video editor button brings the timeline, under the panel. All of it stays on screen, at both sizes.
#include "ui_render_support.hxx"
#include "../ui/CameraPanel.hxx"
#include <imgui.h>
namespace {
template<class Draw>
void ShootCameraPanel(bool& available,sf4e::ui::ReplayBar& bar,int& mode,const Draw& draw) {
    using namespace sf4e;using namespace ui;
    auto& io=ImGui::GetIO();
    const auto press=[&]{io.AddKeyEvent(ImGuiKey_F12,true);draw(nullptr,0,1);io.AddKeyEvent(ImGuiKey_F12,false);draw(nullptr,0,1);};
    mode=2;available=true;
    draw(nullptr,0,2);
    Require(!CameraPanelOpen()&&!WindowDrawn("##camerapanel"),"The camera panel showed before F12");
    press();draw("camera-panel",0,2);
    Require(CameraPanelOpen()&&WindowDrawn("##camerapanel"),"F12 did not open the camera panel");
    auto* panel=FindWindow("##camerapanel");const auto* vp=ImGui::GetMainViewport();
    Require(panel->Pos.x>=vp->Pos.x&&panel->Pos.y>=vp->Pos.y&&panel->Pos.x+panel->Size.x<=vp->Pos.x+vp->Size.x&&
            panel->Pos.y+panel->Size.y<=vp->Pos.y+vp->Size.y+1,"The camera panel left the screen");
    press();draw(nullptr,0,1);
    Require(!CameraPanelOpen()&&!WindowDrawn("##camerapanel"),"F12 did not close the camera panel");
    // Over a replay: the Video editor button is found by hovering down the panel's left half, then clicked.
    press();bar.on=true;draw(nullptr,0,2);
    panel=FindWindow("##camerapanel");
    const ImGuiID editor=panel->GetID("Video editor");
    bool found=false;
    for(float y=panel->Pos.y;y<panel->Pos.y+panel->Size.y&&!found;y+=6){
        io.AddMousePosEvent(panel->Pos.x+panel->Size.x/4,y);draw(nullptr,0,1);
        found=ImGui::GetHoveredID()==editor;
    }
    Require(found,"The camera panel has no Video editor button over a replay");
    io.AddMouseButtonEvent(0,true);draw(nullptr,0,1);io.AddMouseButtonEvent(0,false);draw(nullptr,0,1);
    io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);draw("camera-panel-editor",0,2);
    Require(CameraEditorShown()&&WindowDrawn("##cameratimeline"),"The Video editor button did not bring the timeline");
    panel=FindWindow("##camerapanel");const auto* line=FindWindow("##cameratimeline");
    Require(panel->Pos.y+panel->Size.y<=line->Pos.y+1&&line->Pos.y+line->Size.y<=vp->Pos.y+vp->Size.y+1,
            "The camera panel and its timeline do not fit under each other");
    press();bar={};
    // Training off shuts an open panel, and it stays shut when Training returns.
    press();available=false;draw(nullptr,0,1);
    Require(!CameraPanelOpen()&&!WindowDrawn("##camerapanel"),"The camera panel stayed with Training off");
    available=true;draw(nullptr,0,1);
    Require(!CameraPanelOpen(),"The camera panel came back on its own");
    available=false;
}
}
