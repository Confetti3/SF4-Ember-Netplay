// The match HUD checks of ui_render_test.cxx that are not templates
// (ui_render_match_hud.hxx).
#include "ui_render_match_hud.hxx"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
float MatchHudMinimumPanelScale(float h){return .5f*(std::max)(.8f,h/1080.f)*.85f;}
bool MatchBoxesOverlap(const sf4e::ui::MatchStripBox& a,const sf4e::ui::MatchStripBox& b){return a.x0<b.x1&&b.x0<a.x1&&a.y0<b.y1&&b.y0<a.y1;}
bool MatchHudTouchesBox(const sf4e::ui::MatchStripBounds& bounds,const sf4e::ui::MatchStripBox& box){
    return MatchBoxesOverlap(bounds.panel,box)||(bounds.state.valid&&MatchBoxesOverlap(bounds.state,box));
}
// Top center: centred, ending above the K.O. sign (y 38 of the 720p frame, moved by the name offset),
// its state line apart from it and below the timer and the round markers (from y 160).
static void CheckTopCenterBounds(const sf4e::ui::MatchStripView& view,const sf4e::ui::MatchStripBounds& bounds,float w,float h){
    const float gs=(std::min)(h/720.f,w/1280.f),gy0=(h-720*gs)*.5f;
    Require(std::abs((bounds.panel.x0+bounds.panel.x1)*.5f-w*.5f)<=1,"Match HUD at Top center is not centred");
    Require(bounds.panel.y0>=gy0&&bounds.panel.y1<=gy0+(38+view.nameOffset)*gs+.01f,"Match HUD at Top center reaches the K.O. sign");
    if(!bounds.state.valid)return;
    Require(bounds.stateApart&&std::abs((bounds.state.x0+bounds.state.x1)*.5f-w*.5f)<=1,"Match HUD state line at Top center is not apart and centred");
    Require(bounds.state.y0>=bounds.panel.y1&&bounds.state.y1<=h,"Match HUD state line at Top center is above the panel or off screen");
    Require(std::abs(bounds.state.y1-h)<=.01f||std::abs(bounds.state.y0-(gy0+(160+view.nameOffset)*gs))<=1,"Match HUD state line at Top center is not below the round markers");
}
void CheckMatchHudFrame(const sf4e::ui::MatchStripView& matchStrip,int w,int h){
    using namespace sf4e::ui;
    const auto& io=ImGui::GetIO();
    Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Match HUD captured gameplay input");
    const auto* list=ImGui::GetForegroundDrawList();
    const auto bounds=MatchStripGeometry(matchStrip);
    Require(bounds.panel.valid,"Match HUD reported no geometry");
    // Names moved far up (or a short band) send a top panel to the bottom, on its side at a corner.
    const bool topEdge=matchStrip.anchor>=3;
    Require(!bounds.panelBelow||topEdge,"Match HUD moved a bottom-anchored panel");
    const bool top=topEdge&&!bounds.panelBelow;
    if(top&&matchStrip.anchor==sf4e::netplay::MaxMatchHudAnchor)CheckTopCenterBounds(matchStrip,bounds,static_cast<float>(w),static_cast<float>(h));
    if(matchStrip.layout!=1){
        for(const auto& vertex:list->VtxBuffer){
            Require(vertex.pos.x>=w*.1f-2&&vertex.pos.x<=w*.9f+2&&
                vertex.pos.y>=0&&vertex.pos.y<=h,"Match HUD escaped safe viewport bounds");
            Require(top?vertex.pos.y<=h*.5f:vertex.pos.y>=h*.5f,"Match HUD left its anchored edge");
        }
        return;
    }
    // Split layout: name plates are checked against the 16:9 game image,
    // the telemetry panel keeps the strip's safe band and anchored half.
    Require(bounds.names[0].valid&&bounds.names[1].valid,"Split match HUD reported no name plates");
    const float gs=(std::min)(h/720.f,w/1280.f),gameW=1280*gs,gx0=(w-gameW)*.5f,gy0=(h-720*gs)*.5f;
    const auto inside=[](const MatchStripBox& r,const ImVec2& p){return r.valid&&p.x>=r.x0-2&&p.x<=r.x1+2&&p.y>=r.y0-2&&p.y<=r.y1+2;};
    for(const auto& vertex:list->VtxBuffer)
        Require(inside(bounds.panel,vertex.pos)||inside(bounds.state,vertex.pos)||inside(bounds.names[0],vertex.pos)||inside(bounds.names[1],vertex.pos),
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
    for(const auto& name:bounds.names)Require(!MatchHudTouchesBox(bounds,name),"Match HUD telemetry covers a name plate");
    // P1 from x 146 and P2 to x 1134, each covering its PLAYER label (x 151..248 and 1030..1130) and
    // stopping short of the "N WINS" streak text and the timer (to x 420 and from x 860).
    Require(std::abs(bounds.names[0].x0-(gx0+146*gs))<=1&&std::abs(bounds.names[1].x1-(gx0+1134*gs))<=1,
        "Match HUD names are not anchored over the PLAYER labels");
    Require(bounds.names[0].x1>=gx0+252*gs-1&&bounds.names[1].x0<=gx0+1028*gs+1,"Match HUD name plate leaves part of a PLAYER label showing");
    Require(bounds.names[0].x1<=gx0+420*gs+1&&bounds.names[1].x0>=gx0+860*gs-1,"Match HUD name plate reaches the streak text or the timer");
    Require(bounds.panel.x0>=w*.1f-2&&bounds.panel.x1<=w*.9f+2&&bounds.panel.y0>=0&&bounds.panel.y1<=h,
        "Match HUD telemetry escaped safe viewport bounds");
    Require(top?bounds.panel.y1<=h*.5f:bounds.panel.y0>=h*.5f,"Match HUD telemetry left its anchored edge");
    Require(bounds.panelScale>=MatchHudMinimumPanelScale(h)-.001f,"Match HUD telemetry shrank below half its usual scale");
}
void CheckMatchHudTopCenter(){
    using namespace sf4e::ui;
    // 720p units: the band over the K.O. sign is y 0..38 less a gap of 3; a panel is 34 (split) or 62 (strip) tall at Standard.
    const float windows[][2]={{1280,720},{1920,1080},{1024,768},{2560,1440}};
    const auto fitOf=[](const MatchStripView& view,float w,float h){
        const float gs=(std::min)(h/720.f,w/1280.f);
        return (38+view.nameOffset-3)*gs/((view.layout==1?34.f:62.f)*1.25f);
    };
    const auto geometry=[](const MatchStripView& view,const float (&window)[2]){return MatchStripGeometry(view,ImVec2(0,0),ImVec2(window[0],window[1]));};
    for(const int layout:{0,1})for(const auto& window:windows)for(const int offset:{0,20}){
        MatchStripView view;view.names[0]="Player One";view.names[1]="Player Two";view.pingMs=68;view.rollbackFrames=2;view.appliedDelay=3;
        view.layout=layout;view.anchor=sf4e::netplay::MaxMatchHudAnchor;view.nameOffset=offset;view.connectionWarning=true;view.disconnectCountdownMs=1400;
        float width[3]={0,0,0};
        for(int size=0;size<3;++size){
            view.size=size;
            const auto bounds=geometry(view,window);
            const float usual=(std::max)(.8f,window[1]/1080.f),fit=fitOf(view,window[0],window[1]);
            Require(bounds.panelBelow==(fit<.5f*usual),"Match HUD at Top center fell back at the wrong size");
            Require(bounds.panel.valid&&bounds.state.valid,"Match HUD at Top center reported no geometry");
            // The spacing option does not move it (it does move the Bottom center it falls back to).
            view.raised=true;const auto raised=geometry(view,window);view.raised=false;
            Require(bounds.panelBelow||(raised.panel.x0==bounds.panel.x0&&raised.panel.y0==bounds.panel.y0),"Match HUD spacing moved the Top center panel");
            if(bounds.panelBelow){
                // Bottom center: on the lower half, state line against the panel.
                Require(bounds.panel.y0>=window[1]*.5f&&!bounds.stateApart&&std::abs((bounds.panel.x0+bounds.panel.x1)*.5f-window[0]*.5f)<=1,"Match HUD fell back to the wrong place from Top center");
            }else{
                CheckTopCenterBounds(view,bounds,window[0],window[1]);
                const float factor=size==0?.85f:size==1?1.f:1.25f;
                Require(bounds.panelScale<=usual*factor+.001f&&bounds.panelScale>=.5f*usual*factor-.001f,"Match HUD at Top center has an unexpected scale");
            }
            width[size]=bounds.panel.x1-bounds.panel.x0;
        }
        Require(width[0]<width[1]&&width[1]<width[2],"Match HUD sizes collapse at Top center");
    }
    // At 1080p the split panel keeps its usual scale at every size; the Ember strip is shrunk until
    // the Large one exactly fills the band (35 units, 52.5 px), and keeps three different sizes.
    MatchStripView split;split.names[0]="P1";split.names[1]="P2";split.layout=1;split.anchor=sf4e::netplay::MaxMatchHudAnchor;
    MatchStripView strip=split;strip.layout=0;
    const float p1080[2]={1920,1080};
    const float factors[3]={.85f,1.f,1.25f};
    for(int size=0;size<3;++size){
        split.size=strip.size=size;
        const auto a=geometry(split,p1080),b=geometry(strip,p1080);
        Require(!a.panelBelow&&std::abs(a.panelScale-factors[size])<.001f,"Match HUD split panel shrank at Top center on 1080p");
        Require(!b.panelBelow&&b.panelScale<factors[size]-.05f,"Match HUD strip did not shrink to the band at Top center on 1080p");
        if(size==2)Require(std::abs((b.panel.y1-b.panel.y0)-52.5f)<.01f&&std::abs(b.panel.y0-4.5f)<.01f,"Match HUD Large strip does not fill the band at Top center on 1080p");
    }
    // A taller band (names moved down) grows the panel; one moved up shrinks it, and past half the usual scale it goes to Bottom center.
    const float p4x3[2]={1024,768};
    split.size=strip.size=1;
    auto lowered=split;lowered.nameOffset=20;
    const auto level=geometry(split,p4x3),deeper=geometry(lowered,p4x3);
    Require(!level.panelBelow&&level.panelScale<.8f&&deeper.panelScale>level.panelScale,"Match HUD split panel did not grow with the name offset at Top center");
    auto grownStrip=strip;grownStrip.nameOffset=20;
    const auto flat=geometry(strip,p1080),grown=geometry(grownStrip,p1080);
    Require(grown.panelScale>flat.panelScale&&grown.panel.y1<=(38+20)*1.5f+.01f,"Match HUD strip did not grow with the name offset at Top center");
    Require(geometry(strip,p4x3).panelBelow,"Match HUD strip stayed at Top center on a 4:3 window, where the band is too low");
    Require(!geometry(split,p4x3).panelBelow,"Match HUD split panel left Top center on a 4:3 window");
    auto high=split;high.nameOffset=-40;
    Require(geometry(high,p1080).panelBelow,"Match HUD split panel stayed at Top center with the names moved far up");
    const float p720[2]={1280,720};
    auto upStrip=strip;upStrip.nameOffset=-20;
    Require(geometry(upStrip,p720).panelBelow&&!geometry(strip,p720).panelBelow,"Match HUD strip placed wrongly at Top center on 720p");
    Require(std::string(MatchStripAnchorName(sf4e::netplay::MaxMatchHudAnchor))==sf4e::loc::T("hud_position.top_center")&&
        std::string(MatchStripAnchorName(99))==sf4e::loc::T("hud_position.top_center"),"Top center has no name");
}
bool MatchHudShotCaptured(const char* shot,int w,int h,float dpi){
    const std::string name=shot?shot:"";
    return (name=="match-hud"||name=="match-hud-split")&&((w==1280&&h==720&&dpi==1)||
            (w==1920&&h==1080&&dpi==1)||(w==2560&&h==1440)||(w==3440&&h==1440)||(w==3840&&h==2160&&dpi==2))||
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
