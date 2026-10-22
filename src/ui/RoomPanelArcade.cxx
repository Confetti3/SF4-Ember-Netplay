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

namespace sf4e { namespace ui {
using namespace room_controls;
namespace {
// Each pair's own colour (Arcade.dc.html), lit on both of its cabinets: content, like the tiers' colours.
ImU32 PairColor(std::size_t table) {
    static const ImU32 ember[]={0,IM_COL32(127,176,224,255),IM_COL32(201,160,224,255),IM_COL32(164,206,160,255),
        IM_COL32(255,170,160,255),IM_COL32(233,160,112,255),IM_COL32(213,220,227,255),IM_COL32(241,196,119,255)};
    static const ImU32 bluMint[]={0,IM_COL32(91,140,255,255),0,IM_COL32(154,123,255,255)};
    const ImU32 c=v2::CurrentBrand()==v2::Brand::BluMint?bluMint[table%4]:ember[table%8];
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
 // Arcade: two rows of eight cabinets, cabinet n on top facing n+8 below it;
 // where eight leave a name no room, each row takes two lines of four.
 const float gap=8*s;
 const int across=(board.W()-7*gap)/8>=150*s?8:4,lines=static_cast<int>(room::CabinetCount)/across;
 const float aisle=(across==8?28:20)*s;
 const float w=(board.W()-(across-1)*gap)/across,h=(std::min)(190*s,(board.H()-(lines-1)*aisle)/lines);
 const auto boxOf=[&](int cabinet){
  const ImVec2 at(board.min.x+(cabinet%across)*(w+gap),board.min.y+(cabinet/across)*(h+aisle));
  return v2::Box{at,ImVec2(at.x+w,at.y+h)};
 };
 // A line joins each pair's cabinets wherever they stand: through the aisle
 // under a row (over the last row), or across the aisle between two rows,
 // under the cabinets.
 // ponytail: on four rows a pair two lines apart is joined behind the line between.
 for(const auto& t:v.room.tables){
  int a=room::CabinetOf(v.room,t.p1),b=room::CabinetOf(v.room,t.p2);
  if(!t.p1||!t.p2||a<0||b<0)continue;
  if(a/across>b/across)std::swap(a,b);
  const v2::Box A=boxOf(a),B=boxOf(b);const ImU32 pair=PairColor(t.id);
  if(a/across==b/across){
   const bool last=a/across==lines-1;const float dip=(last?-1:1)*aisle*.75f;
   const ImVec2 p((A.min.x+A.max.x)*.5f,last?A.min.y:A.max.y),q((B.min.x+B.max.x)*.5f,last?B.min.y:B.max.y);
   d->AddBezierCubic(p,ImVec2(p.x,p.y+dip),ImVec2(q.x,q.y+dip),q,pair,2*s);
  }else{
   const ImVec2 p((A.min.x+A.max.x)*.5f,A.max.y),q((B.min.x+B.max.x)*.5f,B.min.y);
   d->AddBezierCubic(p,ImVec2(p.x,p.y+aisle*.6f),ImVec2(q.x,q.y-aisle*.6f),q,pair,2*s);
  }
 }
 for(int i=0;i<static_cast<int>(room::CabinetCount);++i){
  const auto* e=row("cabinet-"+std::to_string(i));
  if(!e)continue;
  const v2::Box b=boxOf(i);
  const bool hovered=press(*e,b),focused=e->id==focus;
  const auto* m=room::FindMember(v.room,v.room.cabinets[i]);
  const auto* t=m&&m->table>=0?&v.room.tables[m->table]:nullptr;
  const bool paired=t&&t->p1&&t->p2;const ImU32 pair=t?PairColor(t->id):c.border;
  v2::CardGround(d,b,focused,hovered,m&&m->id==v.room.localMember);
  if(paired)d->AddRect(b.min,b.max,pair,4*s,0,2*s);
  const v2::Box in=v2::Inset(b,8*s,8*s);
  // The number; the crown and the streak in King of the hill, gold on the top slot.
  v2::Text(d,in.min,24*s,std::to_string(i+1),13*s,paired?pair:c.muted,HeadingFont());
  float right=in.max.x;
  if(type==room::KingRoom&&v.room.streaks[i]){
   const bool crowned=v.room.topSlot==i;
   const std::string streak=std::to_string(v.room.streaks[i]);
   const float sw=HeadingFont()->CalcTextSizeA(13*s,FLT_MAX,0,streak.c_str()).x;
   v2::Text(d,ImVec2(in.max.x-sw,in.min.y),sw,streak,13*s,crowned?gold:c.muted,HeadingFont());
   v2::Icon(d,v2::IconCount-1,ImVec2(in.max.x-sw-18*s,in.min.y),14*s,crowned?gold:c.muted);
   if(crowned)d->AddRect(ImVec2(b.min.x-2*s,b.min.y-2*s),ImVec2(b.max.x+2*s,b.max.y+2*s),gold,6*s,0,2*s);
   right-=sw+24*s;
  }
  if(!m){
   v2::Text(d,ImVec2(in.min.x,in.min.y+(in.H()-13*s)*.5f),in.W(),loc::T("room.cabinet_open"),13*s,c.muted,HeadingFont(),true);
  }else{
   // Waiting, the score (VS before the first game), or the Rematch choice,
   // on the number's line; the player under it, and their fighter if it fits.
   const int side=t&&t->p2==m->id?1:0;
   std::string state=loc::T("room.arcade_waiting");ImU32 tone=pending;
   if(paired&&t->deciding){state=loc::T(t->rematch[side]?"room.rematch_chosen":"room.rematch_ask");}
   else if(paired){state=SetScoreText(t->score).empty()?"VS":SetScoreText(t->score);tone=pair;}
   const bool below=right-in.min.x-28*s<v2::ChipWidth(state);
   if(!below)v2::Chip(d,ImVec2(right-v2::ChipWidth(state),in.min.y-3*s),state,tone);
   float y=in.min.y+24*s;
   y+=tag(*m,ImVec2(in.min.x,y),in.W(),v2::TagSize::XS,std::string())+4*s;
   if(y+16*s<=in.max.y-(below?26*s:0))v2::Text(d,ImVec2(in.min.x,y),in.W(),fighterName(*m),13*s,c.muted);
   if(below)v2::Chip(d,ImVec2(in.min.x,in.max.y-22*s),v2::Fit(state,in.W()-16*s,13*s),tone);
  }
  if(focused)v2::Focus(d,b.min,b.max);
 }
 ImGui::SetCursorScreenPos(origin);ImGui::Dummy(size);
}

} }
