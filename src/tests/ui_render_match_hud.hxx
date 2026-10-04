#pragma once
// The match HUD of ui_render_test.cxx, which includes this after its Require: the
// per-frame geometry checks, the shots the capture filter keeps, and the scenarios of
// the Ember strip and the split layout (names under and above the life bars). `draw`
// is the test's own and `reset` remakes the device objects. The main function keeps the
// renderer, the viewport and locale loops, and sets mode 3 before calling the group.
#include "../ui/Theme.hxx"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
namespace {
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
    const auto overlap=[](const MatchStripBox& a,const MatchStripBox& b){return a.x0<b.x1&&b.x0<a.x1&&a.y0<b.y1&&b.y0<a.y1;};
    for(const auto& vertex:list->VtxBuffer)
        Require(inside(bounds.panel,vertex.pos)||inside(bounds.names[0],vertex.pos)||inside(bounds.names[1],vertex.pos)||
            (bounds.setTag.valid&&inside(bounds.setTag,vertex.pos)),
            "Split match HUD drew outside its name plates and telemetry panel");
    for(const auto& name:bounds.names){
        Require(name.x0>=gx0-1&&name.x1<=gx0+gameW+1&&name.y0>=0&&name.y1<=h,"Match HUD name left the game image");
        Require(name.x1-name.x0<=(matchStrip.namesAbove?.37f:.35f)*gameW+1,"Match HUD name plate too wide for the game");
    }
    Require(bounds.names[0].x1<bounds.names[1].x0,"Match HUD names meet");
    if(matchStrip.namesAbove){
        // Banners fill the top strip above the life bars, clear of the portraits and the timer.
        Require(bounds.names[0].x0>=gx0+112*gs-1&&bounds.names[0].x1<=gx0+556*gs+1&&
            bounds.names[1].x0>=gx0+724*gs-1&&bounds.names[1].x1<=gx0+1168*gs+1,"Match HUD banners cover the portraits or the timer");
        for(const auto& name:bounds.names)Require(name.y1<=95*gs+gy0,"Match HUD banner reaches the life bars");
        Require(bounds.setTag.valid==(matchStrip.setFormat>0),"Set length tag shown without a set length, or missing with one");
        if(bounds.setTag.valid)Require(bounds.setTag.x0>=gx0+600*gs&&bounds.setTag.x1<=gx0+680*gs&&bounds.setTag.y0>=0&&bounds.setTag.y1<=gy0+24*gs,
            "Set length tag left the area above the K.O. sign");
        // The telemetry panel and its state line, at any anchor, never sit on a banner or the tag.
        for(const auto& name:bounds.names)Require(!overlap(bounds.panel,name),"Match HUD telemetry covers a name banner");
        Require(!bounds.setTag.valid||!overlap(bounds.panel,bounds.setTag),"Match HUD telemetry covers the set length tag");
    }else{
        Require(!bounds.setTag.valid,"Under-the-bars names reported a set tag");
        // Each plate stays in the gap between the game's character logo and its round markers.
        Require(std::abs(bounds.names[0].x0-(gx0+236*gs))<=1&&std::abs(bounds.names[1].x1-(gx0+1044*gs))<=1,
            "Match HUD names are not anchored under the life bars");
        Require(bounds.names[0].x1<=gx0+474*gs+1&&bounds.names[1].x0>=gx0+806*gs-1,"Match HUD name plate covers the round markers");
        for(const auto& name:bounds.names)
            Require(name.y0>=gy0+121*gs-1&&name.y1<=gy0+143*gs+1,"Match HUD name plate left y 121..143 of the game frame");
    }
    Require(bounds.panel.x0>=w*.1f-2&&bounds.panel.x1<=w*.9f+2&&bounds.panel.y0>=0&&bounds.panel.y1<=h,
        "Match HUD telemetry escaped safe viewport bounds");
    Require(topEdge?bounds.panel.y1<=h*.5f:bounds.panel.y0>=h*.5f,"Match HUD telemetry left its anchored edge");
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
// The HUD shots, from the Ember strip through the split layout under and above the life
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
    // The split layout: names under the life bars, a small telemetry panel by anchor.
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
    matchStrip.notice.clear();matchStrip.noticeSeverity=0;
    float plateHeight[3]={0,0,0},panelWidth[3]={0,0,0};
    for(int hudSize=0;hudSize<3;++hudSize){
        matchStrip.size=hudSize;
        const auto shot="match-hud-split-size-"+std::to_string(hudSize);draw(shot.c_str());
        const auto bounds=MatchStripGeometry(matchStrip);
        plateHeight[hudSize]=bounds.names[0].y1-bounds.names[0].y0;panelWidth[hudSize]=bounds.panel.x1-bounds.panel.x0;
    }
    Require(plateHeight[0]<plateHeight[1]&&plateHeight[1]<plateHeight[2]&&panelWidth[0]<panelWidth[1]&&panelWidth[1]<panelWidth[2],
        "Split match HUD size settings collapse; two of the three choices do nothing");
    // Long names and a watching count: the plates truncate and never meet, the panel stays in its band.
    matchStrip.names[0]="Long player name with UTF-8 \xc3\xa9\xc3\xa9\xc3\xa9 and more";matchStrip.names[1]="Another very long player name that keeps going";
    matchStrip.score="12 - 10";matchStrip.spectators=3;matchStrip.size=1;
    matchStrip.pingMs=9999;matchStrip.rollbackFrames=999;matchStrip.appliedDelay=10;draw("match-hud-split-long");
    matchStrip.spectator=true;matchStrip.size=1;draw("match-hud-split-spectator");
    // Names above the life bars: banners across the top strip with the games won in boxes.
    matchStrip=split;matchStrip.namesAbove=true;matchStrip.hasScores=true;matchStrip.scores[0]=2;matchStrip.scores[1]=1;
    draw("match-hud-split-above");
    matchStrip.setFormat=5;draw("match-hud-split-above-ft5");
    matchStrip.setFormat=0;matchStrip.hasScores=false;draw("match-hud-split-above-noscore");
    matchStrip.hasScores=true;
    matchStrip.names[0]="Long player name with UTF-8 \xc3\xa9\xc3\xa9\xc3\xa9 and more";matchStrip.names[1]="Another very long player name that keeps going";
    matchStrip.scores[0]=12;matchStrip.scores[1]=10;matchStrip.spectators=3;draw("match-hud-split-above-long");
    matchStrip.setFormat=3;matchStrip.size=2;draw("match-hud-split-above-long-large");
    float bannerHeight[3]={0,0,0};
    for(int hudSize=0;hudSize<3;++hudSize){
        matchStrip.size=hudSize;
        const auto shot="match-hud-split-above-size-"+std::to_string(hudSize);draw(shot.c_str());
        const auto bounds=MatchStripGeometry(matchStrip);bannerHeight[hudSize]=bounds.names[0].y1-bounds.names[0].y0;
    }
    Require(bannerHeight[0]<bannerHeight[1]&&bannerHeight[1]<bannerHeight[2],"Split match HUD banner sizes collapse");
    for(int anchor=1;anchor<5;++anchor){
        matchStrip.anchor=anchor;matchStrip.size=1;
        const auto shot="match-hud-split-above-anchor-"+std::to_string(anchor);draw(shot.c_str());
    }
    // A top anchor puts the telemetry panel and its state line below the banners and the tag, at
    // every size and spacing (CheckMatchHudFrame rejects an overlap in each of these frames too).
    matchStrip.connectionWarning=true;matchStrip.disconnectCountdownMs=1400;
    for(const int anchor:{3,4})for(int hudSize=0;hudSize<3;++hudSize)for(const bool raised:{false,true}){
        matchStrip.anchor=anchor;matchStrip.size=hudSize;matchStrip.raised=raised;draw(nullptr,0,1);
        const auto bounds=MatchStripGeometry(matchStrip);
        Require(bounds.panel.y0>=(std::max)(bounds.names[0].y1,bounds.names[1].y1)&&bounds.panel.y0>=bounds.setTag.y1,
            "A top-anchored telemetry panel starts above the bottom of the name banners");
    }
    matchStrip.connectionWarning=false;matchStrip.disconnectCountdownMs=-1;matchStrip.raised=false;
    matchStrip.anchor=0;matchStrip.notice="Opponent disconnected. The match is over.";matchStrip.noticeSeverity=2;draw("match-hud-split-above-notice");
    matchStrip.layout=0;matchStrip.namesAbove=false;matchStrip.notice.clear();matchStrip.noticeSeverity=0;
    matchStrip.spectator=false;matchStrip.size=0;
}
}
