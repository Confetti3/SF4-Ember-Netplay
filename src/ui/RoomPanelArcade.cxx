// The V2 room board's tables as the room's type lays them out
// (Snapshot::arcade, Arcade.dc.html): a Classic room's tables as the floor of
// facing pairs; an Arcade room's sixteen cabinets in two rows, each pair in its
// own colour and linked by a line wherever its two cabinets stand; King of the
// hill the same under its top slot. Every cabinet and table is its row.
#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "LatencyMark.hxx"
#include "RoomControls.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui {
using namespace room_controls;
namespace {
// Each pair's own colour (Arcade.dc.html), lit on both of its cabinets: content, like the tiers' colours.
ImU32 PairColor(std::size_t table) {
    static const ImU32 ember[]={0,IM_COL32(127,176,224,255),IM_COL32(201,160,224,255),IM_COL32(164,206,160,255),
        IM_COL32(110,204,200,255),IM_COL32(233,160,112,255),IM_COL32(213,220,227,255),IM_COL32(236,128,176,255)};
    const ImU32 c=ember[table%8];
    return c?c:v2::Colors().accent;
}
}

void ApplicationShell::DrawArcadeBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,ImVec2 size,
                                       room::RoomType type) {
 const float s=Scale();const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
 const auto focus=nav.Focus();
 const ImVec2 origin=ImGui::GetCursorScreenPos();
 const v2::Box area{origin,ImVec2(origin.x+size.x,origin.y+size.y)};
 const ImU32 pending=ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)),gold=v2::TierColor(v2::Tier::Gold).stripe;
 const auto row=[&](const std::string& id)->const MenuEntry*{
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  return found==rows.end()?nullptr:&*found;
 };
 // A row's box: a press is Select, the moving pointer focuses it, and the
 // focused one's choices open beside it. True while hovered.
 const auto press=[&](const MenuEntry& e,const v2::Box& b){
  const bool hovered=PressEntry(nav,rows,e,b,action).hovered;
  if(e.id==focus&&nav.Choosing())menu_.bodyChoices={b,area};
  return hovered;
 };
 const auto tag=[&](const room::Member& m,ImVec2 at,float width,v2::TagSize size,std::string status){
  v2::TagView g;g.name=m.name+(m.id==v.room.localMember?std::string(loc::T("room.suffix_you")):"");g.marks={m.country,m.badge};g.status=std::move(status);
  if(size!=v2::TagSize::XS)g.ping=LatencyMarkFor(v.memberLatency,m,v.room.localMember);
  return v2::Tag(d,at,width,g,size);
 };
 const auto fighterName=[&](const room::Member& m)->std::string{
  const auto* f=selection::FindFighter(m.id==v.room.localMember?v.selectedFighter:m.fighter);
  return f?f->name:loc::T("room.fighter_not_shared");
 };
 if(!type){
  // Classic: a line for each table, its two seats facing across the score.
  const std::size_t n=room::TablesInPlay(v.room);
  const float gap=8*s,mid=96*s,line=(std::min)(84*s,(area.H()-(n-1)*gap)/n);
  for(std::size_t i=0;i<n;++i){
   const auto& t=v.room.tables[i];const auto* e=row("table-"+std::to_string(i));
   if(!e)continue;
   const v2::Box b{ImVec2(area.min.x,area.min.y+i*(line+gap)),ImVec2(area.max.x,area.min.y+i*(line+gap)+line)};
   const bool hovered=press(*e,b),focused=e->id==focus;const ImU32 pair=PairColor(i);
   const float half=(b.W()-mid)*.5f;
   const auto banner=DescribeTableBanner(v,t);
   const room::MemberId seats[]={t.p1,t.p2};
   for(int side=0;side<2;++side){
    const v2::Box h{ImVec2(b.min.x+side*(half+mid),b.min.y),ImVec2(b.min.x+side*(half+mid)+half,b.max.y)};
    v2::CardGround(d,h,focused,hovered);
    const v2::Box in=v2::Inset(h,12*s,8*s);
    const auto* m=room::FindMember(v.room,seats[side]);
    if(!m){v2::Text(d,ImVec2(in.min.x,in.min.y+(in.H()-15*s)*.5f),in.W(),loc::T("room.open_seat"),15*s,c.muted);continue;}
    d->AddRect(h.min,h.max,pair,4*s,0,s);
    tag(*m,in.min,in.W(),v2::TagSize::S,fighterName(*m));
    const bool about=!banner.text.empty()&&(banner.seat<0||banner.seat==side);
    const std::string words=about?banner.text:t.ready[side]?std::string(loc::T("room.status.ready")):std::string(PhaseName(t));
    v2::Text(d,ImVec2(in.min.x,in.max.y-14*s),in.W(),words,13*s,about?pending:c.muted);
   }
   const float x=b.min.x+half;
   v2::Text(d,ImVec2(x,b.min.y+10*s),mid,loc::Tf("room.v2.table",i+1),12*s,c.muted,HeadingFont(),true);
   if(t.p1||t.p2){
    const std::string score=t.p1&&t.p2&&!SetScoreText(t.score).empty()?SetScoreText(t.score):"VS";
    v2::Chip(d,ImVec2(x+(mid-v2::ChipWidth(score))*.5f,b.min.y+line*.5f-4*s),score,pair);
   }
   if(focused)v2::Focus(d,b.min,b.max);
  }
  ImGui::SetCursorScreenPos(origin);ImGui::Dummy(size);
  return;
 }
 v2::Box board=area;
 if(type==room::KingRoom){
  // The top slot: the longest current streak, its holder and the streak.
  const v2::Box banner=v2::CutTop(board,56*s,12*s);
  v2::CardGround(d,banner,false,false,true);
  v2::Icon(d,v2::IconCount-1,ImVec2(banner.min.x+14*s,banner.min.y+12*s),32*s,gold);
  float x=banner.min.x+58*s;
  x+=v2::SectionHeading(d,ImVec2(x,banner.min.y+20*s),loc::T("room.top_slot"),gold)+16*s;
  const int top=v.room.topSlot;
  const auto* holder=top>=0?room::FindMember(v.room,v.room.cabinets[top]):nullptr;
  if(holder){
   const std::string streak=loc::Tf("room.streak",v.room.streaks[top]);
   const float sw=v2::ChipWidth(streak);
   tag(*holder,ImVec2(x,banner.min.y+12*s),(std::max)(0.f,banner.max.x-26*s-sw-x),v2::TagSize::S,fighterName(*holder));
   v2::Chip(d,ImVec2(banner.max.x-14*s-sw,banner.min.y+16*s),streak,gold);
  }else v2::Text(d,ImVec2(x,banner.min.y+19*s),banner.max.x-14*s-x,loc::T("room.top_slot_none"),15*s,c.muted);
 }
 // Real Arcade rows: two rows of eight tall cabinets, each over its stool
 // with the player's back on it; cabinet n on top, n+8 under it.
 // Where they would be too small to read, they keep their size and scroll.
 // Who plays whom, under the cabinets: a chip per pair in its colour. The one
 // under the pointer or focused lights its two cabinets and dims the rest.
 int lit=-1;
 std::vector<const MenuEntry*> matchups;
 for(const auto& e:rows)if(e.id.compare(0,8,"matchup-")==0)matchups.push_back(&e);
 if(!matchups.empty()){
  const v2::Box strip=v2::CutBottom(board,40*s,8*s);
  float x=strip.min.x+6*s;
  for(const auto* e:matchups){
   const int table=std::stoi(e->id.substr(8));
   const std::string text=e->label+(e->value.empty()?std::string():"  "+e->value);
   const float w=(std::min)(strip.max.x-6*s-x,ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,0,text.c_str()).x+40*s);
   if(w<80*s)break;
   const v2::Box chip{ImVec2(x,strip.min.y+4*s),ImVec2(x+w,strip.max.y-4*s)};
   const bool hovered=PressEntry(nav,rows,*e,chip,action).hovered,focused=e->id==focus;
   if(hovered||focused)lit=table;
   v2::RowGround(d,chip,focused,hovered);
   d->AddCircleFilled(ImVec2(chip.min.x+14*s,(chip.min.y+chip.max.y)*.5f),5*s,PairColor(table));
   v2::Text(d,ImVec2(chip.min.x+26*s,chip.min.y+(chip.H()-14*s)*.5f),w-32*s,text,14*s,hovered||focused?c.text:c.muted);
   if(focused)v2::Focus(d,chip.min,chip.max,false);
   x+=w+8*s;
  }
 }
 // A margin for the focus and hover marks, which reach 4 px past a cabinet.
 board=v2::Inset(board,6*s,6*s);
 const float gap=8*s,band=36*s,cell=(board.W()-7*gap)/8;
 const float h=(std::max)(120*s,(std::min)((board.H()-band)/2/1.4f,240*s)),w=(std::min)(cell,h*.75f),unit=h*1.4f;
 const bool scrolls=2*unit+band>board.H();
 if(scrolls){
  ImGui::SetCursorScreenPos(board.min);
  ImGui::BeginChild("Arcade rows",ImVec2(board.W(),board.H()),0,ImGuiWindowFlags_NoNavInputs);
  d=ImGui::GetWindowDrawList();
  const ImVec2 inner=ImGui::GetCursorScreenPos();board={inner,ImVec2(inner.x+board.W(),inner.y+2*unit+band)};
 }
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
  const bool hovered=PressEntry(nav,rows,*e,spot,action,scrolls).hovered,focused=e->id==focus;
  if(focused&&nav.Choosing())menu_.bodyChoices={spot,area};
  const auto* m=room::FindMember(v.room,v.room.cabinets[i]);
  const auto* t=m&&m->table>=0?&v.room.tables[m->table]:nullptr;
  const bool paired=t&&t->p1&&t->p2,lit=m!=nullptr;const ImU32 pair=paired?PairColor(t->id):pending;
  const bool crowned=type==room::KingRoom&&v.room.topSlot==i;
  // Every cabinet glows: dim while free, brighter once someone sits there, in the
  // pair's colour while a pair plays; a little more under the pointer.
  const bool waiting=lit&&!paired;
  const bool picked=lit>=0&&paired&&static_cast<int>(t->id)==lit;
  v2::Glow(d,b.min,b.max,paired?pair:c.text,(picked?.9f:paired?.45f:waiting?.3f:.08f)+(hovered?.1f:0),(picked?10:6)*s,6*s);
  d->AddRectFilled(b.min,b.max,c.ink,6*s,ImDrawFlags_RoundCornersTop);
  // From the top: marquee, screen, name plate, control panel, kick plate.
  v2::Box body=b;
  const v2::Box marquee=v2::CutTop(body,(std::max)(16*s,h*.12f)),kick=v2::CutBottom(body,h*.06f);
  v2::CutBottom(body,4*s);
  const v2::Box panel=v2::Inset(v2::CutBottom(body,h*.1f),5*s,0),plate=v2::Inset(v2::CutBottom(body,22*s),3*s,0);
  v2::CutTop(body,4*s);
  const v2::Box screen=v2::Inset(body,5*s,0);
  // The marquee: the number, in the pair's colour; in King of the hill the
  // streak beside a crown, gold on the top slot.
  d->AddRectFilled(marquee.min,marquee.max,crowned?gold:paired?pair:c.button,6*s,ImDrawFlags_RoundCornersTop);
  const float mh=marquee.H(),ms=(std::min)(11*s,mh*.75f);const ImU32 ink=paired||crowned?c.ink:waiting?c.text:c.muted;
  const int streak=type==room::KingRoom?v.room.streaks[i]:0;
  v2::Text(d,ImVec2(marquee.min.x+4*s,marquee.min.y+(mh-ms)*.5f),streak?marquee.W()*.45f:marquee.W()-8*s,std::to_string(i+1),ms,ink,HeadingFont(),!streak);
  if(streak){
   const std::string count=std::to_string(streak);
   const float cw=HeadingFont()->CalcTextSizeA(ms,FLT_MAX,0,count.c_str()).x;
   v2::Text(d,ImVec2(marquee.max.x-4*s-cw,marquee.min.y+(mh-ms)*.5f),cw,count,ms,ink,HeadingFont());
   v2::Icon(d,v2::IconCount-1,ImVec2(marquee.max.x-8*s-cw-ms,marquee.min.y+(mh-ms)*.5f),ms,ink);
  }
  // The screen: the fighter and the score while a pair plays, else Waiting,
  // the Rematch choice, or Insert coin.
  const int side=t&&t->p2==m->id?1:0;
  std::string first=loc::T("room.cabinet_free"),second;ImU32 tone=c.muted;
  if(m&&!paired){first=loc::T("room.arcade_waiting");tone=c.text;}
  else if(paired&&t->deciding){first=loc::T(t->rematch[side]?"room.rematch_chosen":"room.rematch_ask");tone=pending;}
  // Who plays whom: the opponent's name on the screen, in the pair's colour, over the score.
  else if(paired){const auto* other=room::FindMember(v.room,side?t->p1:t->p2);
   if(other)NoteUserText(other->name);first=loc::Tf("room.arcade_versus",other?other->name:std::string());second=SetScoreText(t->score).empty()?fighterName(*m):SetScoreText(t->score);tone=pair;}
  d->AddRectFilled(screen.min,screen.max,!m?c.frame:paired&&!t->deciding?c.header:v2::color::Alpha(pending,.12f),2*s);
  const float ss=(std::min)(11*s,screen.H()*.3f),mid=(screen.min.y+screen.max.y)*.5f;
  const auto space=first.find(' ');
  if(second.empty()&&space!=std::string::npos&&HeadingFont()->CalcTextSizeA(ss,FLT_MAX,0,first.c_str()).x>screen.W()-4*s){second=first.substr(space+1);first.resize(space);}
  v2::Text(d,ImVec2(screen.min.x+2*s,second.empty()?mid-ss*.5f:mid-ss-1*s),screen.W()-4*s,first,ss,tone,HeadingFont(),true);
  if(!second.empty())v2::Text(d,ImVec2(screen.min.x+2*s,mid+1*s),screen.W()-4*s,second,ss,paired&&!t->deciding?pair:tone,HeadingFont(),true);
  // The name plate: the player's tag, its edge in the pair's colour.
  d->AddRectFilled(plate.min,plate.max,c.frame);
  if(paired)d->AddRect(plate.min,plate.max,pair,0,0,s);
  if(m){
   v2::TagView g;g.name=m->name;g.marks={m->country,{}};
   v2::Tag(d,ImVec2(plate.min.x+2*s,plate.min.y+(plate.H()-ImGui::GetFontSize())*.5f-2*s),plate.W()-4*s,g,v2::TagSize::XS);
  }
  d->AddRectFilled(panel.min,panel.max,c.button);
  const ImVec2 knob((panel.min.x+panel.max.x)*.5f,(panel.min.y+panel.max.y)*.5f);
  d->AddCircleFilled(ImVec2(knob.x-12*s,knob.y),3.5f*s,c.live);
  for(int k=0;k<3;++k)d->AddCircleFilled(ImVec2(knob.x-2*s+k*7*s,knob.y),3*s,c.text);
  if(paired)d->AddRectFilled(kick.min,kick.max,v2::color::Alpha(pair,.2f));
  d->AddRect(b.min,b.max,crowned?gold:paired?pair:c.border,6*s,ImDrawFlags_RoundCornersTop,2*s);
  if(focused)v2::Focus(d,ImVec2(b.min.x-4*s,b.min.y-4*s),ImVec2(b.max.x+4*s,p.y+unit),false);
  // While a matchup is picked below, the other cabinets step back.
  if(lit>=0&&!picked)d->AddRectFilled(ImVec2(b.min.x-2*s,b.min.y-2*s),ImVec2(b.max.x+2*s,b.max.y+2*s),v2::color::Alpha(c.ink,.6f),6*s);
  // The stool, and on it the player's back (a 60 by 70 drawing).
  const float k=h*.38f/60;const ImVec2 o(b.min.x+(w-60*k)*.5f,b.max.y-h*.04f);
  const auto q=[&](float x,float y){return ImVec2(o.x+x*k,o.y+y*k);};
  d->AddRectFilled(q(12,52),q(48,57),paired?pair:c.border,2*k);
  d->AddLine(q(17,57),q(13,70),c.hover,2.5f*k);d->AddLine(q(43,57),q(47,70),c.hover,2.5f*k);d->AddLine(q(15,65),q(45,65),c.hover,2.5f*k);
  if(!m)continue;
  // ponytail: an Ash jacket for everyone; the title's tier once members carry titles.
  const auto jacket=v2::TierColor(v2::Tier::Ash);
  const ImVec2 back[]={q(14,54),q(15,40),q(18,29),q(22,22),q(38,22),q(42,29),q(45,40),q(46,54)};
  d->AddConvexPolyFilled(back,8,jacket.plate);d->AddPolyline(back,8,jacket.stripe,ImDrawFlags_Closed,1.5f*k);
  d->AddLine(q(30,23),q(30,53),v2::color::Alpha(jacket.stripe,.55f),2.5f*k);
  d->AddRectFilled(q(26,16),q(34,23),c.muted,2*k);
  d->AddCircleFilled(q(30,11),8.75f*k,c.button);
 }
 if(scrolls){ImGui::SetCursorScreenPos(board.min);ImGui::Dummy(ImVec2(board.W(),board.H()));ImGui::EndChild();}
 ImGui::SetCursorScreenPos(origin);ImGui::Dummy(size);
}

} }
