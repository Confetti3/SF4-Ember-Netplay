// An Arcade room's board (Snapshot::arcade): its sixteen cabinets in the
// room's view, in place of the Classic room's table cards (DrawRoomBoard).
#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "RoomCards.hxx"
#include "V2.hxx"
#include "LatencyMark.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
namespace {
// Each pair's own colour (Arcade.dc.html), lit while anyone sits there: content, like the tiers' colours.
ImU32 PairColor(std::size_t table) {
    static const ImU32 ember[]={0,IM_COL32(241,196,119,255),IM_COL32(164,206,160,255),IM_COL32(127,176,224,255),IM_COL32(201,160,224,255),
        IM_COL32(255,170,160,255),IM_COL32(233,160,112,255),IM_COL32(213,220,227,255)};
    static const ImU32 bluMint[]={0,IM_COL32(91,140,255,255),0,IM_COL32(154,123,255,255)};
    const ImU32 c=v2::CurrentBrand()==v2::Brand::BluMint?bluMint[table%4]:ember[table%8];
    return c?c:v2::Colors().accent;
}
}

void ApplicationShell::DrawArcadeBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,ImVec2 size,
                                       room::ArcadeView view) {
 const float s=Scale();
 const auto focus=nav.Focus();const bool changed=focus!=roomBoardFocus_;
 const auto fighter=[&](const room::Member* m){return m?(m->id==v.room.localMember?v.selectedFighter:m->fighter):-1;};
 // A cabinet's press does what Select does on its table's row.
 const auto pressBody=[&](const MenuEntry& e){
  if(!nav.Asking()&&!nav.Editing()){nav.Focus(e.id,rows);action=nav.Choose(rows);}
 };
 // An Arcade room's floor: two columns of cabinets facing each other, as
 // single player cabinets stand in an arcade, a row for each table. A pair
 // with anyone at it is joined across the aisle by a band in its table's
 // colour with its score, or VS while a cabinet waits for an opponent; an
 // empty cabinet says Insert coin. A pair is its table's row: it takes the
 // focus and the press.
 // What both Arcade views show of a table's cabinets: each cabinet's number,
 // player and table, a colour shared by the pair while anyone sits there,
 // and a band in that colour joining the pair with its score or VS.
 // One colour for each of the room's eight tables.
 static const ImU32 accents[room::TableCount]={IM_COL32(230,186,72,255),IM_COL32(110,160,230,255),IM_COL32(120,200,140,255),IM_COL32(190,140,220,255),
  IM_COL32(240,130,60,255),IM_COL32(80,210,220,255),IM_COL32(240,110,170,255),IM_COL32(190,220,80,255)};
 const auto tableRow=[&](const room::Table& t)->const MenuEntry*{
  const auto id="table-"+std::to_string(t.id);
  const auto row=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  return row==rows.end()?nullptr:&*row;
 };
 const auto cabinetView=[&](const room::Table& t,int side,const MenuEntry* row){
  const room::MemberId member=side?t.p2:t.p1;
  CabinetTileView view;view.id="cabinet-"+std::to_string(t.id*2+side);view.number=t.id*2+side+1;
  view.accent=t.p1||t.p2?accents[t.id]:0;
  view.status=loc::Tf("room.battle_slot",t.id+1);
  view.crown=t.streakHolder==member&&t.streak>=2;
  if(view.crown)view.status+="  "+loc::Tf("room.streak",t.streak);
  if(const auto* m=room::FindMember(v.room,member)){view.occupied=true;view.fighter=fighter(m);view.country=m->country;
   view.name=m->name+(m->id==v.room.localMember?std::string(loc::T("room.suffix_you")):"");
   const auto* f=selection::FindFighter(view.fighter);view.caption=f?f->name:loc::T("room.fighter_not_shared");}
  else view.status=loc::T("room.cabinet_free");
  if(row){view.focused=row->id==focus;view.focusChanged=changed&&side==0;}
  view.disabled=nav.Choosing();
  return view;
 };
 // A press on an empty cabinet sits there, as its seat's choice does; on
 // anyone's it is a press on its table's row.
 const auto pressCabinet=[&](const MenuEntry* row,const CabinetTileView& view,int side){
  if(!row)return;
  if(!view.occupied&&!row->choices.empty()){action={};action.kind=MenuAction::Chosen;action.id=row->id;action.text=side?"sit-p2":"sit-p1";return;}
  pressBody(*row);
 };
 const auto pairBand=[&](const room::Table& t,ImVec2 min,ImVec2 max,float size){
  const ImU32 accent=accents[t.id];
  auto* draw=ImGui::GetWindowDrawList();
  draw->AddRectFilled(min,max,(accent&0x00FFFFFF)|0x55000000,4*s);
  draw->AddRect(min,max,accent,4*s,0,2*s);
  std::string elided;
  DrawCardText(elided,ImVec2(min.x,(min.y+max.y-size)*.5f),max.x-min.x,t.p1&&t.p2?SetScoreText(t.score):"VS",size,palette::Ivory,true);
 };
 const auto arcadeFloor=[&](ImVec2 size){
  const float gap=6*s,band=80*s,width=(size.x-band)*.5f;
  const float height=(std::min)(96*s,(size.y-(room::TableCount-1)*gap)/room::TableCount);
  const auto origin=ImGui::GetCursorScreenPos();
  for(const auto& t:v.room.tables){
   const auto* row=tableRow(t);
   const float y=origin.y+t.id*(height+gap);
   for(int side=0;side<2;++side){
    const auto view=cabinetView(t,side,row);
    ImGui::SetCursorScreenPos(ImVec2(origin.x+side*(width+band),y));
    if(DrawCabinetTile(view,ImVec2(width,height)))pressCabinet(row,view,side);
   }
   if(t.p1||t.p2)pairBand(t,ImVec2(origin.x+width+4*s,y+height*.25f),ImVec2(origin.x+width+band-4*s,y+height*.75f),16*s);
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+room::TableCount*(height+gap)-gap));
  ImGui::Dummy(ImVec2(size.x,0));
 };
 // The Real Arcade view (room::ArcadeRows, room::ArcadeColumns): tall cabinets over a
 // drawn arcade. In two rows of eight a table's pair faces each other across
 // the aisle, cabinet n on top and n+8 below it; in four columns each pair is
 // stacked, numbered as on the floor. A band between a pair's cabinets joins
 // them.
 const auto realArcade=[&](ImVec2 size,bool columns){
  const auto origin=ImGui::GetCursorScreenPos();
  DrawArcadeBackdrop(origin,ImVec2(origin.x+size.x,origin.y+size.y));
  const int across=columns?4:8,down=columns?4:2;
  const float gap=8*s,band=24*s;
  // Four columns are two blocks of stacked pairs; two rows are two rows.
  float width=(size.x-(across-1)*gap)/across,height=columns?(size.y-2*band-gap)/4:(size.y-band)/down;
  // A cabinet keeps its shape: as wide as the space allows, about twice as tall.
  width=(std::min)(width,height*.52f);height=(std::min)(height,width*2.2f);
  const float left=origin.x+(size.x-(across*width+(across-1)*gap))*.5f;
  const float floor=columns?4*height+2*band+gap:2*height+band,top=origin.y+(std::max)(0.f,(size.y-floor)*.5f);
  for(const auto& t:v.room.tables){
   const auto* row=tableRow(t);
   ImVec2 at[2];
   for(int side=0;side<2;++side){
    const int column=columns?t.id%4:t.id,line=columns?(t.id/4)*2+side:side;
    const float y=columns?top+(line/2)*(2*height+band+gap)+(line%2)*(height+band):top+line*(height+band);
    at[side]=ImVec2(left+column*(width+gap),y);
    ImGui::SetCursorScreenPos(at[side]);
    auto view=cabinetView(t,side,row);
    if(!columns)view.number=t.id+1+side*static_cast<int>(room::TableCount);
    if(DrawRealCabinet(view,ImVec2(width,height),loc::T("room.cabinet_empty")))pressCabinet(row,view,side);
   }
   if(t.p1||t.p2)pairBand(t,ImVec2(at[0].x+width*.15f,at[0].y+height+2*s),ImVec2(at[0].x+width*.85f,at[0].y+height+band-2*s),13*s);
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+size.y));
  ImGui::Dummy(ImVec2(size.x,0));
 };
 // V2 (Arcade.dc.html): the cabinets on a lit floor in the room's view. A pair
 // is its table's row: the pointer on either cabinet focuses it, the focus rings
 // the pair, and a press is the cabinet's as above.
 const auto v2Arcade=[&](ImVec2 size,room::ArcadeView shown){
  const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
  const ImVec2 origin=ImGui::GetCursorScreenPos(),pointer=ImGui::GetIO().MouseDelta;
  const bool moved=pointer.x!=0||pointer.y!=0;
  const v2::Box floor{origin,ImVec2(origin.x+size.x,origin.y+size.y)};
  d->AddRectFilledMultiColor(floor.min,floor.max,c.header,c.header,c.frame,c.frame);
  d->AddRect(floor.min,floor.max,c.border,4*s,0,s);
  const v2::Box in=v2::Inset(floor,14*s,14*s);
  const ImU32 pending=ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
  const auto centred=[&](float size,ImVec2 mid,ImU32 color,const std::string& text){
   const auto w=HeadingFont()->CalcTextSizeA(size,FLT_MAX,0,text.c_str());
   d->AddText(HeadingFont(),size,ImVec2(mid.x-w.x*.5f,mid.y-w.y*.5f),color,text.c_str());
  };
  const auto tag=[&](const room::Member& m,ImVec2 at,float width,bool ping){
   v2::TagView g;g.name=m.name;g.marks={m.country,m.badge};
   if(ping)g.ping=LatencyMarkFor(v.memberLatency,m,v.room.localMember);
   v2::Tag(d,at,width,g,v2::TagSize::XS);
  };
  // A cabinet's button: the press and the pointer's focus.
  const auto cabinet=[&](const room::Table& t,int side,const MenuEntry* row,const v2::Box& b){
   const auto view=cabinetView(t,side,row);
   ImGui::SetCursorScreenPos(b.min);ImGui::PushID(view.id.c_str());
   const bool pressed=ImGui::InvisibleButton("##cabinet",ImVec2(b.W(),b.H())),hovered=ImGui::IsItemHovered();ImGui::PopID();
   if(pressed&&!nav.Choosing())pressCabinet(row,view,side);
   else if(hovered&&moved&&row&&!nav.Choosing())nav.Focus(row->id,rows);
  };
  // The pair's score, or VS while one waits, on a pill in the pair's colour.
  const auto band=[&](const room::Table& t,ImVec2 mid,float width,float height,float size){
   const v2::Box b{ImVec2(mid.x-width*.5f,mid.y-height*.5f),ImVec2(mid.x+width*.5f,mid.y+height*.5f)};
   if(!t.p1&&!t.p2){if(shown!=room::ArcadeFloor)centred(size,mid,c.border,"\xC2\xB7");return;}
   const ImU32 pair=PairColor(t.id);
   d->AddRectFilled(b.min,b.max,v2::color::Alpha(pair,.13f),height*.5f);d->AddRect(b.min,b.max,pair,height*.5f,0,s);
   centred(size,mid,pair,t.p1&&t.p2?SetScoreText(t.score):"VS");
  };
  const auto ring=[&](const MenuEntry* row,ImVec2 min,ImVec2 max){
   if(!row||row->id!=focus)return;
   v2::Focus(d,ImVec2(min.x-4*s,min.y-4*s),ImVec2(max.x+4*s,max.y+4*s),false);
   if(nav.Choosing())menu_.bodyChoices={{min,max},floor};
  };
  const auto state=[&](const room::Table& t,room::MemberId member){
   if(!(t.p1&&t.p2))return std::string(loc::T("room.arcade_waiting"));
   const std::string playing=loc::T("room.status.playing");
   return t.streakHolder==member&&t.streak>=2?loc::Tf("room.streak",t.streak)+" \xC2\xB7 "+playing:playing;
  };
  if(shown==room::ArcadeFloor){
   // A line for each table: the pair facing each other, the band between them.
   float gap=8*s;const float bandWidth=64*s;
   const float line=(std::min)(40*s,(in.H()-7*gap)/8);if(line<36*s)gap=4*s;
   const float width=(in.W()-bandWidth-16*s)*.5f,top=in.min.y+(std::max)(0.f,(in.H()-8*line-7*gap)*.5f);
   for(const auto& t:v.room.tables){
    const auto* row=tableRow(t);const ImU32 pair=PairColor(t.id);const bool lit=t.p1||t.p2;
    const float y=top+t.id*(line+gap);
    for(int side=0;side<2;++side){
     const v2::Box b{ImVec2(in.min.x+side*(width+bandWidth+16*s),y),ImVec2(in.min.x+side*(width+bandWidth+16*s)+width,y+line)};
     cabinet(t,side,row,b);
     d->AddRectFilled(b.min,b.max,c.frame,4*s);d->AddRect(b.min,b.max,lit?pair:c.border,4*s,0,s);
     // A small cabinet with its number on the marquee.
     const ImVec2 mini(b.min.x+10*s,b.min.y+(line-28*s)*.5f);
     d->AddRectFilled(mini,ImVec2(mini.x+22*s,mini.y+28*s),c.ink,2*s);
     d->AddRectFilled(mini,ImVec2(mini.x+22*s,mini.y+10*s),lit?pair:c.button,2*s,ImDrawFlags_RoundCornersTop);
     d->AddRect(mini,ImVec2(mini.x+22*s,mini.y+28*s),lit?pair:c.border,2*s,0,2*s);
     centred(9*s,ImVec2(mini.x+11*s,mini.y+5*s),lit?c.ink:c.muted,std::to_string(t.id+1+side*room::TableCount));
     const room::MemberId member=side?t.p2:t.p1;
     const auto* m=room::FindMember(v.room,member);
     const std::string words=m?state(t,member):std::string();
     const float wordsWidth=HeadingFont()->CalcTextSizeA(12*s,FLT_MAX,0,words.c_str()).x,x=mini.x+30*s;
     // The ping only where the name still fits beside it; the members list always has it.
     if(m)tag(*m,ImVec2(x,b.min.y+(line-ImGui::GetFontSize())*.5f-2*s),(std::max)(0.f,b.max.x-10*s-wordsWidth-8*s-x),width>=420*s);
     else d->AddText(HeadingFont(),13*s,ImVec2(x,b.min.y+(line-13*s)*.5f),c.muted,loc::T("room.cabinet_free"));
     d->AddText(HeadingFont(),12*s,ImVec2(b.max.x-10*s-wordsWidth,b.min.y+(line-12*s)*.5f),t.p1&&t.p2?pair:c.muted,words.c_str());
    }
    band(t,ImVec2(in.min.x+width+8*s+bandWidth*.5f,y+line*.5f),bandWidth,28*s,14*s);
    ring(row,ImVec2(in.min.x,y),ImVec2(in.max.x,y+line));
   }
  }else{
   // Tall cabinets, each over its stool, the player's back on it. Rows: n on
   // top faces n+8 under it; Columns: four columns of stacked pairs.
   const bool columns=shown==room::ArcadeColumns;
   const float across=columns?18*s:8*s,cell=columns?(in.W()-3*across)/4:(in.W()-7*across)/8;
   const float byHeight=columns?(in.H()-50*s)/4/1.6f:(in.H()-52*s)/2/1.56f;
   const float h=(std::max)(48*s,(std::min)({byHeight,cell/.6f,240*s})),w=h*.6f,unit=h*1.56f;
   const float bandHeight=columns?26*s:36*s,pairHeight=2*unit+bandHeight;
   const float top=in.min.y+(std::max)(0.f,(in.H()-(columns?2*pairHeight+10*s:pairHeight))*.5f);
   const auto tall=[&](const room::Table& t,int side,const MenuEntry* row,ImVec2 at){
    const v2::Box b{at,ImVec2(at.x+w,at.y+h)};
    cabinet(t,side,row,b);
    const ImU32 pair=PairColor(t.id);const bool lit=t.p1||t.p2;
    const room::MemberId member=side?t.p2:t.p1;const auto* m=room::FindMember(v.room,member);
    if(lit)d->AddRect(ImVec2(b.min.x-2*s,b.min.y-2*s),ImVec2(b.max.x+2*s,b.max.y+2*s),v2::color::Alpha(pair,.27f),8*s,0,4*s);
    d->AddRectFilled(b.min,b.max,c.ink,6*s,ImDrawFlags_RoundCornersTop);
    // From the top: marquee, screen, name plate, control panel, kick plate.
    v2::Box body=b;
    const v2::Box marquee=v2::CutTop(body,h*.12f),kick=v2::CutBottom(body,h*.06f);
    v2::CutBottom(body,4*s);
    const v2::Box panel=v2::Inset(v2::CutBottom(body,h*.1f),5*s,0),plate=v2::Inset(v2::CutBottom(body,22*s),5*s,0);
    v2::CutTop(body,4*s);
    const v2::Box screen=v2::Inset(body,5*s,0);
    d->AddRectFilled(marquee.min,marquee.max,lit?pair:c.button,6*s,ImDrawFlags_RoundCornersTop);
    const std::string number=std::to_string(t.id+1+side*room::TableCount);
    const bool crown=m&&t.streakHolder==member&&t.streak>=2;
    centred(11*s,ImVec2((marquee.min.x+marquee.max.x)*.5f+(crown?6*s:0),(marquee.min.y+marquee.max.y)*.5f),lit?c.ink:c.muted,number);
    if(crown)v2::Icon(d,v2::IconCount-1,ImVec2((marquee.min.x+marquee.max.x)*.5f-14*s,(marquee.min.y+marquee.max.y)*.5f-5*s),10*s,c.ink);
    const bool playing=m&&t.p1&&t.p2;
    d->AddRectFilled(screen.min,screen.max,!m?c.frame:playing?c.header:v2::color::Alpha(pending,.12f),2*s);
    const auto* f=m?selection::FindFighter(fighter(m)):nullptr;
    const std::string on=!m?std::string(loc::T("room.cabinet_empty")):playing?(f?f->name:std::string(loc::T("room.fighter_not_shared"))):loc::T("room.arcade_waiting");
    centred(10*s,ImVec2((screen.min.x+screen.max.x)*.5f,(screen.min.y+screen.max.y)*.5f),!m?c.muted:playing?pair:pending,on);
    d->AddRectFilled(plate.min,plate.max,c.frame);
    // A small cabinet keeps its flag and name inside the plate.
    d->PushClipRect(plate.min,plate.max,true);
    if(m)tag(*m,ImVec2(plate.min.x+2*s,plate.min.y),plate.W()-4*s,false);
    else{
     const char* coin=loc::T("room.cabinet_free");const float wide=HeadingFont()->CalcTextSizeA(11*s,FLT_MAX,0,coin).x;
     centred(11*s*(std::min)(1.f,plate.W()/wide),ImVec2((plate.min.x+plate.max.x)*.5f,(plate.min.y+plate.max.y)*.5f),c.muted,coin);
    }
    d->PopClipRect();
    d->AddRectFilled(panel.min,panel.max,c.button);
    const ImVec2 mid((panel.min.x+panel.max.x)*.5f,(panel.min.y+panel.max.y)*.5f);
    d->AddCircleFilled(ImVec2(mid.x-12*s,mid.y),3.5f*s,c.live);
    for(int i=0;i<3;++i)d->AddCircleFilled(ImVec2(mid.x-2*s+i*7*s,mid.y),3*s,c.text);
    if(lit)d->AddRectFilled(kick.min,kick.max,v2::color::Alpha(pair,.2f));
    d->AddRect(b.min,b.max,lit?pair:c.border,6*s,ImDrawFlags_RoundCornersTop,2*s);
    // The stool, and on it the player's back (a 60 by 70 drawing).
    const float k=h*.62f/60;const ImVec2 o(b.min.x+(w-60*k)*.5f,b.max.y-h*.16f);
    const auto p=[&](float x,float y){return ImVec2(o.x+x*k,o.y+y*k);};
    d->AddRectFilled(p(12,52),p(48,57),lit?pair:c.border,2*k);
    d->AddLine(p(17,57),p(13,70),c.hover,2.5f*k);d->AddLine(p(43,57),p(47,70),c.hover,2.5f*k);d->AddLine(p(15,65),p(45,65),c.hover,2.5f*k);
    if(!m)return;
    // ponytail: an Ash jacket and one hair colour; the title's tier and icon once members carry titles.
    const auto jacket=v2::TierColor(v2::Tier::Ash);
    const ImVec2 back[]={p(14,54),p(15,40),p(18,29),p(22,22),p(38,22),p(42,29),p(45,40),p(46,54)};
    d->AddConvexPolyFilled(back,8,jacket.plate);d->AddPolyline(back,8,jacket.stripe,ImDrawFlags_Closed,1.5f*k);
    d->AddLine(p(30,23),p(30,53),v2::color::Alpha(jacket.stripe,.55f),2.5f*k);
    d->AddRectFilled(p(15,25),p(25,32),c.button,1.5f*k);d->AddRect(p(15,25),p(25,32),jacket.stripe,1.5f*k,0,.8f*k);
    if(4.6f*k>=7*s)centred(4.6f*k,p(20,28.5f),c.text,m->country.ToString());
    d->AddRectFilled(p(26,16),p(34,23),IM_COL32(217,168,130,255),2*k);
    d->AddCircleFilled(p(30,11),8.75f*k,IM_COL32(58,42,31,255));
   };
   for(const auto& t:v.room.tables){
    const auto* row=tableRow(t);
    const int column=columns?t.id%4:t.id;
    const float x=in.min.x+column*(cell+across)+(cell-w)*.5f,y=columns?top+(t.id/4)*(pairHeight+10*s):top;
    tall(t,0,row,ImVec2(x,y));tall(t,1,row,ImVec2(x,y+unit+bandHeight));
    band(t,ImVec2(x+w*.5f,y+unit+bandHeight*.5f),56*s,columns?22*s:26*s,13*s);
    ring(row,ImVec2(x,y),ImVec2(x+w,y+pairHeight));
   }
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+size.y));
  ImGui::Dummy(ImVec2(size.x,0));
 };
 if(v2::On())v2Arcade(size,v2::Narrow(ImGui::GetIO().DisplaySize.x,s)?room::ArcadeFloor:view);
 else if(view==room::ArcadeRows||view==room::ArcadeColumns)realArcade(size,view==room::ArcadeColumns);
 else arcadeFloor(size);
}

} }
