#pragma once
// The match HUD of ui_render_test.cxx, which includes this after its Require: the
// per-frame geometry checks, the shots the capture filter keeps, and the scenarios of
// the Ember strip and the split layout (name plates over the game's PLAYER labels). `draw`
// is the test's own and `reset` remakes the device objects. The main function keeps the
// renderer, the viewport and locale loops, and sets mode 3 before calling the group.
#include "../ui/Theme.hxx"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
namespace {
// Half the Small panel's usual scale on a screen h tall: the least a squeezed panel keeps.
float MatchHudMinimumPanelScale(float h){return .5f*(std::max)(.8f,h/1080.f)*.85f;}
bool MatchBoxesOverlap(const sf4e::ui::MatchStripBox& a,const sf4e::ui::MatchStripBox& b){return a.x0<b.x1&&b.x0<a.x1&&a.y0<b.y1&&b.y0<a.y1;}
// One drawn frame of the HUD, in a window of w by h: it never takes input, the Ember
// strip stays inside the safe band and on its anchored half, and the split layout keeps
// its name labels inside the 16:9 game image where the game's own HUD leaves room.
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
// Whether the capture filter (--match-shots-only) keeps this shot at this window size and scale.
bool MatchHudShotCaptured(const char* shot,int w,int h,float dpi){
    const std::string name=shot?shot:"";
    return (name=="match-hud"||name=="match-hud-split")&&((w==1280&&h==720&&dpi==1)||
            (w==1920&&h==1080&&dpi==1)||(w==2560&&h==1440)||(w==3440&&h==1440)||(w==3840&&h==2160&&dpi==1))||
        (w==1920&&h==1080&&dpi==1&&
            (name=="match-hud-size-0"||name=="match-hud-size-2"||name=="match-hud-raised"||name=="match-hud-long"||
             name=="match-hud-unavailable"||name=="match-hud-spectator"||name=="match-hud-reset"||
             name.find("match-hud-anchor-")==0||name.find("match-hud-split-")==0));
}
// Small, Standard and Large must stay three different sizes at this window size.
void CheckMatchHudScales(){
    sf4e::ui::MatchStripView strip;strip.names[0]="P1";strip.names[1]="P2";
    // 'small' is a windows.h macro; never name a local that.
    strip.size=0;const float sizeSmall=sf4e::ui::MatchStripScale(strip);
    strip.size=1;const float sizeStandard=sf4e::ui::MatchStripScale(strip);
    strip.size=2;const float sizeLarge=sf4e::ui::MatchStripScale(strip);
    Require(sizeSmall<sizeStandard&&sizeStandard<sizeLarge,
        "Match HUD size settings collapse at this viewport; two of the three choices do nothing");
}
// The HUD shots, from the Ember strip through the split layout above the life
// bars. Every drawn frame is checked by CheckMatchHudFrame, so the loops over anchors, sizes
// and spacings also exercise the geometry rules.
template<class Draw,class Reset>
void ShootMatchHud(sf4e::ui::MatchStripView& matchStrip,const Draw& draw,const Reset& reset){
    using namespace sf4e;using namespace sf4e::ui;
    {
        // The strip's link-state line: the most urgent condition wins,
        // a stall names itself, a warning carries the drop countdown.
        MatchStripView strip;strip.names[0]="P1";strip.names[1]="P2";
        Require(MatchStripStateLine(strip).empty(),"Match HUD shows a state line with nothing to say");
        strip.notice="Connection restored.";strip.noticeSeverity=0;
        Require(MatchStripStateLine(strip)=="Connection restored.","Info notice not shown on the match HUD");
        strip.connectionWarning=true;strip.disconnectCountdownMs=2100;
        Require(MatchStripStateLine(strip)==loc::Tf("match.connection_countdown",3),"Connection warning countdown missing or wrong rounding");
        strip.disconnectCountdownMs=-1;
        Require(MatchStripStateLine(strip)==loc::T("match.connection_unstable"),"Connection warning without a countdown");
        strip.predictionStalled=true;
        Require(MatchStripStateLine(strip)==loc::T("match.waiting_opponent"),"A prediction stall must be named on the match HUD");
        strip.notice="Opponent disconnected. The match is over.";strip.noticeSeverity=2;
        Require(MatchStripStateLine(strip)=="Opponent disconnected. The match is over.","An error notice must outrank the stall and warning lines");
        strip.pingMs=68;strip.rollbackFrames=7;strip.appliedDelay=2;
        matchStrip=strip;draw("match-hud-disconnected");
        matchStrip.notice.clear();matchStrip.noticeSeverity=0;matchStrip.predictionStalled=false;
        matchStrip.connectionWarning=true;matchStrip.disconnectCountdownMs=1400;draw("match-hud-warning");
        matchStrip.connectionWarning=false;matchStrip.disconnectCountdownMs=-1;matchStrip.predictionStalled=true;draw("match-hud-stalled");
        matchStrip.predictionStalled=false;
    }
    // Each anchor stays inside the safe bounds and on its own edge (checked per frame).
    for(int anchor=1;anchor<5;++anchor){
        matchStrip.anchor=anchor;
        const auto shot="match-hud-anchor-"+std::to_string(anchor);draw(shot.c_str());
    }
    // A top anchor draws the state line below the panel so it stays on screen; raised moves in from the top.
    matchStrip.anchor=3;matchStrip.connectionWarning=true;matchStrip.disconnectCountdownMs=1400;draw("match-hud-anchor-top-warning");
    matchStrip.anchor=4;matchStrip.raised=true;matchStrip.connectionWarning=false;matchStrip.disconnectCountdownMs=-1;
    matchStrip.notice="Opponent disconnected. The match is over.";matchStrip.noticeSeverity=2;draw("match-hud-anchor-top-notice");
    matchStrip.anchor=2;draw("match-hud-anchor-bottom-notice");
    matchStrip.anchor=0;matchStrip.raised=false;matchStrip.notice.clear();matchStrip.noticeSeverity=0;
    for(int hudSize=0;hudSize<3;++hudSize){
        matchStrip.size=hudSize;
        const auto shot="match-hud-size-"+std::to_string(hudSize);draw(shot.c_str());
    }
    matchStrip.raised=true;draw("match-hud-raised");
    matchStrip.names[0]="Long player name with UTF-8 \xc3\xa9\xc3\xa9\xc3\xa9";matchStrip.names[1]="Another very long player name";
    matchStrip.pingMs=9999;matchStrip.rollbackFrames=999;matchStrip.appliedDelay=10;draw("match-hud-long");
    matchStrip.pingMs=-1;matchStrip.appliedDelay=-1;draw("match-hud-unavailable");
    matchStrip.spectator=true;draw("match-hud-spectator");
    matchStrip.score="12 - 10";draw("match-hud-score");
    reset();draw("match-hud-reset");
    // The split layout: names above the life bars, a small telemetry panel by anchor.
    MatchStripView split;split.names[0]="Player One";split.names[1]="Player Two";
    split.links[0]=NetworkLink::Wired;split.links[1]=NetworkLink::Wireless;
    split.pingMs=68;split.rollbackFrames=2;split.appliedDelay=3;split.layout=1;
    matchStrip=split;draw("match-hud-split");
    for(int anchor=1;anchor<5;++anchor){
        matchStrip.anchor=anchor;
        const auto shot="match-hud-split-anchor-"+std::to_string(anchor);draw(shot.c_str());
    }
    matchStrip.anchor=3;matchStrip.connectionWarning=true;matchStrip.disconnectCountdownMs=1400;draw("match-hud-split-anchor-top-warning");
    matchStrip.anchor=4;matchStrip.raised=true;matchStrip.connectionWarning=false;matchStrip.disconnectCountdownMs=-1;
    matchStrip.notice="Opponent disconnected. The match is over.";matchStrip.noticeSeverity=2;draw("match-hud-split-anchor-top-notice");
    matchStrip.anchor=0;matchStrip.raised=false;draw("match-hud-split-notice");
    // A top anchor keeps the panel and its state line above the plates at every size and spacing,
    // with a warning and with a notice: drawn here (CheckMatchHudFrame rejects an overlap in each
    // frame), and laid out on short screens the harness does not render, where the panel shrinks
    // to fit above the plates and the three sizes stay distinct.
    const std::string hardNotice=matchStrip.notice;
    const ImVec2 shortScreens[]={ImVec2(960,540),ImVec2(854,480),ImVec2(640,360)};
    for(const bool warning:{true,false})for(const int anchor:{3,4})for(const bool raised:{false,true}){
        matchStrip.connectionWarning=warning;matchStrip.disconnectCountdownMs=warning?1400:-1;
        matchStrip.notice=warning?"":hardNotice;matchStrip.noticeSeverity=warning?0:2;
        Require(MatchStripStateLine(matchStrip)==(warning?sf4e::loc::Tf("match.connection_countdown",2):hardNotice),"Top-anchor sweep shows the wrong state line");
        matchStrip.anchor=anchor;matchStrip.raised=raised;
        for(int hudSize=0;hudSize<3;++hudSize){matchStrip.size=hudSize;draw(nullptr,0,1);}
        for(const auto& screen:shortScreens){
            float width[3]={0,0,0};
            for(int hudSize=0;hudSize<3;++hudSize){
                matchStrip.size=hudSize;
                const auto bounds=MatchStripGeometry(matchStrip,ImVec2(0,0),screen);
                for(const auto& name:bounds.names)Require(!MatchBoxesOverlap(bounds.panel,name),"Match HUD telemetry covers a name plate on a short screen");
                Require(bounds.panel.y0>=0,"Match HUD telemetry left the top of a short screen");
                width[hudSize]=bounds.panel.x1-bounds.panel.x0;
            }
            Require(width[0]<width[1]&&width[1]<width[2],"Match HUD sizes collapse on a short screen");
        }
    }
    matchStrip.connectionWarning=false;matchStrip.disconnectCountdownMs=-1;matchStrip.anchor=0;matchStrip.size=0;matchStrip.raised=false;
    matchStrip.notice.clear();matchStrip.noticeSeverity=0;
    float plateHeight[3]={0,0,0},panelWidth[3]={0,0,0};
    for(int hudSize=0;hudSize<3;++hudSize){
        matchStrip.size=hudSize;
        const auto shot="match-hud-split-size-"+std::to_string(hudSize);draw(shot.c_str());
        const auto bounds=MatchStripGeometry(matchStrip);
        plateHeight[hudSize]=bounds.names[0].y1-bounds.names[0].y0;panelWidth[hudSize]=bounds.panel.x1-bounds.panel.x0;
    }
    // The names keep the PLAYER label's size; the setting sizes the panel.
    Require(plateHeight[0]==plateHeight[1]&&plateHeight[1]==plateHeight[2]&&panelWidth[0]<panelWidth[1]&&panelWidth[1]<panelWidth[2],
        "Split match HUD size settings collapse, or they change the names");
    // Long names and a watching count: the plates truncate and never meet, the panel stays in its band.
    matchStrip.names[0]="Long player name with UTF-8 \xc3\xa9\xc3\xa9\xc3\xa9 and more";matchStrip.names[1]="Another very long player name that keeps going";
    matchStrip.score="12 - 10";matchStrip.spectators=3;matchStrip.size=1;
    matchStrip.pingMs=9999;matchStrip.rollbackFrames=999;matchStrip.appliedDelay=10;draw("match-hud-split-long");
    matchStrip.spectator=true;matchStrip.size=1;draw("match-hud-split-spectator");
    // The name offset moves both plates by that many game units and nothing else; a top-anchored
    // panel stays above plates moved all the way up, on short screens too.
    matchStrip.spectator=false;matchStrip.score.clear();matchStrip.spectators=0;
    const auto unmoved=MatchStripGeometry(matchStrip);
    const float shiftScale=(std::min)(ImGui::GetMainViewport()->Size.y/720.f,ImGui::GetMainViewport()->Size.x/1280.f);
    for(const int offset:{-60,-20,60}){
        matchStrip.nameOffset=offset;
        for(const int anchor:{0,3,4}){
            matchStrip.anchor=anchor;draw(anchor==0&&offset==-20?"match-hud-split-name-up":nullptr,0,1);
            const auto moved=MatchStripGeometry(matchStrip);
            // All the way up leaves no readable room above the names; part way up still does.
            Require(moved.panelBelow==(anchor>=3&&offset==-60),"Match HUD placed a top panel on the wrong edge for the name offset");
            for(int side=0;side<2;++side)
                Require(std::abs(moved.names[side].y0-unmoved.names[side].y0-offset*shiftScale)<=1&&
                    moved.names[side].x0==unmoved.names[side].x0&&moved.names[side].x1==unmoved.names[side].x1&&
                    moved.names[side].y1-moved.names[side].y0==unmoved.names[side].y1-unmoved.names[side].y0,"Match HUD name offset does not move the plates by itself");
            for(const auto& screen:shortScreens){
                const auto bounds=MatchStripGeometry(matchStrip,ImVec2(0,0),screen);
                for(const auto& name:bounds.names)Require(!MatchBoxesOverlap(bounds.panel,name),"Match HUD telemetry covers a moved name plate on a short screen");
                Require(bounds.panel.y0>=0&&bounds.panel.y1<=screen.y,"Match HUD telemetry left a short screen around moved name plates");
                Require(bounds.panelScale>=MatchHudMinimumPanelScale(screen.y)-.001f,"Match HUD telemetry unreadable above moved name plates on a short screen");
            }
        }
    }
    matchStrip.nameOffset=0;matchStrip.anchor=0;
    matchStrip.layout=0;matchStrip.spectator=false;matchStrip.size=0;
}
}
