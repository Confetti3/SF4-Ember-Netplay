// The in-match network HUD: the Ember strip (one panel) and the split layout (a name
// plate or banner per fighter and a small telemetry panel). One text fitter, one
// state-line painter and one panel anchoring serve both; only how the names and the
// telemetry are arranged differs. The declarations live in Theme.hxx with the other
// presentation views.
#include "Theme.hxx"
#include "MenuProbes.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
std::string MatchStripStateLine(const MatchStripView& view) {
    // The most urgent condition wins: a hard notice, then a stall (the game
    // is visibly frozen and the player needs to know why), then a warning.
    if(view.noticeSeverity>=2&&!view.notice.empty())return view.notice;
    if(view.predictionStalled)return loc::T("match.waiting_opponent");
    if(view.connectionWarning){
        if(view.disconnectCountdownMs>=0)
            return loc::Tf("match.connection_countdown",(view.disconnectCountdownMs+999)/1000);
        return loc::T("match.connection_unstable");
    }
    return view.notice;
}
void DrawMatchNotice(const std::string& message,int severity) {
    if(message.empty())return;
    const auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x*.5f,vp->Pos.y+vp->Size.y-24*Scale()),ImGuiCond_Always,ImVec2(.5f,1));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x*.6f,0));
    ImGui::Begin("Match notice",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing);
    const ImVec4 colors[]={ImVec4(.71f,.66f,.61f,1),ImVec4(1,.77f,.38f,1),ImVec4(1,.46f,.38f,1)};
    ImGui::PushStyleColor(ImGuiCol_Text,colors[(std::max)(0,(std::min)(2,severity))]);
    ImGui::TextWrapped("%s",message.c_str());ImGui::PopStyleColor();ImGui::End();
}
namespace {
constexpr float MatchWidth=520.f;
constexpr float MatchHeight=62.f;
constexpr float MatchStateHeight=24.f;
constexpr float SplitHeight=34.f,SplitLabel=13.f,SplitValue=18.f,SplitName=19.f;
float MatchScale(const MatchStripView& view) {
    const float sizes[]={.85f,1.f,1.25f};
    // The readability floor belongs to the viewport term alone. Flooring the
    // product made Small and Standard identical at 720p and collapsed all three
    // at 480p, so two of the three settings did nothing.
    const float viewport=(std::max)(.8f,ImGui::GetMainViewport()->Size.y/1080.f);
    return viewport*sizes[(std::max)(0,(std::min)(2,view.size))];
}
float TextWidth(float size,const std::string& t){return DiagnosticFont()->CalcTextSizeA(size,FLT_MAX,0,t.c_str()).x;}
// `t` cut on a character boundary with an ellipsis to fit `maxWidth`; empty when not even that fits.
std::string FitText(std::string t,float maxWidth,float size){
    if(TextWidth(size,t)<=maxWidth)return t;
    const std::string ellipsis="...";
    while(!t.empty()&&TextWidth(size,t+ellipsis)>maxWidth){
        auto end=t.size()-1;while(end>0&&(static_cast<unsigned char>(t[end])&0xc0)==0x80)--end;t.resize(end);
    }
    return TextWidth(size,ellipsis)<=maxWidth?t+ellipsis:std::string();
}
// A neutral panel keeps the telemetry readable on bright stages, but a solid block
// pulled the eye mid-fight: let the stage show through a little. Quiet border.
void DrawPanelBox(ImDrawList* draw,ImVec2 p,float w,float h,float s){
    draw->AddRectFilled(p,ImVec2(p.x+w,p.y+h),IM_COL32(20,19,18,200),6*s);
    draw->AddRect(p,ImVec2(p.x+w,p.y+h),IM_COL32(88,76,65,100),6*s);
}
// Link state / notice line beside a panel, `top` its upper edge. Same width as the
// panel, own background so it reads against any stage; colour follows severity.
void DrawStateLine(const MatchStripView& view,ImDrawList* draw,float x,float w,float top,float s){
    const auto state=MatchStripStateLine(view);
    if(state.empty())return;
    const int severity=view.noticeSeverity>0||view.connectionWarning||view.predictionStalled?
        (std::max)(view.noticeSeverity,view.connectionWarning||view.predictionStalled?1:0):0;
    const ImU32 colors[]={IM_COL32(181,169,155,255),IM_COL32(255,196,96,255),IM_COL32(255,118,96,255)};
    const float h=MatchStateHeight*s;
    draw->AddRectFilled(ImVec2(x,top),ImVec2(x+w,top+h),IM_COL32(20,19,18,235),4*s);
    const auto line=FitText(state,w-16*s,16*s);
    draw->AddText(DiagnosticFont(),16*s,ImVec2(x+8*s,top+(h-16*s)*.5f),colors[(std::max)(0,(std::min)(2,severity))],line.c_str());
}
struct PanelSpot{ImVec2 pos;float stateTop=0;};
// Where the telemetry panel (w by h) sits by the anchor and spacing preferences, and the
// top of its state line: above the panel at the bottom anchors, below it at the top ones, so
// it stays on screen. `minTop` keeps a top-anchored panel, and so its state line, under
// anything else drawn along the top edge.
PanelSpot PlacePanel(const MatchStripView& view,float w,float h,float s,float minTop){
    const auto* vp=ImGui::GetMainViewport();
    const float gap=(view.raised?48.f:12.f)*(std::max)(.8f,vp->Size.y/1080.f);
    // The side anchors keep the 10% margins the strip has always stayed within.
    const int anchor=(std::max)(0,(std::min)(4,view.anchor));
    const bool top=anchor>=3,left=anchor==1||anchor==3,right=anchor==2||anchor==4;
    const float x=left?vp->Pos.x+vp->Size.x*.1f:right?vp->Pos.x+vp->Size.x*.9f-w:vp->Pos.x+(vp->Size.x-w)*.5f;
    // The state line takes the room on the side facing the screen centre; keep it on screen.
    const float stateRoom=(MatchStateHeight+4)*s,low=top?(std::max)(vp->Pos.y,minTop):vp->Pos.y+stateRoom,high=vp->Pos.y+vp->Size.y-h-(top?stateRoom:0.f);
    const float wanted=top?vp->Pos.y+gap:vp->Pos.y+vp->Size.y-gap-h;
    PanelSpot spot;spot.pos=ImVec2(x,(std::max)(low,(std::min)(high,wanted)));
    spot.stateTop=top?spot.pos.y+h+4*s:spot.pos.y-MatchStateHeight*s-4*s;
    return spot;
}

// The Ember strip: one panel with "A vs B" over ping, rollback and delay.
void PaintMatchStrip(const MatchStripView& view, ImDrawList* draw, ImVec2 p, float w, float s) {
    NoteUserText(view.names[0]);NoteUserText(view.names[1]);
    const float glyph=16*s,glyphGap=6*s,glyphSpace=glyph+glyphGap;
    auto* font=DiagnosticFont();
    DrawPanelBox(draw,p,w,MatchHeight*s,s);
    const auto text=[&](float x,float y,const std::string& t,float size,ImU32 color){draw->AddText(font,size,ImVec2(x,y),color,t.c_str());};
    // One centred "A vs B" line. Anchoring each name to a fixed "vs" made the
    // pair lopsided whenever the names differed in length.
    // The names share whatever the middle label (a running score can be wide) leaves.
    const auto versus=view.score.empty()?std::string(loc::T("match.versus_short")):view.score;
    // Each side's link mark sits on the outer side of its name.
    const auto gap=12*s,vsWidth=TextWidth(16*s,versus),nameWidth=(w-30*s-2*gap-vsWidth-2*glyphSpace)*.5f;
    const auto left=FitText(view.names[0],nameWidth,20*s),right=FitText(view.names[1],nameWidth,20*s);
    const auto leftWidth=TextWidth(20*s,left),rightWidth=TextWidth(20*s,right);
    auto x=p.x+(w-(glyphSpace+leftWidth+gap+vsWidth+gap+rightWidth+glyphSpace))*.5f;
    DrawNetworkLinkGlyph(draw,ImVec2(x,p.y+7*s),glyph,view.links[0]);x+=glyphSpace;
    text(x,p.y+4*s,left,20*s,IM_COL32(243,235,221,255));x+=leftWidth+gap;
    text(x,p.y+6*s,versus,16*s,IM_COL32(255,135,56,230));x+=vsWidth+gap;
    text(x,p.y+4*s,right,20*s,IM_COL32(243,235,221,255));
    DrawNetworkLinkGlyph(draw,ImVec2(x+rightWidth+glyphGap,p.y+7*s),glyph,view.links[1]);
    const std::string labels[]={loc::T("match.ping"),loc::T("match.rollback"),view.spectator?"":loc::T("match.delay")};
    const std::string values[]={view.pingMs<0?"\xe2\x80\x94":std::to_string(view.pingMs)+" ms",
        std::to_string(view.rollbackFrames)+"f",view.spectator?loc::T("match.spectating"):view.appliedDelay<0?"\xe2\x80\x94":std::to_string(view.appliedDelay)+"f"};
    const float column=(w-24*s)/3;
    for(int i=0;i<3;++i){
        const float x=p.x+12*s+column*i;
        // Fixed label/value anchors keep the layout stable as values change.
        text(x,p.y+33*s,labels[i],16*s,IM_COL32(181,169,155,255));
        const float valueX=x+(labels[i].empty()?0:TextWidth(16*s,labels[i])+8*s);
        // A translated label must leave room for its whole value.
        ReportMenuText(("hud-telemetry/"+std::to_string(i)).c_str(),22*s,MatchHeight*s,valueX-x+TextWidth(22*s,values[i]),column-4*s);
        const auto value=FitText(values[i],x+column-valueX-4*s,22*s);
        text(valueX,p.y+29*s,value,22*s,IM_COL32(243,235,221,255));
    }
}

// The split layout: a name plate under each life bar and a small telemetry panel.
// Same Ember language as the strip (Inter, the dark translucent panel, the link
// glyphs, an Ember rule), only arranged around the game's own HUD.
struct Plate {
    std::string name;NetworkLink link=NetworkLink::Unknown;
    float x0=0,y0=0,x1=0,y1=0,k=1,font=SplitName;bool right=false,showGlyph=true;
};
// `edge` is the plate's outer edge (its left side for P1, right for P2); the
// link glyph sits on the inner side of the name. The glyph is dropped when its
// room would truncate the name; the name is then cut only if it still does not fit.
Plate MakePlate(const MatchStripView& view,int side,float edge,float y,float k,float maxWidth){
    Plate p;p.right=side==1;p.k=k;p.font=SplitName*k;p.link=view.links[side];
    const float glyph=p.font*.9f,gap=6*k,withGlyph=21*k+gap+glyph,without=21*k;
    p.name=FitText(view.names[side],(std::max)(0.f,maxWidth-withGlyph),p.font);
    p.showGlyph=p.name==view.names[side];
    if(!p.showGlyph)p.name=FitText(view.names[side],(std::max)(0.f,maxWidth-without),p.font);
    const float w=(p.showGlyph?withGlyph:without)+TextWidth(p.font,p.name);
    p.x0=p.right?edge-w:edge;p.x1=p.x0+w;p.y0=y;p.y1=y+p.font+6*k;
    return p;
}
void DrawPlate(ImDrawList* draw,const Plate& p){
    const float k=p.k,glyph=p.font*.9f,gap=6*k;
    draw->AddRectFilled(ImVec2(p.x0,p.y0),ImVec2(p.x1,p.y1),IM_COL32(20,19,18,185),5*k);
    draw->AddRect(ImVec2(p.x0,p.y0),ImVec2(p.x1,p.y1),IM_COL32(88,76,65,110),5*k);
    // The Ember rule on the outer edge, in line with the life bar's end.
    const float rule=p.right?p.x1-7*k:p.x0+4*k;
    draw->AddRectFilled(ImVec2(rule,p.y0+4*k),ImVec2(rule+3*k,p.y1-4*k),palette::Ember,1.5f*k);
    const float nameWidth=p.x1-p.x0-21*k-(p.showGlyph?gap+glyph:0.f);
    const float nameX=p.right?p.x0+7*k+(p.showGlyph?glyph+gap:0.f):p.x0+14*k;
    const float glyphX=p.right?p.x0+7*k:nameX+nameWidth+gap;
    if(p.showGlyph)DrawNetworkLinkGlyph(draw,ImVec2(glyphX,p.y0+(p.y1-p.y0-glyph)*.5f),glyph,p.link);
    if(p.name.empty())return;
    auto* font=DiagnosticFont();
    draw->AddText(font,p.font,ImVec2(nameX+k,p.y0+3*k+k),IM_COL32(0,0,0,190),p.name.c_str());
    draw->AddText(font,p.font,ImVec2(nameX,p.y0+3*k),palette::Ivory,p.name.c_str());
}
// Names above the life bars: one banner per side across the top strip, the name
// and link glyph inside, and the side's games won in a square box at the outer end.
struct Banner {
    std::string name;NetworkLink link=NetworkLink::Unknown;
    float x0=0,y0=0,x1=0,y1=0,k=1,font=SplitName;bool right=false,hasScore=false;int score=0;
};
constexpr float BannerHeight=34.f;
Banner MakeBanner(const MatchStripView& view,int side,float x0,float x1,float yMid,float k){
    Banner b;b.right=side==1;b.k=k;b.font=SplitName*k;b.link=view.links[side];b.hasScore=view.hasScores;b.score=view.scores[side];
    const float h=BannerHeight*k,box=h-8*k,glyph=b.font*.9f;
    b.x0=x0;b.x1=x1;b.y0=yMid-h*.5f;b.y1=yMid+h*.5f;
    const float outer=b.hasScore?4*k+box+8*k:12*k,inner=8*k+glyph+6*k;
    b.name=FitText(view.names[side],(std::max)(0.f,x1-x0-outer-inner),b.font);
    return b;
}
void DrawBanner(ImDrawList* draw,const Banner& b){
    const float k=b.k,h=b.y1-b.y0,box=h-8*k,glyph=b.font*.9f;
    auto* font=DiagnosticFont();
    draw->AddRectFilled(ImVec2(b.x0,b.y0),ImVec2(b.x1,b.y1),IM_COL32(20,19,18,200),6*k);
    draw->AddRect(ImVec2(b.x0,b.y0),ImVec2(b.x1,b.y1),IM_COL32(88,76,65,110),6*k);
    // The Ember rule along the lower edge, the banner's accent.
    draw->AddRectFilled(ImVec2(b.x0+6*k,b.y1-2.5f*k),ImVec2(b.x1-6*k,b.y1-.5f*k),palette::Ember,1*k);
    float nameLeft=b.x0+12*k,nameRight=b.x1-8*k-glyph-6*k;
    if(b.right){nameLeft=b.x0+8*k+glyph+6*k;nameRight=b.x1-12*k;}
    if(b.hasScore){
        const float bx=b.right?b.x1-4*k-box:b.x0+4*k,by=b.y0+4*k;
        draw->AddRectFilled(ImVec2(bx,by),ImVec2(bx+box,by+box),IM_COL32(12,11,10,235),4*k);
        draw->AddRect(ImVec2(bx,by),ImVec2(bx+box,by+box),palette::Ember,4*k,0,1.5f*k);
        const auto digits=std::to_string(b.score);const float size=b.font*1.05f,w=TextWidth(size,digits);
        draw->AddText(font,size,ImVec2(bx+(box-w)*.5f,by+(box-size)*.5f),palette::Ember,digits.c_str());
        if(b.right)nameRight=bx-8*k;else nameLeft=bx+box+8*k;
    }
    const float glyphX=b.right?b.x0+8*k:b.x1-8*k-glyph;
    DrawNetworkLinkGlyph(draw,ImVec2(glyphX,b.y0+(h-glyph)*.5f-1*k),glyph,b.link);
    if(b.name.empty())return;
    const float x=b.right?nameRight-TextWidth(b.font,b.name):nameLeft,y=b.y0+(h-b.font)*.5f-1*k;
    draw->AddText(font,b.font,ImVec2(x+k,y+k),IM_COL32(0,0,0,190),b.name.c_str());
    draw->AddText(font,b.font,ImVec2(x,y),palette::Ivory,b.name.c_str());
}
// "FT5" centered above the timer.
struct SetTag{std::string text;float x0=0,y0=0,x1=0,y1=0,k=1,font=12;bool valid=false;};
SetTag MakeSetTag(const MatchStripView& view,float centerX,float y,float k){
    SetTag t;if(view.setFormat<=0)return t;
    t.k=k;t.font=12*k;t.text="FT"+std::to_string(view.setFormat);t.valid=true;
    const float w=TextWidth(t.font,t.text)+14*k;
    t.x0=centerX-w*.5f;t.x1=centerX+w*.5f;t.y0=y;t.y1=y+t.font+6*k;
    return t;
}
void DrawSetTag(ImDrawList* draw,const SetTag& t){
    if(!t.valid)return;
    draw->AddRectFilled(ImVec2(t.x0,t.y0),ImVec2(t.x1,t.y1),IM_COL32(20,19,18,200),4*t.k);
    draw->AddRect(ImVec2(t.x0,t.y0),ImVec2(t.x1,t.y1),IM_COL32(255,135,56,120),4*t.k);
    draw->AddText(DiagnosticFont(),t.font,ImVec2(t.x0+7*t.k,t.y0+3*t.k),palette::Ember,t.text.c_str());
}
struct Cell{std::string label,value;float width=0;ImU32 color=palette::Ivory;};
struct Telemetry{std::vector<Cell> cells;float w=0,h=SplitHeight,s=1;};
Telemetry MakeTelemetry(const MatchStripView& view,float s,float maxWidth){
    Telemetry t;t.s=s;t.h=SplitHeight*s;
    struct Spec{std::string label,value,reserve;ImU32 color;};
    std::vector<Spec> specs;
    // Names above the life bars carry the score in their boxes.
    if(!view.score.empty()&&!view.namesAbove)specs.push_back({"",view.score,"",IM_COL32(255,135,56,230)});
    specs.push_back({loc::T("match.ping"),view.pingMs<0?"\xe2\x80\x94":std::to_string(view.pingMs)+" ms","999 ms",palette::Ivory});
    specs.push_back({loc::T("match.rollback"),std::to_string(view.rollbackFrames)+"f","99f",palette::Ivory});
    specs.push_back(view.spectator?Spec{"",loc::T("match.spectating"),"",palette::Ivory}:
        Spec{loc::T("match.delay"),view.appliedDelay<0?"\xe2\x80\x94":std::to_string(view.appliedDelay)+"f","99f",palette::Ivory});
    if(view.spectators>0)specs.push_back({loc::T("room.status.watching"),std::to_string(view.spectators),"99",palette::Ivory});
    float total=0;
    for(const auto& spec:specs){
        Cell c;c.label=spec.label;c.value=spec.value;c.color=spec.color;
        const float label=c.label.empty()?0:TextWidth(SplitLabel*s,c.label)+6*s;
        c.width=20*s+label+(std::max)(TextWidth(SplitValue*s,c.value),TextWidth(SplitValue*s,spec.reserve));
        total+=c.width;t.cells.push_back(std::move(c));
    }
    // Over a narrow viewport the cells share what there is; values then truncate.
    const float shrink=total>maxWidth?maxWidth/total:1.f;
    for(auto& c:t.cells)c.width*=shrink;
    t.w=total*shrink;
    return t;
}
void DrawTelemetry(ImDrawList* draw,const Telemetry& t,ImVec2 p){
    const float s=t.s;auto* font=DiagnosticFont();
    DrawPanelBox(draw,p,t.w,t.h,s);
    // A short Ember tab on the top edge marks the panel as Ember's.
    draw->AddRectFilled(ImVec2(p.x+10*s,p.y),ImVec2(p.x+34*s,p.y+2*s),palette::Ember);
    float x=p.x;int index=0;
    for(const auto& c:t.cells){
        if(index>0)draw->AddLine(ImVec2(x,p.y+9*s),ImVec2(x,p.y+t.h-9*s),IM_COL32(88,76,65,140),1.f);
        float tx=x+10*s;
        const float label=c.label.empty()?0:TextWidth(SplitLabel*s,c.label)+6*s;
        if(!c.label.empty())draw->AddText(font,SplitLabel*s,ImVec2(tx,p.y+(t.h-SplitLabel*s)*.5f+1.5f*s),palette::Muted,c.label.c_str());
        const float room=c.width-20*s-label;
        const auto value=FitText(c.value,room,SplitValue*s);
        ReportMenuText(("hud-telemetry-split/"+std::to_string(index)).c_str(),SplitValue*s,t.h,label+TextWidth(SplitValue*s,c.value),c.width-20*s);
        draw->AddText(font,SplitValue*s,ImVec2(tx+label,p.y+(t.h-SplitValue*s)*.5f),c.color,value.c_str());
        x+=c.width;++index;
    }
}
struct SplitPlaced {
    Plate plates[2];Banner banners[2];SetTag tag;bool above=false;
    Telemetry tel;PanelSpot spot;bool hasState=false;
};
SplitPlaced PlaceSplit(const MatchStripView& view){
    const auto* vp=ImGui::GetMainViewport();
    NoteUserText(view.names[0]);NoteUserText(view.names[1]);
    const float s=MatchScale(view);
    // The game keeps 16:9 inside the window: measure from its image so the names
    // stay beside the life bars on an ultrawide screen.
    const float gs=(std::min)(vp->Size.y/720.f,vp->Size.x/1280.f),gameW=1280*gs;
    const float gx0=vp->Pos.x+(vp->Size.x-gameW)*.5f,gy0=vp->Pos.y+(vp->Size.y-720*gs)*.5f;
    SplitPlaced out;out.above=view.namesAbove;
    float minTop=0;
    if(out.above){
        // The top strip above the life bars (y about 6..44 of 720, centre line 26), between the
        // portraits (to x 102 and from x 1178) and clear of the timer box (x 562..718).
        const float sizes[]={.85f,1.f,1.25f};
        const float k=gs*sizes[(std::max)(0,(std::min)(2,view.size))];
        out.banners[0]=MakeBanner(view,0,gx0+112*gs,gx0+556*gs,gy0+26*gs,k);
        out.banners[1]=MakeBanner(view,1,gx0+724*gs,gx0+1168*gs,gy0+26*gs,k);
        // Ends above the game's K.O. sign (it starts at y 26) at every size: the tag stops
        // growing past 1.1, so its top at y 3 and height of 18 units end by y 23.
        out.tag=MakeSetTag(view,gx0+640*gs,gy0+3*gs,(std::min)(k,1.1f*gs));
        // A top-anchored telemetry panel starts below the banners and the tag, never on them.
        minTop=(std::max)(out.banners[0].y1,out.banners[1].y1)+6*gs;
        if(out.tag.valid)minTop=(std::max)(minTop,out.tag.y1+6*gs);
    }else{
        // The game's own HUD fills the rest of the strip under each life bar: the character
        // logo (x 91..230 and 1050..1189) and the round markers (to x 474 and from x 806).
        // The plates take the gap between them, y 121..143 at most, so Large is the full 22
        // and the other sizes shrink: the plate is 25 units high (19 of text), 22 gs at Large.
        const float plateSizes[]={.78f,.89f,1.f};
        const float k=gs*.88f*plateSizes[(std::max)(0,(std::min)(2,view.size))],maxName=238*gs;
        out.plates[0]=MakePlate(view,0,gx0+236*gs,gy0+121*gs,k,maxName);
        out.plates[1]=MakePlate(view,1,gx0+1044*gs,gy0+121*gs,k,maxName);
    }
    out.tel=MakeTelemetry(view,s,vp->Size.x*.8f);
    out.spot=PlacePanel(view,out.tel.w,out.tel.h,s,minTop);
    out.hasState=!MatchStripStateLine(view).empty();
    return out;
}
}
float MatchStripScale(const MatchStripView& view) { return MatchScale(view); }
MatchStripBounds MatchStripGeometry(const MatchStripView& view){
    MatchStripBounds bounds;
    if(view.layout!=1)return bounds;
    const auto placed=PlaceSplit(view);
    if(placed.above){
        for(int side=0;side<2;++side){const auto& b=placed.banners[side];bounds.names[side]={b.x0,b.y0,b.x1,b.y1,true};}
        const auto& t=placed.tag;bounds.setTag={t.x0,t.y0,t.x1,t.y1,t.valid};
    }else for(int side=0;side<2;++side){const auto& p=placed.plates[side];bounds.names[side]={p.x0,p.y0,p.x1,p.y1,true};}
    const float s=placed.tel.s;
    bounds.panel={placed.spot.pos.x,placed.spot.pos.y,placed.spot.pos.x+placed.tel.w,placed.spot.pos.y+placed.tel.h,true};
    if(placed.hasState){
        bounds.panel.y0=(std::min)(bounds.panel.y0,placed.spot.stateTop);
        bounds.panel.y1=(std::max)(bounds.panel.y1,placed.spot.stateTop+MatchStateHeight*s);
    }
    return bounds;
}
void DrawMatchStrip(const MatchStripView& view) {
    auto* draw=ImGui::GetForegroundDrawList();
    if(view.layout==1){
        const auto placed=PlaceSplit(view);
        if(placed.above){for(const auto& banner:placed.banners)DrawBanner(draw,banner);DrawSetTag(draw,placed.tag);}
        else for(const auto& plate:placed.plates)DrawPlate(draw,plate);
        DrawTelemetry(draw,placed.tel,placed.spot.pos);
        DrawStateLine(view,draw,placed.spot.pos.x,placed.tel.w,placed.spot.stateTop,placed.tel.s);
        return;
    }
    const auto* vp=ImGui::GetMainViewport();const float s=MatchScale(view);
    const float w=(std::min)(MatchWidth*s,vp->Size.x*.8f);
    const auto spot=PlacePanel(view,w,MatchHeight*s,s,0);
    PaintMatchStrip(view,draw,spot.pos,w,s);
    DrawStateLine(view,draw,spot.pos.x,w,spot.stateTop,s);
}
void DrawMatchStripPreview(const MatchStripView& view) {
    const auto available=ImGui::GetContentRegionAvail();
    const float s=(std::min)(MatchScale(view),(std::max)(1.f,available.x)/MatchWidth);
    const auto p=ImGui::GetCursorScreenPos();
    if(view.layout==1){
        // The names on one row, left and right, with the telemetry panel under them.
        NoteUserText(view.names[0]);NoteUserText(view.names[1]);
        auto* draw=ImGui::GetWindowDrawList();const float width=MatchWidth*s,k=.75f*s;
        float top=0;
        if(view.namesAbove){
            // The set tag on its own row, then the two banners at the top of the picture.
            const auto tag=MakeSetTag(view,p.x+width*.5f,p.y,k);
            DrawSetTag(draw,tag);
            const float rowTop=tag.valid?tag.y1-p.y+4*s:0.f,h=BannerHeight*k;
            const Banner banners[2]={MakeBanner(view,0,p.x,p.x+width*.46f,p.y+rowTop+h*.5f,k),MakeBanner(view,1,p.x+width*.54f,p.x+width,p.y+rowTop+h*.5f,k)};
            for(const auto& banner:banners)DrawBanner(draw,banner);
            top=rowTop+h+8*s;
        }else{
            const Plate plates[2]={MakePlate(view,0,p.x,p.y,k,width*.48f),MakePlate(view,1,p.x+width,p.y,k,width*.48f)};
            for(const auto& plate:plates)DrawPlate(draw,plate);
            top=plates[0].y1-p.y+8*s;
        }
        const auto tel=MakeTelemetry(view,s,width);
        DrawTelemetry(draw,tel,ImVec2(p.x+(width-tel.w)*.5f,p.y+top));
        ImGui::Dummy(ImVec2(width,top+tel.h));
    }else{
        PaintMatchStrip(view,ImGui::GetWindowDrawList(),p,MatchWidth*s,s);
        ImGui::Dummy(ImVec2(MatchWidth*s,MatchHeight*s));
    }
    ImGui::TextDisabled("%s",loc::Tf("match.preview_spacing",view.raised?loc::T("spacing.raised"):loc::T("spacing.normal")).c_str());
    ImGui::TextDisabled("%s",loc::Tf("match.preview_position",MatchStripAnchorName(view.anchor)).c_str());
}
const char* MatchStripLayoutName(int layout) {
    return loc::T(layout==1?"hud_layout.split":"hud_layout.strip");
}
const char* MatchStripNamesName(int names) {
    return loc::T(names==1?"hud_names.above":"hud_names.below");
}
const char* MatchStripAnchorName(int anchor) {
    const char* names[]={loc::T("hud_position.bottom_center"),loc::T("hud_position.bottom_left"),loc::T("hud_position.bottom_right"),loc::T("hud_position.top_left"),loc::T("hud_position.top_right")};
    return names[(std::max)(0,(std::min)(4,anchor))];
}
} }
