// The room board of an Arcade or King of the hill room (Snapshot::arcade,
// Arcade.dc.html): sixteen cabinets in two rows, each pair in its own colour
// and linked by a line wherever its two cabinets stand; King of the hill the
// same under its top slot. Every cabinet is its row. A Classic room keeps its
// table cards (RoomPanelTables.cxx).
#include "RoomPanelBoard.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "LatencyMark.hxx"
#include "RoomControls.hxx"
#include "RowId.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <optional>
#include <utility>

namespace sf4e { namespace ui {
using namespace room_controls;
namespace {
// Each pair's own colour (Arcade.dc.html), lit on both of its cabinets: content, like the tiers' colours.
// None is orange, which stays the focus mark's.
ImU32 PairColor(std::size_t table) {
    static const ImU32 pairs[]={IM_COL32(127,176,224,255),IM_COL32(201,160,224,255),IM_COL32(164,206,160,255),IM_COL32(110,204,200,255),
        IM_COL32(236,128,176,255),IM_COL32(213,220,227,255),IM_COL32(206,214,110,255),IM_COL32(230,100,100,255)};
    return pairs[table%8];
}
// A cabinet's place in its row, as a share of its own height: the cabinet and
// the gap under it that the pair lines run through.
constexpr float CabinetReach=1.1f;
// The floor's measures: the gap between cabinets, the aisle between the two
// rows, a cabinet's least height (under it the rows scroll), King of the
// hill's top slot banner, and a line of matchups, four to a line so all eight
// pairs of a full room fit on two.
constexpr float CabinetGap=8,Aisle=36,LeastCabinet=92,TopSlotHeight=48,MatchupLine=30;
constexpr std::size_t MatchupsPerLine=4;
float MatchupStripHeight(std::size_t matchups,float s){return matchups?((matchups+MatchupsPerLine-1)/MatchupsPerLine)*MatchupLine*s+6*s:0;}
std::size_t MatchupCount(const std::vector<MenuEntry>& rows){
 return static_cast<std::size_t>(std::count_if(rows.begin(),rows.end(),[](const MenuEntry& e){return RowKey<int>(e.id,"matchup-").has_value();}));
}
// A cabinet's cell in rows `width` wide, which keeps three quarters of its
// least height however narrow they are; the rows scroll across past that.
float CabinetCell(float width,float s){return (std::max)(LeastCabinet*s*.75f,(width-7*CabinetGap*s)/8);}
bool RowsScrollAcross(float width,float s){return 8*CabinetCell(width,s)+7*CabinetGap*s>width+.5f;}
// `a` toward `b` by `t` (0 to 1): the shades of a part between two of the palette's roles.
ImU32 Mix(ImU32 a,ImU32 b,float t) {
    const ImVec4 x=ImGui::ColorConvertU32ToFloat4(a),y=ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x+(y.x-x.x)*t,x.y+(y.y-x.y)*t,x.z+(y.z-x.z)*t,x.w+(y.w-x.w)*t));
}

// A cabinet drawn as an upright seen from the front: the lit marquee and the
// control panel stand a little proud of the body, as on a real cabinet, so
// its outline (the T-molding, in the pair's colour) reads as one at any size.
struct Cabinet {
    v2::Box marquee,bezel,screen,plate,panel,lower;
    float inset;
    // Speaker grilles, scanlines, the coin door and the second row of buttons
    // only where they still read; a small cabinet keeps the shapes that tell it apart.
    bool detail;
};
Cabinet LayOutCabinet(v2::Box b,float s) {
    Cabinet k;const float w=b.W(),h=b.H();
    k.detail=h>=150*s;k.inset=(std::max)(2*s,w*.045f);
    v2::Box body=b;
    k.marquee=v2::CutTop(body,(std::max)(16*s,h*.13f));
    body=v2::Box{ImVec2(b.min.x+k.inset,body.min.y),ImVec2(b.max.x-k.inset,body.max.y)};
    k.lower=v2::CutBottom(body,k.detail?h*.12f:(std::max)(4*s,h*.05f));
    const v2::Box panel=v2::CutBottom(body,k.detail?h*.16f:(std::max)(10*s,h*.12f),2*s);
    k.panel=v2::Box{ImVec2(b.min.x,panel.min.y),ImVec2(b.max.x,panel.max.y)};
    k.plate=v2::Inset(v2::CutBottom(body,22*s,3*s),3*s,0);
    v2::CutTop(body,k.detail?(std::max)(5*s,h*.035f):3*s);
    k.bezel=v2::Inset(body,(std::max)(1*s,w*.025f),0);
    k.screen=v2::Inset(k.bezel,(std::max)(2*s,w*.035f),(std::max)(2*s,w*.035f));
    return k;
}
// `picture` on the cabinet's glass, as big as the glass holds, standing on
// its foot, in front of the screen's own colour (a pair's, so each pair's
// screens read apart), with the scanlines over it and a shade at its foot for
// the screen's words. False while the art has no image for it.
bool DrawCabinetFighter(ImDrawList* d,const v2::Box& glass,const FighterPicture& picture,ImU32 tone,float s) {
    if(glass.H()<24*s)return false;
    ReportFighterPicture(picture,glass.min,glass.max);
    const auto image=FighterImage(picture,glass.H());
    if(!image.texture||image.width<=0||image.height<=0)return false;
    const v2::Box g=v2::Inset(glass,1.5f*s,1.5f*s);
    d->AddRectFilledMultiColor(g.min,g.max,v2::color::Alpha(tone,.55f),v2::color::Alpha(tone,.55f),v2::color::Alpha(tone,.2f),v2::color::Alpha(tone,.2f));
    const float factor=(std::min)(g.W()/image.width,g.H()/image.height),w=image.width*factor,h=image.height*factor;
    const float x=(g.min.x+g.max.x-w)*.5f;
    d->AddImage(image.texture,ImVec2(x,g.max.y-h),ImVec2(x+w,g.max.y),image.uvMin,image.uvMax);
    const auto& c=v2::Colors();
    if(g.H()>40*s)for(float y=g.min.y+.5f*s;y<g.max.y;y+=3*s)d->AddLine(ImVec2(g.min.x,y),ImVec2(g.max.x,y),v2::color::Alpha(c.ink,.22f),(std::max)(1.f,.75f*s));
    const float shade=g.min.y+g.H()*.45f;
    d->AddRectFilledMultiColor(ImVec2(g.min.x,shade),g.max,v2::color::Alpha(c.ink,0),v2::color::Alpha(c.ink,0),v2::color::Alpha(c.ink,.85f),v2::color::Alpha(c.ink,.85f));
    return true;
}
// The cabinet's shell and its fixed parts: body, marquee ground, bezel and
// glass glowing in `glow`, control panel with its stick and six buttons, coin
// door and kick plate, and the outline in `edge`. The words on the marquee,
// screen and plate are the board's.
void DrawCabinet(ImDrawList* d,v2::Box b,const Cabinet& k,float s,ImU32 marqueeGround,ImU32 glow,float glowStrength,ImU32 edge,
                 bool seated,ImU32 kickGlow) {
    const auto& c=v2::Colors();
    const float l=b.min.x,r=b.max.x,il=l+k.inset,ir=r-k.inset,w=b.W();
    // The body: darker at its sides, so it reads as a box rather than a card.
    const float mid=(il+ir)*.5f;
    d->AddRectFilledMultiColor(ImVec2(il,k.marquee.max.y),ImVec2(mid,b.max.y),c.frame,c.panel,c.panel,c.frame);
    d->AddRectFilledMultiColor(ImVec2(mid,k.marquee.max.y),ImVec2(ir,b.max.y),c.panel,c.frame,c.frame,c.panel);
    // The marquee, lit from behind: brighter at its top, a lip under it.
    const v2::Box& m=k.marquee;
    d->AddRectFilledMultiColor(m.min,m.max,Mix(marqueeGround,c.text,.22f),Mix(marqueeGround,c.text,.22f),marqueeGround,marqueeGround);
    d->AddLine(ImVec2(m.min.x,m.min.y+.5f*s),ImVec2(m.max.x,m.min.y+.5f*s),v2::color::Alpha(c.text,.25f),s);
    d->AddRectFilled(ImVec2(m.min.x,m.max.y-(std::max)(1.f,1.5f*s)),m.max,c.ink);
    // Speaker grilles either side of the cabinet's head.
    if(k.detail){
     const float gy=(m.max.y+k.bezel.min.y)*.5f,step=(std::max)(3*s,w*.03f);
     for(int side=0;side<2;++side)for(int j=0;j<4;++j){
      const float x=side?ir-w*.12f-j*step:il+w*.12f+j*step;
      d->AddCircleFilled(ImVec2(x,gy),(std::max)(.8f*s,w*.008f),c.ink);
     }
    }
    // The bezel and the glass: a glow from the top in the screen's tone, and
    // scanlines over it where the screen is tall enough for them to read as such.
    d->AddRectFilled(k.bezel.min,k.bezel.max,c.ink,3*s);
    const v2::Box& g=k.screen;
    d->AddRectFilled(g.min,g.max,Mix(c.ink,glow,.05f*glowStrength),4*s);
    const v2::Box shine=v2::Inset(g,1.5f*s,1.5f*s);
    d->AddRectFilledMultiColor(shine.min,shine.max,v2::color::Alpha(glow,.2f*glowStrength),v2::color::Alpha(glow,.2f*glowStrength),
                               v2::color::Alpha(glow,.04f*glowStrength),v2::color::Alpha(glow,.04f*glowStrength));
    if(g.H()>40*s)for(float y=g.min.y+2*s;y<g.max.y-2*s;y+=3*s)d->AddLine(ImVec2(g.min.x+2*s,y),ImVec2(g.max.x-2*s,y),v2::color::Alpha(c.ink,.35f),(std::max)(1.f,.75f*s));
    d->AddRect(g.min,g.max,v2::color::Alpha(glow,.18f*glowStrength+.08f),4*s,0,s);
    d->AddLine(ImVec2(g.min.x+4*s,g.min.y+2*s),ImVec2(g.min.x+g.W()*.4f,g.min.y+2*s),v2::color::Alpha(c.text,.12f),s);
    // The name plate's ground; the board puts the player's tag on it.
    d->AddRectFilled(k.plate.min,k.plate.max,c.frame,2*s);
    // The control panel: its sloped top widening toward the player, the face under it.
    const v2::Box& p=k.panel;
    const float front=p.min.y+p.H()*.64f;
    d->AddQuadFilled(ImVec2(il,p.min.y),ImVec2(ir,p.min.y),ImVec2(r,front),ImVec2(l,front),c.button);
    d->AddLine(ImVec2(il,p.min.y),ImVec2(ir,p.min.y),v2::color::Alpha(c.text,.12f),s);
    d->AddRectFilled(ImVec2(l,front),p.max,c.frame);
    d->AddLine(ImVec2(l,front),ImVec2(r,front),v2::color::Alpha(edge,.6f),s);
    // The stick (a red ball top) and the six buttons in two rows of three, as a
    // Street Fighter panel has them; one row where the panel is small.
    const float top=front-p.min.y,cy=p.min.y+top*.52f;
    const float ball=(std::max)(2.5f*s,(std::min)(top*.24f,w*.06f));
    const ImVec2 stick(l+w*.26f,cy-top*.08f);
    d->AddEllipseFilled(ImVec2(stick.x,cy+top*.2f),ImVec2(ball*1.3f,ball*.55f),c.ink);
    d->AddLine(ImVec2(stick.x,cy+top*.2f),stick,c.muted,(std::max)(1.f,ball*.35f));
    d->AddCircleFilled(stick,ball,c.live);
    if(k.detail)d->AddCircleFilled(ImVec2(stick.x-ball*.35f,stick.y-ball*.35f),ball*.3f,v2::color::Alpha(c.text,.55f));
    const bool two=k.detail||top>=14*s;
    // The buttons keep clear of the panel's edges, as on a real cabinet.
    const float from=l+w*.44f,span=r-w*.2f-from,button=(std::max)(1.6f*s,(std::min)(top*(two?.14f:.18f),span/3*.3f));
    const float pitch=(std::min)(span/3,button*3);
    const ImU32 cap=seated?v2::color::Alpha(c.text,.85f):v2::color::Alpha(c.muted,.55f);
    for(int row=0;row<(two?2:1);++row)for(int j=0;j<3;++j){
     const float x=from+(span-3*pitch)*.5f+pitch*(j+.5f)+(row?button*.35f:0),y=two?cy+(row?button*1.15f:-button*1.15f)+(j==0?button*.3f:0):cy;
     if(k.detail)d->AddCircleFilled(ImVec2(x,y+.8f*s),button+.6f*s,c.ink);
     d->AddCircleFilled(ImVec2(x,y),button,cap);
    }
    // Under the panel: the coin door with its two lit slots, then the kick plate.
    const v2::Box& o=k.lower;
    const float kick=(std::max)(3*s,b.H()*.03f);
    if(k.detail&&o.H()>=14*s){
     const float dw=w*.36f,dh=(o.H()-kick)*.7f;
     const v2::Box door{ImVec2(mid-dw*.5f,o.min.y+(o.H()-kick-dh)*.4f),ImVec2(mid+dw*.5f,o.min.y+(o.H()-kick-dh)*.4f+dh)};
     d->AddRectFilled(door.min,door.max,c.frame,2*s);
     d->AddRect(door.min,door.max,c.border,2*s,0,s);
     const float sw=(std::max)(2*s,dw*.08f),sh=dh*.34f;
     for(int j=0;j<2;++j){
      const float x=door.min.x+dw*(j?.7f:.3f);
      d->AddRectFilled(ImVec2(x-sw*.5f,door.min.y+dh*.14f),ImVec2(x+sw*.5f,door.min.y+dh*.14f+sh),seated?c.live:v2::color::Alpha(c.live,.35f),s);
     }
    }
    d->AddRectFilled(ImVec2(il,b.max.y-kick),ImVec2(ir,b.max.y),c.ink);
    if(kickGlow)d->AddRectFilled(ImVec2(il,b.max.y-kick),ImVec2(ir,b.max.y),kickGlow);
    // The outline: marquee, body, the panel's ledge, body to the floor.
    const float pt=p.min.y,pf=front,pb=p.max.y;
    const ImVec2 outline[]={ImVec2(il,b.max.y),ImVec2(il,pb),ImVec2(l,pb),ImVec2(l,pf),ImVec2(il,pt),ImVec2(il,m.max.y),ImVec2(l,m.max.y),
        ImVec2(l,m.min.y),ImVec2(r,m.min.y),ImVec2(r,m.max.y),ImVec2(ir,m.max.y),ImVec2(ir,pt),ImVec2(r,pf),ImVec2(r,pb),ImVec2(ir,pb),
        ImVec2(ir,b.max.y)};
    d->AddPolyline(outline,static_cast<int>(sizeof outline/sizeof*outline),edge,0,(std::max)(1.f,1.5f*s));
}
}

