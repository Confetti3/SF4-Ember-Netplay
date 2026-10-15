// The match HUD checks of ui_render_test.cxx that are not templates
// (ui_render_match_hud.hxx).
#include "ui_render_match_hud.hxx"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
float MatchHudMinimumPanelScale(float h){return .5f*(std::max)(.8f,h/1080.f)*.85f;}
bool MatchBoxesOverlap(const sf4e::ui::MatchStripBox& a,const sf4e::ui::MatchStripBox& b){return a.x0<b.x1&&b.x0<a.x1&&a.y0<b.y1&&b.y0<a.y1;}
void CheckMatchHudFrame(const sf4e::ui::MatchStripView& matchStrip,int w,int h){
    using namespace sf4e::ui;
    const auto& io=ImGui::GetIO();
    Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Match HUD captured gameplay input");
    const auto* list=ImGui::GetForegroundDrawList();
    const bool topEdge=matchStrip.anchor>=3;
    if(matchStrip.layout!=1){
        for(const auto& vertex:list->VtxBuffer){
            Require(vertex.pos.x>=w*.1f-2&&vertex.pos.x<=w*.9f+2&&
                vertex.pos.y>=0&&vertex.pos.y<=h,"Match HUD escaped safe viewport bounds");
            Require(topEdge?vertex.pos.y<=h*.5f:vertex.pos.y>=h*.5f,"Match HUD left its anchored edge");
        }
        return;
    }
    // Split layout: name plates are checked against the 16:9 game image,
    // the telemetry panel keeps the strip's safe band and anchored half.
    const auto bounds=MatchStripGeometry(matchStrip);
    Require(bounds.panel.valid&&bounds.names[0].valid&&bounds.names[1].valid,"Split match HUD reported no geometry");
    const float gs=(std::min)(h/720.f,w/1280.f),gameW=1280*gs,gx0=(w-gameW)*.5f,gy0=(h-720*gs)*.5f;
    const auto inside=[](const MatchStripBox& r,const ImVec2& p){return p.x>=r.x0-2&&p.x<=r.x1+2&&p.y>=r.y0-2&&p.y<=r.y1+2;};
    for(const auto& vertex:list->VtxBuffer)
        Require(inside(bounds.panel,vertex.pos)||inside(bounds.names[0],vertex.pos)||inside(bounds.names[1],vertex.pos),
            "Split match HUD drew outside its name plates and telemetry panel");
    for(const auto& name:bounds.names){
        Require(name.x0>=gx0-1&&name.x1<=gx0+gameW+1&&name.y0>=0&&name.y1<=h,"Match HUD name left the game image");
        Require(name.x1-name.x0<=.35f*gameW+1,"Match HUD name plate too wide for the game");
        // The game's label row above the life bars (top edge y 98): y 74..95, covering the label's y 76..93,
        // moved by the player's name offset for a game whose HUD position was changed.
        const float y0=gy0+(74+matchStrip.nameOffset)*gs;
        Require(std::abs(name.y0-y0)<=1&&std::abs(name.y1-(y0+21*gs))<=1,"Match HUD name plate does not fill y 74..95 of the game frame, moved by the name offset");
    }
    Require(bounds.names[0].x1<bounds.names[1].x0,"Match HUD names meet");
    // The telemetry panel and its state line, at any anchor, never sit on a name plate.
    for(const auto& name:bounds.names)Require(!MatchBoxesOverlap(bounds.panel,name),"Match HUD telemetry covers a name plate");
    // P1 from x 146 and P2 to x 1134, each covering its PLAYER label (x 151..248 and 1030..1130) and
    // stopping short of the "N WINS" streak text and the timer (to x 420 and from x 860).
    Require(std::abs(bounds.names[0].x0-(gx0+146*gs))<=1&&std::abs(bounds.names[1].x1-(gx0+1134*gs))<=1,
        "Match HUD names are not anchored over the PLAYER labels");
    Require(bounds.names[0].x1>=gx0+252*gs-1&&bounds.names[1].x0<=gx0+1028*gs+1,"Match HUD name plate leaves part of a PLAYER label showing");
    Require(bounds.names[0].x1<=gx0+420*gs+1&&bounds.names[1].x0>=gx0+860*gs-1,"Match HUD name plate reaches the streak text or the timer");
    Require(bounds.panel.x0>=w*.1f-2&&bounds.panel.x1<=w*.9f+2&&bounds.panel.y0>=0&&bounds.panel.y1<=h,
        "Match HUD telemetry escaped safe viewport bounds");
    // Names moved far up send a top-anchored panel to the bottom corner on its side.
    Require(!bounds.panelBelow||topEdge,"Match HUD moved a bottom-anchored panel");
    const bool top=topEdge&&!bounds.panelBelow;
    Require(top?bounds.panel.y1<=h*.5f:bounds.panel.y0>=h*.5f,"Match HUD telemetry left its anchored edge");
    Require(bounds.panelScale>=MatchHudMinimumPanelScale(h)-.001f,"Match HUD telemetry shrank below half its usual scale");
}
bool MatchHudShotCaptured(const char* shot,int w,int h,float dpi){
    const std::string name=shot?shot:"";
    return (name=="match-hud"||name=="match-hud-split")&&((w==1280&&h==720&&dpi==1)||
            (w==1920&&h==1080&&dpi==1)||(w==2560&&h==1440)||(w==3440&&h==1440)||(w==3840&&h==2160&&dpi==1))||
        (w==1920&&h==1080&&dpi==1&&
            (name=="match-hud-size-0"||name=="match-hud-size-2"||name=="match-hud-raised"||name=="match-hud-long"||
             name=="match-hud-unavailable"||name=="match-hud-spectator"||name=="match-hud-reset"||
             name.find("match-hud-anchor-")==0||name.find("match-hud-split-")==0));
}
void CheckMatchHudScales(){
    sf4e::ui::MatchStripView strip;strip.names[0]="P1";strip.names[1]="P2";
    // 'small' is a windows.h macro; never name a local that.
    strip.size=0;const float sizeSmall=sf4e::ui::MatchStripScale(strip);
    strip.size=1;const float sizeStandard=sf4e::ui::MatchStripScale(strip);
    strip.size=2;const float sizeLarge=sf4e::ui::MatchStripScale(strip);
    Require(sizeSmall<sizeStandard&&sizeStandard<sizeLarge,
        "Match HUD size settings collapse at this viewport; two of the three choices do nothing");
}