float ApplicationShell::ArcadeBoardHeight(const ShellView& v,const std::vector<MenuEntry>& rows,float width) {
 const float s=Scale(),strip=MatchupStripHeight(MatchupCount(rows),s);
 // As DrawArcadeBoard lays it out: the banner, the matchups, the focus margin
 // round the rows, and a scrollbar under rows that scroll across.
 return (v.room.arcade==room::KingRoom?(TopSlotHeight+8)*s:0)+(strip>0?strip+8*s:0)+12*s+
  2*LeastCabinet*s*CabinetReach+Aisle*s+(RowsScrollAcross(width-12*s,s)?ImGui::GetStyle().ScrollbarSize:0);
}

void ApplicationShell::DrawArcadeBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,ImVec2 size,
                                       room::RoomType type,bool scrolled,bool reveal) {
 const float s=Scale();const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
 const auto focus=nav.Focus();
 const bool focusMoved=reveal;
 const ImVec2 origin=ImGui::GetCursorScreenPos();
 const v2::Box area{origin,ImVec2(origin.x+size.x,origin.y+size.y)};
 const ImU32 pending=ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)),gold=v2::TierColor(v2::Tier::Gold).stripe;
 // Where the focus arrived this frame, for the scroll that holds the board.
 std::optional<float> arrivedAt;
 // A row's box: a press is Select on it, the moving pointer focuses it, and
 // with `scroll` it scrolls into view as the focus arrives. True while hovered.
 const auto press=[&](const MenuEntry& e,const v2::Box& b,bool scroll){
  ReportMenuCard(e.id.c_str(),b.min,b.max);
  const bool arrives=scroll&&focusMoved;
  const bool hovered=PressEntry(nav,rows,e,b,action,arrives).hovered;
  if(arrives&&e.id==focus)ImGui::SetScrollHereX(.5f);
  if(focusMoved&&e.id==focus)arrivedAt=(b.min.y+b.max.y)*.5f;
  return hovered;
 };
 const auto row=[&](const std::string& id)->const MenuEntry*{
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  return found==rows.end()?nullptr:&*found;
 };
 const auto tag=[&](const room::Member& m,ImVec2 at,float width,v2::TagSize size,std::string status){
  v2::TagView g;g.name=m.name+(m.id==v.room.localMember?std::string(loc::T("room.suffix_you")):"");g.marks={m.country,m.badge};g.status=std::move(status);
  if(size!=v2::TagSize::XS)g.ping=LatencyMarkFor(v.memberLatency,m,v.room.localMember);
  return v2::Tag(d,at,width,g,size);
 };
 const FighterArt art=RoomFighterArt(preferences_);
 v2::Box board=area;
 if(type==room::KingRoom){
  // The top slot: the longest current streak, its holder and the streak.
  const v2::Box banner=v2::CutTop(board,TopSlotHeight*s,8*s);
  v2::CardGround(d,banner,false,false,true);
  v2::Icon(d,v2::IconCount-1,ImVec2(banner.min.x+14*s,banner.min.y+10*s),28*s,gold);
  float x=banner.min.x+58*s;
  x+=v2::SectionHeading(d,ImVec2(x,banner.min.y+16*s),loc::T("room.top_slot"),gold)+16*s;
  const int top=v.room.topSlot;
  const auto* holder=top>=0?room::FindMember(v.room,v.room.cabinets[top]):nullptr;
  if(holder){
   const std::string streak=loc::Tf("room.streak",v.room.streaks[top]);
   const float sw=v2::ChipWidth(streak);
   tag(*holder,ImVec2(x,banner.min.y+8*s),(std::max)(0.f,banner.max.x-26*s-sw-x),v2::TagSize::S,SeatCaption(SeatLookOf(v,*holder)));
   v2::Chip(d,ImVec2(banner.max.x-14*s-sw,banner.min.y+12*s),streak,gold);
  }else v2::Text(d,ImVec2(x,banner.min.y+15*s),banner.max.x-14*s-x,loc::T("room.top_slot_none"),15*s,c.muted);
 }
 // Real Arcade rows: two rows of eight tall cabinets, cabinet n on top and
 // n+8 under it.
 // Where they would be too small to read, they keep their size and scroll:
 // down under their least height, across under their least width (a narrow window).
 // Who plays whom, under the cabinets: a chip per pair in its colour. The one
 // under the pointer or focused lights its two cabinets and dims the rest.
 int lit=-1;
 // Each matchup row and the table its pair plays at.
 std::vector<std::pair<const MenuEntry*,int>> matchups;
 for(const auto& e:rows)if(const auto table=RowKey<int>(e.id,"matchup-"))matchups.push_back({&e,*table});
 const std::size_t per=MatchupsPerLine;
 const float stripHeight=MatchupStripHeight(matchups.size(),s);
 const auto drawMatchups=[&](v2::Box strip,bool scroll){
  const float w=(strip.W()-12*s-(per-1)*8*s)/per;
  for(std::size_t k=0;k<matchups.size();++k){
   const auto* e=matchups[k].first;const int table=matchups[k].second;
   const std::string text=e->label+(e->value.empty()?std::string():"  "+e->value);
   const float x=strip.min.x+6*s+(k%per)*(w+8*s),y=strip.min.y+4*s+(k/per)*MatchupLine*s;
   const v2::Box chip{ImVec2(x,y),ImVec2(x+w,y+26*s)};
   const bool hovered=press(*e,chip,scroll),focused=e->id==focus;
   if(hovered||focused)lit=table;
   v2::RowGround(d,chip,focused,hovered);
   d->AddCircleFilled(ImVec2(chip.min.x+14*s,(chip.min.y+chip.max.y)*.5f),5*s,PairColor(table));
   v2::Text(d,ImVec2(chip.min.x+26*s,chip.min.y+(chip.H()-13*s)*.5f),w-32*s,text,13*s,hovered||focused?c.text:c.muted,nullptr,false,!e->userText);
   if(focused)v2::Focus(d,chip.min,chip.max,false);
  }
 };
 const float gap=CabinetGap*s,band=Aisle*s,least=LeastCabinet*s;
 // On a board too short for even one row of cabinets over the matchups (a
 // narrow window), the matchups scroll with the rows, under them.
 const bool stacked=!matchups.empty()&&board.H()-stripHeight-8*s-12*s<least*CabinetReach;
 if(!matchups.empty()&&!stacked)drawMatchups(v2::CutBottom(board,stripHeight,8*s),false);
 // A margin for the focus and hover marks, which reach 4 px past a cabinet.
 board=v2::Inset(board,6*s,6*s);
 const float shown=board.W();
 // Rows that scroll across keep their scrollbar's height out of the cabinets'.
 const bool sideways=RowsScrollAcross(board.W(),s);
 const float cell=CabinetCell(board.W(),s),across=8*cell+7*gap,scrollbar=sideways?ImGui::GetStyle().ScrollbarSize:0;
 const float h=(std::max)(least,(std::min)((board.H()-band-scrollbar)/2/CabinetReach,240*s)),w=(std::min)(cell,h*.8f),unit=h*CabinetReach;
 const bool scrolls=stacked||2*unit+band+scrollbar>board.H()+.5f||sideways;
 if(scrolls){
  ImGui::SetCursorScreenPos(board.min);
  ImGui::BeginChild("Arcade rows",ImVec2(board.W(),(std::max)(board.H(),1.f)),0,ImGuiWindowFlags_NoNavInputs|(sideways?ImGuiWindowFlags_HorizontalScrollbar:0));
  d=ImGui::GetWindowDrawList();
  const ImVec2 inner=ImGui::GetCursorScreenPos();board={inner,ImVec2(inner.x+(std::max)(board.W(),across),inner.y+2*unit+band)};
 }
 if(stacked)drawMatchups(v2::Box{ImVec2(board.min.x-6*s,board.max.y+8*s),ImVec2(board.min.x-6*s+shown+12*s,board.max.y+8*s+stripHeight)},true);
 const float top=board.min.y+(std::max)(0.f,(board.H()-2*unit-band)*.5f),aisle=top+unit+band*.5f;
 const auto at=[&](int cabinet){return ImVec2(board.min.x+(cabinet%8)*(cell+gap)+(cell-w)*.5f,top+(cabinet/8)*(unit+band));};
 // A line joins each pair's cabinets along the aisle between the rows, each
 // pair at its own height there, so it never crosses another cabinet.
 for(const auto& t:v.room.tables){
  const int a=room::CabinetOf(v.room,t.p1),b=room::CabinetOf(v.room,t.p2);
  if(!t.p1||!t.p2||a<0||b<0)continue;
  const float y=aisle+(t.id-3.5f)*3*s;
  const auto stub=[&](int cabinet){const ImVec2 p=at(cabinet);return ImVec2(p.x+w*.5f,cabinet<8?p.y+unit:p.y);};
  const ImVec2 points[]={stub(a),ImVec2(stub(a).x,y),ImVec2(stub(b).x,y),stub(b)};
  d->AddPolyline(points,4,PairColor(t.id),0,2*s);
 }
 for(int i=0;i<static_cast<int>(room::CabinetCount);++i){
  const auto* e=row("cabinet-"+std::to_string(i));
  if(!e)continue;
  const ImVec2 p=at(i);
  const v2::Box b{p,ImVec2(p.x+w,p.y+h)};
  const v2::Box spot{p,ImVec2(p.x+w,p.y+unit)};
  // A cabinet scrolls into view when the focus moves to it, so the wheel
  // scrolls the rows between moves.
  const bool hovered=press(*e,spot,scrolls),focused=e->id==focus;
  if(focused&&nav.Choosing())menu_.bodyChoices={spot,area};
  const auto* m=room::FindMember(v.room,v.room.cabinets[i]);
  const auto* t=m&&m->table>=0?&v.room.tables[m->table]:nullptr;
  const bool paired=t&&t->p1&&t->p2,seated=m!=nullptr;const ImU32 pair=paired?PairColor(t->id):pending;
  const bool crowned=type==room::KingRoom&&v.room.topSlot==i;
  // Every cabinet glows: dim while free, brighter once someone sits there, in the
  // pair's colour while a pair plays; a little more under the pointer.
  const bool waiting=seated&&!paired;
  const bool picked=lit>=0&&paired&&static_cast<int>(t->id)==lit;
  const float inset=(std::max)(2*s,w*.045f);
  v2::Glow(d,ImVec2(b.min.x+inset,b.min.y),ImVec2(b.max.x-inset,b.max.y),paired?pair:c.text,(picked?.9f:paired?.45f:waiting?.3f:.08f)+(hovered?.1f:0),(picked?10:6)*s,2*s);
  // The screen: the fighter and the score while a pair plays, else Waiting,
  // the Rematch choice, or Insert coin.
  const int side=t&&t->p2==m->id?1:0;
  std::string first=loc::T("room.cabinet_free"),second;ImU32 tone=c.muted;
  if(m&&!paired){first=loc::T("room.arcade_waiting");tone=c.text;}
  else if(paired&&t->deciding){first=loc::T(t->rematch[side]?"room.rematch_chosen":"room.rematch_ask");tone=pending;}
  // Who plays whom: the opponent's name on the screen, in the pair's colour, over the score.
  else if(paired){const auto* other=room::FindMember(v.room,side?t->p1:t->p2);
   if(other)NoteUserText(other->name);first=loc::Tf("room.arcade_versus",other?other->name:std::string());second=SetScoreText(t->score).empty()?SeatCaption(SeatLookOf(v,*m),true):SetScoreText(t->score);tone=pair;}
  ImU32 secondTone=paired&&!t->deciding?pair:tone;
  // What a table card's banner says about this seat (a held start, the Ready
  // timeout, a Training call) takes the screen's second line.
  if(t)if(const auto banner=DescribeTableBanner(v,*t);!banner.text.empty()&&(banner.seat<0||banner.seat==side)){second=banner.text;secondTone=pending;}
  // The shell: marquee lit in the pair's colour (gold on the top slot), the
  // glass glowing in the screen's tone, the outline in the marquee's colour.
  const Cabinet k=LayOutCabinet(b,s);
  const ImU32 edge=crowned?gold:paired?pair:waiting?v2::color::Alpha(pending,.55f):c.border;
  const ImU32 glass=paired&&!t->deciding?pair:seated?pending:c.muted;
  DrawCabinet(d,b,k,s,crowned?gold:paired?pair:waiting?c.hover:c.button,glass,!seated?.45f:1.f,edge,seated,paired?v2::color::Alpha(pair,.25f):0);
  // The marquee: the number; in King of the hill the streak beside a crown,
  // gold on the top slot.
  const v2::Box& marquee=k.marquee;
  const float mh=marquee.H(),ms=(std::min)(15*s,mh*.68f);const ImU32 ink=paired||crowned?c.ink:waiting?c.text:c.muted;
  const int streak=type==room::KingRoom?v.room.streaks[i]:0;
  v2::Text(d,ImVec2(marquee.min.x+4*s,marquee.min.y+(mh-ms)*.5f),streak?marquee.W()*.45f:marquee.W()-8*s,std::to_string(i+1),ms,ink,HeadingFont(),!streak);
  if(streak){
   const std::string count=std::to_string(streak);
   const float cw=HeadingFont()->CalcTextSizeA(ms,FLT_MAX,0,count.c_str()).x;
   v2::Text(d,ImVec2(marquee.max.x-4*s-cw,marquee.min.y+(mh-ms)*.5f),cw,count,ms,ink,HeadingFont());
   v2::Icon(d,v2::IconCount-1,ImVec2(marquee.max.x-8*s-cw-ms,marquee.min.y+(mh-ms)*.5f),ms,ink);
  }
  const v2::Box& screen=k.screen;
  // A seated player's fighter fills the glass (Settings › Room layout ›
  // Pictures), standing on its foot, under the screen's scanlines; the
  // screen's words go to its foot, over a shade.
  const bool pictured=m&&DrawCabinetFighter(d,screen,SeatPicture(SeatLookOf(v,*m),art),glass,s);
  const float ss=(std::min)(15*s,screen.H()*.34f);
  const auto space=first.find(' ');
  if(second.empty()&&space!=std::string::npos&&HeadingFont()->CalcTextSizeA(ss,FLT_MAX,0,first.c_str()).x>screen.W()-4*s){second=first.substr(space+1);first.resize(space);}
  // Each line at the size that fits the screen's width, down to 7 px; past
  // that (a banner's sentence) it is cut, and the row's words carry it whole.
  const float room=screen.W()-4*s;
  const auto fit=[&](const std::string& line){const float w=HeadingFont()->CalcTextSizeA(ss,FLT_MAX,0,line.c_str()).x;return w>room-s?(std::max)(7*s,ss*(room-s)/w):ss;};
  // Where the lines meet: the screen's middle, or over a picture its foot.
  const float mid=!pictured?(screen.min.y+screen.max.y)*.5f:screen.max.y-3*s-(second.empty()?fit(first)*.5f:fit(second)+1*s);
  v2::Text(d,ImVec2(screen.min.x+2*s,second.empty()?mid-fit(first)*.5f:mid-fit(first)-1*s),room,first,fit(first),tone,HeadingFont(),true,!paired);
  if(!second.empty())v2::Text(d,ImVec2(screen.min.x+2*s,mid+1*s),room,second,fit(second),secondTone,HeadingFont(),true,!paired);
  // The name plate: the player's tag, its edge in the pair's colour. A small
  // cabinet's plate gives the name the flag's room (Members still shows it).
  const v2::Box& plate=k.plate;
  if(paired)d->AddRect(plate.min,plate.max,pair,2*s,0,s);
  if(m){
   v2::TagView g;g.name=m->name;g.marks={k.detail?m->country:CountryCode(),{}};
   v2::Tag(d,ImVec2(plate.min.x+2*s,plate.min.y+(plate.H()-ImGui::GetFontSize())*.5f-2*s),plate.W()-4*s,g,v2::TagSize::XS);
  }
  if(focused)v2::Focus(d,ImVec2(b.min.x-4*s,b.min.y-4*s),ImVec2(b.max.x+4*s,p.y+unit),false);
  // While a matchup is picked below, the other cabinets step back.
  if(lit>=0&&!picked)d->AddRectFilled(ImVec2(b.min.x-2*s,b.min.y-2*s),ImVec2(b.max.x+2*s,p.y+unit),v2::color::Alpha(c.ink,.6f),2*s);
 }
 if(scrolls){ImGui::SetCursorScreenPos(board.min);ImGui::Dummy(ImVec2(board.W(),board.H()+(stacked?8*s+stripHeight:0)));ImGui::EndChild();}
 // The scroll holding the board brings the row the focus arrived on into view.
 if(scrolled&&arrivedAt)ImGui::SetScrollFromPosY(*arrivedAt-ImGui::GetWindowPos().y,.5f);
 ImGui::SetCursorScreenPos(origin);ImGui::Dummy(size);
}

} }
