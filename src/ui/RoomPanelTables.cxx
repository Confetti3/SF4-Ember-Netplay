// The room board (Room.dc.html): the tables, the focused row's line and the
// room's action bar on the left; Members over Chat in a column on the right
// (either side of the tables on a 32:9 screen, RoomPanelColumn.cxx); on a
// narrow screen one of the three at a time, under tabs that follow the focus.
// A Classic room's floor is its table cards, drawn here; an Arcade or King of
// the hill room's is its cabinets (RoomPanelArcade.cxx). The rows are built
// in RoomPanel.cxx (RoomBoardRows).
#include "RoomPanelBoard.hxx"
#include "TableCardShape.hxx"
#include "Theme.hxx"
#include "RoomControls.hxx"
#include "RoomCards.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "LatencyMark.hxx"
#include "MenuProbes.hxx"
#include "SelectionLabels.hxx"
#include "../common/GameCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cstring>

namespace sf4e { namespace ui {
using namespace room_controls;

const MenuEntry* RoomBoardFrame::Find(const char* id) const {
 const auto found=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
 return found==rows.end()?nullptr:&*found;
}
bool RoomBoardFrame::Press(const MenuEntry& e,const v2::Box& b,bool choose) const {
 ReportMenuCard(e.id.c_str(),b.min,b.max);
 const auto press=PressEntry(nav,rows,e,b,action,focusChanged,{},choose);
 if(press.pressed&&!choose)nav.Focus(e.id,rows);
 return press.hovered;
}
void RoomBoardFrame::Ground(ImDrawList* d,const v2::Box& b,ImU32 fill,ImU32 edge) const {
 d->AddRectFilled(b.min,b.max,fill,4*s);d->AddRect(b.min,b.max,edge,4*s,0,s);
}
void RoomBoardFrame::Panel(ImDrawList* d,const v2::Box& b) const {
 Ground(d,b,v2::PanelGround(v.SeeThrough()),v2::Colors().border);
}
float RoomBoardFrame::HideButton(ImDrawList* d,const MenuEntry& e,ImVec2 corner) const {
 const auto& c=v2::Colors();
 const std::string label=loc::T(e.id.compare(0,10,"room-show-")?"room.v2.hide":"room.v2.show");
 const float w=ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,0,label.c_str()).x+20*s;
 const v2::Box b{ImVec2(corner.x-w,corner.y),ImVec2(corner.x,corner.y+32*s)};
 const bool hovered=Press(e,b),focused=e.id==focus;
 v2::CardGround(d,b,focused,hovered,false);
 v2::Text(d,ImVec2(b.min.x+10*s,b.min.y+9*s),w-20*s,label,14*s,e.enabled&&(hovered||focused)?c.text:c.muted);
 if(focused)v2::Focus(d,b.min,b.max);
 return w;
}

SeatLook SeatLookOf(const ShellView& v,const room::Member& m) {
 const bool own=m.id==v.room.localMember;
 const int picked=own?v.selectedPick.fighter:m.fighter;
 SeatLook look;look.picked=picked>=0;look.fighter=look.picked?picked:own?v.preferences.mainFighter:m.mainFighter;
 look.random=own&&v.selectedRandom;
 if(!look.picked)return look;
 if(own){look.costume=v.selectedPick.costume;look.color=v.selectedPick.color;look.ultra=v.selectedPick.ultra;}
 else{look.costume=m.costume;look.color=m.color;look.ultra=m.ultra;}
 return look;
}
FighterArt RoomFighterArt(const netplay::PlayerPreferences& preferences) {
 return preferences.roomCharacterPictures?FighterArt::Character:FighterArt::Portrait;
}
FighterPicture SeatPicture(const SeatLook& look,FighterArt art) {
 return {look.fighter,art,(std::max)(0,look.costume),(std::max)(0,look.color)};
}
std::string SeatCaption(const SeatLook& look,bool brief) {
 if(!look.picked||!ember::ValidFighter(look.fighter))return loc::T("room.fighter_not_shared");
 const char* fighter=ember::Catalog().fighterName(look.fighter);
 const std::string name=look.random?loc::Tf("selection.random_fighter",fighter):std::string(fighter);
 if(look.costume<0||look.ultra<0)return name;
 selection::Pick pick;pick.fighter=look.fighter;pick.costume=look.costume;pick.color=(std::max)(0,look.color);
 // A game whose fighters have one Ultra (the catalog's) names none.
 if(ember::Catalog().ultraChoices<=1)return name+" · "+ShortCostumeLabel(pick);
 return loc::Tf("room.seat_look",name,ShortCostumeLabel(pick),brief?ShortUltraLabel(look.ultra):UltraLabel(look.ultra));
}

namespace {
// The room's action bar: the board's Bar rows, every row the floor and the
// column do not draw (BoardAreaOf), as buttons on as few lines as their labels allow (v2::BarLines),
// each place at least 140 px. The invitation's copy buttons share one place,
// each with its short name, where the bar is wide enough for both.
class RoomActionBar {
public:
 RoomActionBar(const RoomBoardFrame& f,float width):f_(f){
  for(const auto& e:f.rows){
   if(BoardAreaOf(e.id)!=BoardArea::Bar)continue;
   const bool copy=HasPrefix(e.id,"copy");
   if(copy&&!slots_.empty()&&HasPrefix(slots_.back().front()->id,"copy")&&SlotNeed({slots_.back().front(),&e})<=width)slots_.back().push_back(&e);
   else slots_.push_back({&e});
  }
  for(const auto& slot:slots_)places_.push_back((std::max)(140*f.s,SlotNeed(slot)));
 }
 // The bar's height `width` wide.
 float Height(float width) const {return (v2::BarLines(width,places_,12*f_.s)*60-12)*f_.s;}
 // The bar `width` wide from `at`, drawn in `d`; a focused popout's choices open over `column`.
 void Draw(ImDrawList* d,ImVec2 at,float width,const v2::Box& column) const {
  const float s=f_.s;
  const int lines=v2::BarLines(width,places_,12*s);
  const auto places=v2::BarPlaces({at,ImVec2(at.x+width,at.y+Height(width))},places_,12*s,lines,48*s);
  for(std::size_t i=0;i<slots_.size();++i){
   // A shared place goes to its buttons as a bar of its own.
   const auto parts=v2::BarPlaces(places[i],Needs(slots_[i]),8*s,1,48*s);
   for(std::size_t k=0;k<slots_[i].size();++k){
    const auto& e=*slots_[i][k];const v2::Box& b=parts[k];
    const bool hovered=f_.Press(e,b),focused=e.id==f_.focus,leave=e.id=="leave"||e.id=="stop-watching";
    // Ready is the one filled button; Leave room is drawn in the danger tone.
    v2::Button(d,b,Label(e),e.id=="ready"?v2::ButtonKind::Primary:leave?v2::ButtonKind::Danger:v2::ButtonKind::Secondary,focused,hovered,f_.feedback.Enabled(e));
    if(focused&&e.popout)f_.choices={b,column};
   }
  }
 }
private:
 static std::string Label(const MenuEntry& e){
  return e.id=="copy"?std::string(loc::T("room.v2.copy_invite")):e.id=="copy-short"?std::string(loc::T("room.v2.copy_link")):e.label;
 }
 // What a button's label needs: a split button only v2::Button's own 12 px each side.
 float Need(const MenuEntry& e,bool split) const {return HeadingFont()->CalcTextSizeA(17*f_.s,FLT_MAX,0,Label(e).c_str()).x+(split?24:36)*f_.s;}
 std::vector<float> Needs(const std::vector<const MenuEntry*>& slot) const {
  std::vector<float> each;for(const auto* e:slot)each.push_back(Need(*e,slot.size()>1));return each;
 }
 float SlotNeed(const std::vector<const MenuEntry*>& slot) const {
  float w=(slot.size()-1)*8*f_.s;for(const float n:Needs(slot))w+=n;return w;
 }
 const RoomBoardFrame& f_;
 std::vector<std::vector<const MenuEntry*>> slots_;
 std::vector<float> places_;
};

// The board's one line for the focused row's explanation. Lines after the
// first (the fighter summary under Ready, for example) and any elided text
// are reachable on hover.
void DrawFocusedLine(const RoomBoardFrame& f,float size) {
 // The focus now: the narrow board's tabs may have just moved it.
 const auto* e=f.Find(f.nav.Focus().c_str());
 if(!e)return;
 std::string elided;
 // The highlighted option of the entry's open choice, in full.
 const auto& nav=f.nav;
 const std::string choice=nav.Choosing()&&nav.DialogId()==e->id&&nav.ChoiceIndex()<e->choices.size()?e->choices[nav.ChoiceIndex()].detail:std::string();
 const auto newline=e->detail.find('\n');
 std::string line=choice.empty()?e->detail.substr(0,newline):choice;
 // The board renders in place of the menu list, so it uses the same smoothed
 // verdict and the same wording. Presentation only: nav.Choose still gates on
 // the live entry and SendRoom still re-checks room authority at dispatch.
 const std::string reason=f.feedback.Enabled(*e)?"":loc::T(f.feedback.Pending(*e)?"room.updating":"common.unavailable");
 NoteDetailText(e->detail.substr(0,newline),e->detailText);
 if(!reason.empty())line=line.empty()?reason:line+"  -  "+reason;
 const auto p=ImGui::GetCursorScreenPos();const float lineWidth=ImGui::GetContentRegionAvail().x;
 DrawCardText(elided,p,lineWidth,line,size,reason.empty()?v2::Colors().muted:ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)));
 if(newline!=std::string::npos)elided+=(elided.empty()?"":"\n")+e->detail;
 ImGui::Dummy(ImVec2(lineWidth,ImGui::GetFontSize()));
 FlushCardTip(elided);
}

// A table card's footer: a first-to-N table names its set and rotation where
// the round settings would be; those stay on the table's rules rows.
std::string TableFooter(const ShellView& v,const room::Table& t) {
 const auto watching=t.spectators.size()+t.watchingNext.size();
 return (v.room.practice?std::string(loc::T("room.table_footer_practice"))+"     ":std::string())+
  (v.room.delayPolicy==room::DelayPolicy::Own?std::string(loc::T("room.table_footer_own_delay"))+"     ":std::string())+
  (v.room.wiredOnly?std::string(loc::T("room.table_footer_wired_only"))+"     ":std::string())+(t.rules.format==room::SetFormat::Unlimited
  ?loc::Tf("room.table_footer",t.rules.roundCount,room::MatchRoundTime(v.room,t),t.queue.size(),watching)
  :loc::Tf("room.table_footer_set",SetLengthText(t.rules.format),RotationText(t.rules.rotation),t.queue.size(),watching));
}
// The footer's queue and watchers alone, for a versus card's header while the strip covers its foot.
std::string TableCounts(const room::Table& t) {
 return loc::Tf("room.table_counts",t.queue.size(),t.spectators.size()+t.watchingNext.size());
}

// A card's strip: the open seat chooser's options, or on your own focused
// place Ready and Leave. A strip option is half a card wide, so Unready drops
// its "unlock fighter".
std::vector<TableCardOption> StripOptions(const RoomBoardFrame& f,const MenuEntry& e,const room::Table& t,bool choosing,bool ownPlace,const PlaceExit& place) {
 std::vector<TableCardOption> options;
 if(choosing)for(const auto& c:e.choices){const auto* option=FindSeatOption(c.id);options.push_back({c.label,c.enabled,option?option->seat:-1});}
 else if(ownPlace){
  const auto mine=room::PlaceOf(f.v.room,f.v.room.localMember);
  const bool seated=mine.kind==room::Place::Kind::Seat;
  const auto ready=seated?DescribeReady(f.v,t,mine.seat):ReadyControl{};
  options={{ready.kind==ReadyControl::Unready?std::string(loc::T("room.unready.short")):e.hint,f.feedback.Enabled(e)&&(!seated||ready.kind!=ReadyControl::None)},
   {place.label,place.allowed}};
 }
 return options;
}

// A seat in `box`: the fighter's picture, as tall as the box, at its outer
// end (the right one for a versus card's second seat), and in the rest the
// player's tag over their fighter, costume and Ultra, with Ready on its inner
// side while they are ready. Where the box is tall enough a third line says
// the player's win streak at this table, else how long they have been idle,
// as the Classic card did. A versus card centres the lines between the
// picture and the score, so the pair stands even about it; a stacked seat
// starts them beside the picture. The picture is the fighter they picked,
// else their main (SeatLookOf). An empty seat is an empty frame and "Open seat".
void DrawSeat(const RoomBoardFrame& f,ImDrawList* d,const v2::Box& box,const room::Table& t,int side,bool versus,bool marked,std::string& elided) {
 const auto& v=f.v;const float s=f.s,face=box.H(),gap=table_card::FaceGap*s;const auto& c=v2::Colors();
 const bool mirrored=versus&&side==1;
 const v2::Box tile=mirrored?v2::Box{ImVec2(box.max.x-face,box.min.y),box.max}:v2::Box{box.min,ImVec2(box.min.x+face,box.max.y)};
 const float left=mirrored?box.min.x:tile.max.x+gap,right=mirrored?tile.min.x-gap:box.max.x,room=right-left;
 const auto widthOf=[](const std::string& value,float size){return ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,value.c_str()).x;};
 // Where a line `width` wide starts: centred on a versus card, else at the start.
 const auto lineAt=[&](float width){return left+(versus?(std::max)(0.f,room-width)*.5f:0.f);};
 const float tagLine=v2::ChipFont()+8*s,lineSize=13*s;
 const auto* m=room::FindMember(v.room,side?t.p2:t.p1);
 if(!m){
  // An open seat is a place to sit: a dim tile with a plus.
  d->AddRectFilled(tile.min,tile.max,v2::color::Alpha(c.frame,.55f),4*s);d->AddRect(tile.min,tile.max,c.border,4*s,0,s);
  const ImVec2 mid((tile.min.x+tile.max.x)*.5f,(tile.min.y+tile.max.y)*.5f);const float arm=face*.14f;
  d->AddLine(ImVec2(mid.x-arm,mid.y),ImVec2(mid.x+arm,mid.y),c.muted,2*s);d->AddLine(ImVec2(mid.x,mid.y-arm),ImVec2(mid.x,mid.y+arm),c.muted,2*s);
  const std::string open=loc::T("room.open_seat");
  const float w=(std::min)(room,widthOf(open,15*s));
  DrawCardText(elided,ImVec2(lineAt(w),box.min.y+(face-15*s)*.5f),room,open,15*s,c.muted);
  return;
 }
 const auto look=SeatLookOf(v,*m);
 // The third line: a streak takes it from idle time.
 const std::string streak=StreakText(t,m->id),note=streak.empty()?IdleText(*m,v.roomElapsedMs):streak;
 const bool third=!note.empty()&&face>=tagLine+2*lineSize+16*s;
 // The lines together, centred on the picture.
 const float block=tagLine+4*s+lineSize+(third?4*s+lineSize:0),top=box.min.y+(std::max)(0.f,(face-block)*.5f);
 DrawCharacterPortrait(SeatPicture(look,f.art),tile.min,tile.max,c.frame);
 if(marked)d->AddRect(ImVec2(tile.min.x-2*s,tile.min.y-2*s),ImVec2(tile.max.x+2*s,tile.max.y+2*s),ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)),3*s,0,2*s);
 const auto tag=MemberTag(v.memberLatency,*m,v.room.localMember);
 const float tagWidth=(std::min)(room,v2::TagWidth(tag,v2::TagSize::S));
 v2::Tag(d,ImVec2(lineAt(tagWidth),top),tagWidth,tag,v2::TagSize::S);
 // The fighter, costume and Ultra, and Ready on their inner side (towards
 // the score) while the seat is ready. The Ultra's short name where the long
 // one does not fit.
 const bool ready=t.ready[side]&&(t.phase==room::TablePhase::Waiting||t.phase==room::TablePhase::Ready);
 const std::string readyWord=ready?loc::T("room.status.ready"):"";
 const float chip=ready?v2::ChipWidth(readyWord)+8*s:0,y=top+tagLine+4*s;
 std::string name=SeatCaption(look);
 if(widthOf(name,lineSize)>room-chip)name=SeatCaption(look,true);
 const float nameWidth=(std::min)(room-chip,widthOf(name,lineSize)),x=lineAt(nameWidth+chip);
 DrawCardText(elided,ImVec2(mirrored?x+chip:x,y),room-chip,name,lineSize,c.muted);
 if(ready)v2::Chip(d,ImVec2(mirrored?x:x+nameWidth+8*s,y+(lineSize-v2::ChipFont()-8*s)*.5f),readyWord,palette::Ready);
 if(third){
  const float w=(std::min)(room,widthOf(note,lineSize));
  DrawCardText(elided,ImVec2(lineAt(w),y+lineSize+4*s),room,note,lineSize,streak.empty()?c.muted:c.accent);
 }
}

// A Classic room's floor in `tables`: its cards as `shape` lays them out
// (TableCardShape.hxx), in their own scroll when there are more than fit;
// with `follows`, the action bar after them in that scroll.
//  A versus card:  TABLE 1 [phase] name
//                  [face]   name, fighter, streak   2 - 1   name, fighter, streak   [face]
//                  rules, queue, watching (or the strip)
//  A stacked card: TABLE 1 [phase] name                                    2 - 1
//                  [face] name, fighter
//                  [face] name, fighter
//                  rules, queue, watching (or the strip)
void DrawTableCards(const RoomBoardFrame& f,const v2::Box& tables,const TableCardShape& shape,const PlaceExit& place,const RoomActionBar* follows) {
 namespace card=table_card;
 const auto& v=f.v;const auto& nav=f.nav;const float s=f.s;const auto& c=v2::Colors();
 const bool onOwnPlace=OnOwnPlace(place,f.focus);
 const auto pointer=ImGui::GetIO().MouseDelta;const bool pointerMoved=pointer.x!=0||pointer.y!=0;
 const int columns=shape.columns;const bool versus=shape.versus;const float cardHeight=shape.height;
 const float cardWidth=(tables.W()-(columns-1)*card::Between*s)/columns;
 std::string elided;
 const auto text=[&](ImVec2 p,float width,const std::string& value,float size,ImU32 color,bool centred=false){DrawCardText(elided,p,width,value,size,color,centred);};
 ImGui::SetCursorScreenPos(tables.min);
 ImGui::BeginChild("Room tables",ImVec2(tables.W(),tables.H()),0,ImGuiWindowFlags_NoNavInputs);
 auto* td=ImGui::GetWindowDrawList();
 const auto start=ImGui::GetCursorScreenPos();int n=0;
 for(const auto& e:f.rows){
  const auto index=RowKey<std::size_t>(e.id,"table-");
  if(!index||*index>=v.room.tables.size())continue;
  const auto& t=v.room.tables[*index];
  const ImVec2 at(start.x+(n%columns)*(cardWidth+card::Between*s),start.y+(n/columns)*(cardHeight+card::Between*s));++n;
  const v2::Box b{at,ImVec2(at.x+cardWidth,at.y+cardHeight)};
  const bool focused=e.id==f.focus,choosing=nav.Choosing()&&nav.DialogId()==e.id&&!e.choices.empty();
  const auto options=StripOptions(f,e,t,choosing,!nav.Asking()&&onOwnPlace&&focused,place);
  // The set's score once both seats are taken, else vs; a closed table neither.
  const bool paired=t.p1&&t.p2;
  const std::string score=t.phase==room::TablePhase::Closed?std::string():paired?SetScoreText(t.score):"vs";
  const float scoreSize=22*s,scoreWidth=HeadingFont()->CalcTextSizeA(scoreSize,FLT_MAX,0,score.c_str()).x;
  // The seats: a versus card's at its two ends with the score between them,
  // a stacked card's one over the other.
  const float seatTop=b.min.y+card::Header*s;
  v2::Box seats[2];
  if(versus){
   const float middle=(std::max)(card::Middle*s,scoreWidth+32*s),half=(b.W()-2*card::Pad*s-middle)*.5f;
   seats[0]={ImVec2(b.min.x+card::Pad*s,seatTop),ImVec2(b.min.x+card::Pad*s+half,seatTop+card::Face*s)};
   seats[1]={ImVec2(b.max.x-card::Pad*s-half,seatTop),ImVec2(b.max.x-card::Pad*s,seatTop+card::Face*s)};
  }else for(int side=0;side<2;++side){const float y=seatTop+side*(card::SeatRow+card::SeatRowGap)*s;seats[side]={ImVec2(b.min.x+card::Pad*s,y),ImVec2(b.max.x-card::Pad*s,y+card::SeatRow*s)};}
  // The strip covers the footer. Its buttons go in first: the first item
  // under the pointer is the one it is over.
  const v2::Box strip{ImVec2(b.min.x+card::Pad*s,b.max.y-card::Footer*s),ImVec2(b.max.x-card::Pad*s,b.max.y-12*s)};
  const float ow=options.empty()?0:(strip.W()-(options.size()-1)*8*s)/options.size();
  int over=-1,pressed=-1;
  for(std::size_t i=0;i<options.size();++i){
   ImGui::SetCursorScreenPos(ImVec2(strip.min.x+i*(ow+8*s),strip.min.y));ImGui::PushID(e.id.c_str());ImGui::PushID(static_cast<int>(i));
   // The open chooser is itself a question, so its own options answer it.
   if(ImGui::InvisibleButton("##option",ImVec2(ow,strip.H()))&&(choosing||!nav.Asking())&&!nav.Editing())pressed=static_cast<int>(i);
   if(ImGui::IsItemHovered())over=static_cast<int>(i);
   ImGui::PopID();ImGui::PopID();
   ReportMenuCard((e.id+"/"+std::to_string(i)).c_str(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
  }
  const bool hovered=f.Press(e,b,!OnOwnPlace(place,e.id));
  if(choosing){
   // The chosen side follows the mouse, but only a moving one takes the choice.
   if(over>=0&&pointerMoved)f.nav.ChoiceIndex(static_cast<std::size_t>(over));
   if(pressed>=0)f.action=f.nav.Pick(static_cast<std::size_t>(pressed),f.rows);
  }else if(pressed==0){f.nav.Focus(e.id,f.rows);f.action=f.nav.Choose(f.rows);}
  else if(pressed==1)f.action={MenuAction::Activate,"leave-place"};
  if(!ImGui::IsItemVisible())continue;
  v2::CardGround(td,b,focused,hovered,place.table==static_cast<int>(t.id));
  // The score: a versus card's in the middle between the seats, at the
  // height of the players' names; a stacked card's at the header's right.
  const ImU32 scoreTone=paired?c.accent:c.muted;
  const float y=b.min.y+12*s,w=b.W()-2*card::Pad*s;
  float nameEnd=b.max.x-card::Pad*s;
  if(versus)td->AddText(HeadingFont(),scoreSize,ImVec2(seats[0].max.x+(seats[1].min.x-seats[0].max.x-scoreWidth)*.5f,seatTop+(card::Face*s-scoreSize)*.5f),scoreTone,score.c_str());
  else{nameEnd-=scoreWidth+12*s;td->AddText(HeadingFont(),scoreSize,ImVec2(b.max.x-card::Pad*s-scoreWidth,y),scoreTone,score.c_str());}
  // While the strip covers a versus card's foot, its queue and watchers stay
  // in sight at the header's right, where a stacked card has its score.
  if(versus&&!options.empty()){
   const std::string counts=TableCounts(t);
   const float countsWidth=ImGui::GetFont()->CalcTextSizeA(13*s,FLT_MAX,0,counts.c_str()).x;
   text(ImVec2(nameEnd-countsWidth,y+4*s),countsWidth,counts,13*s,c.muted);
   nameEnd-=countsWidth+12*s;
  }
  // TABLE n, its state, and the host's name for it.
  float x=b.min.x+card::Pad*s;
  x+=v2::SectionHeading(td,ImVec2(x,y+4*s),loc::Tf("room.v2.table",t.id+1),c.muted)+8*s;
  const ImU32 tone=t.phase==room::TablePhase::Playing?c.accent:t.phase==room::TablePhase::Idle||t.phase==room::TablePhase::Closed?c.muted:
   ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
  x+=v2::Chip(td,ImVec2(x,y),PhaseName(t),tone)+10*s;
  if(!t.name.empty()){NoteUserText(t.name);text(ImVec2(x,y+3*s),nameEnd-x,t.name,15*s,c.text);}
  const auto banner=DescribeTableBanner(v,t);
  for(int side=0;side<2;++side)DrawSeat(f,td,seats[side],t,side,versus,banner.seat==side,elided);
  // The seat the open chooser's option would take.
  if(choosing&&nav.ChoiceIndex()<options.size())
   if(const int seat=options[nav.ChoiceIndex()].seat;seat==0||seat==1)v2::Focus(td,ImVec2(seats[seat].min.x-6*s,seats[seat].min.y-4*s),ImVec2(seats[seat].max.x+6*s,seats[seat].max.y+4*s),false);
  if(!banner.text.empty())text(ImVec2(b.min.x+card::Pad*s,seats[1].max.y+6*s),w,banner.text,13*s,ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)));
  // The rules, queue and watchers along the foot, where the strip does not cover them.
  if(options.empty())text(ImVec2(b.min.x+card::Pad*s,b.max.y-12*s-14*s),w,TableFooter(v,t),13*s,c.muted);
  for(std::size_t i=0;i<options.size();++i){
   const v2::Box o{ImVec2(strip.min.x+i*(ow+8*s),strip.min.y),ImVec2(strip.min.x+i*(ow+8*s)+ow,strip.max.y)};
   const bool chosen=choosing&&i==nav.ChoiceIndex();
   f.Ground(td,o,chosen?c.header:over==static_cast<int>(i)?c.hover:c.button,v2::color::Alpha(c.accent,.35f));
   text(ImVec2(o.min.x+8*s,o.min.y+8*s),ow-16*s,options[i].label,15*s,options[i].live?c.text:c.muted,true);
   if(chosen)v2::Focus(td,o.min,o.max,false);
  }
  // Flushed once per card, while the card's button is still the hovered item.
  FlushCardTip(elided);
  if(focused)v2::Focus(td,b.min,b.max);
 }
 float bottom=start.y+((n+columns-1)/columns)*(cardHeight+card::Between*s)-card::Between*s;
 if(follows){const float width=ImGui::GetContentRegionAvail().x;follows->Draw(td,ImVec2(start.x,bottom+12*s),width,tables);bottom+=12*s+follows->Height(width);}
 ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(tables.W(),bottom-start.y));
 ImGui::EndChild();
}
}

void ApplicationShell::DrawRoomBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,
                                     const MenuVisualFeedback& feedback) {
 boardDrawn_=true;
 const float s=Scale();
 const float logical=ImGui::GetContentRegionAvail().x/s;const bool narrow=logical<860,ultra=logical>=1960;
 roomBoardNarrow_=narrow;
 // LB and RB go between the floor, the bar, Members and Chat (a narrow
 // board's tabs, the bar under the floor's). They step before the frame is
 // taken, so the row they land on is this frame's moved focus and scrolls
 // into view as a D-pad move's does.
 if(const int step=TabDelta(action)){action={};StepRoomArea(v,rows,nav,step,narrow);}
 RoomBoardFrame f{v,rows,nav,action,feedback,nav.Focus(),nav.Focus()!=roomBoardFocus_,menu_.bodyChoices,s,RoomFighterArt(preferences_)};
 const auto& focus=f.focus;
 const unsigned unread=transcript_.Unread(muted_);
 const auto origin=ImGui::GetCursorScreenPos();
 if(const auto table=RowKey<int>(focus,"table-");table&&*table<static_cast<int>(v.room.tables.size()))selectedTable_=*table;
 // A cabinet's table is its player's, whoever they play.
 if(const auto cabinet=RowKey<std::size_t>(focus,"cabinet-");cabinet&&*cabinet<room::CabinetCount)
  if(const auto* m=room::FindMember(v.room,v.room.cabinets[*cabinet]);m&&m->table>=0)selectedTable_=m->table;
 // B on your own table's card leaves your place there, cancels a start held
 // for a locked-in spectator, or says why a seat cannot be left yet; anywhere
 // else on the board it is the ordinary Back, so Home stays reachable without
 // giving up the place. On the keyboard Escape always goes back, and Delete
 // does what B does on your card.
 const auto place=ExitFromPlace(v);
 const bool leaveKey=action.kind==MenuAction::Shortcut&&action.delta==MenuInput::Leave;
 if(OnOwnPlace(place,focus)&&((action.kind==MenuAction::Back&&!action.keyboard)||leaveKey))action={MenuAction::Activate,"leave-place"};
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(6*s,6*s));
 ImGui::PushStyleColor(ImGuiCol_ChildBg,v2::BoardGround(v.SeeThrough()));
 const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
 v2::Box area{origin,ImVec2(origin.x+ImGui::GetContentRegionAvail().x,origin.y+height)};
 ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
 const bool showMembers=preferences_.roomShowMembers,showChat=preferences_.roomShowChat;
 // The narrow board's tab on show: the focused row's.
 int part=BoardTabOf(BoardAreaOf(focus),true);
 if(narrow){
  ImGui::SetCursorScreenPos(area.min);
  const std::vector<std::string> labels{loc::T("room.v2.board"),std::string(loc::T("room.members"))+" "+std::to_string(v.room.members.size()),
   std::string(loc::T("room.chat"))+(unread?" "+std::to_string(unread):std::string())};
  // A tab the pointer pressed goes where LB and RB would, and shows at once.
  const int clicked=v2::Tabs(labels,part);
  if(clicked>=0&&clicked!=part&&!nav.Choosing()&&FocusRoomTab(v,rows,nav,clicked,true)){
   f.focus=nav.Focus();f.focusChanged=true;part=clicked;
  }
  area.min.y=ImGui::GetCursorScreenPos().y;
 }
 v2::Box tables=area,members=area,chat=area;
 if(ultra){
  members=v2::CutLeft(tables,(showMembers?480:FoldedColumnWidth)*s,24*s);chat=v2::CutRight(tables,(showChat?520:FoldedColumnWidth)*s,24*s);
  if(!showMembers)members.max.y=members.min.y+FoldedPanelHeight*s;if(!showChat)chat.max.y=chat.min.y+FoldedPanelHeight*s;
 }
 // The watched game's picture between at and to, with what is watched and Hide on it;
 // the corner window stays away this frame.
 const auto showPicture=[&](ImVec2 at,ImVec2 to){
  d->AddImage(v.watchedPicture,at,to);d->AddRect(at,to,c.border,0,0,s);
  const auto watched=std::find_if(v.room.tables.begin(),v.room.tables.end(),[&](const room::Table& t){return std::find(t.spectators.begin(),t.spectators.end(),v.room.localMember)!=t.spectators.end();});
  // A follower's says so, since the table it shows changes by itself.
  if(watched!=v.room.tables.end())v2::Chip(d,ImVec2(at.x+8*s,at.y+8*s),follow_.On()?loc::Tf("room.follow.following",watched->id+1):
   std::string(loc::T("room.status.watching"))+" \xC2\xB7 "+loc::Tf("room.v2.table",watched->id+1),c.accent);
  boardPicture_=true;
  if(const auto* hide=f.Find("room-hide-picture"))f.HideButton(d,*hide,ImVec2(to.x-8*s,at.y+8*s));
 };
 // The Small picture stands at the column's top, over Members.
 columnPicture_=!narrow&&!ultra&&v.watchedPicture&&preferences_.pictureMode==netplay::PictureSmall;
 v2::Box picture{};
 if(!ultra&&!narrow)ArrangeRoom(area,tables,members,chat,&picture);
 if(columnPicture_){f.Panel(d,picture);const v2::Box in=v2::Inset(picture,12*s,12*s);showPicture(in.min,in.max);}
 if(!narrow||part==0){
  // The tables, under their heading and how many play and watch.
  std::size_t playing=0,watching=0;bool banners=false;
  for(const auto& t:v.room.tables){playing+=t.phase==room::TablePhase::Playing;watching+=t.spectators.size()+t.watchingNext.size();banners|=!DescribeTableBanner(v,t).text.empty();}
  const float headingWidth=v2::SectionHeading(d,tables.min,loc::T("room.v2.tables"),c.muted);
  const std::string summary=loc::Tf("room.v2.tables_summary",playing,watching);
  const float summaryWidth=ImGui::GetFont()->CalcTextSizeA(15*s,FLT_MAX,0,summary.c_str()).x;
  d->AddText(ImGui::GetFont(),15*s,ImVec2(tables.max.x-summaryWidth,tables.min.y),c.muted,summary.c_str());
  d->AddLine(ImVec2(tables.min.x+headingWidth+12*s,tables.min.y+9*s),ImVec2(tables.max.x-summaryWidth-12*s,tables.min.y+9*s),c.border,s);
  v2::CutTop(tables,20*s,12*s);
  // The room's actions along the bottom, then the focused row's line over them.
  const RoomActionBar bar(f,tables.W());
  const auto cards=ShapeTableCards(tables.W(),ultra||logical>=1640?3:narrow&&tables.W()<640*s?1:2,preferences_.roomTableGrid,banners,s);
  const float cardHeight=cards.height;
  // The bar stays along the bottom while the floor fits over it and the
  // focused row's line: a row of cards, or an Arcade room's whole floor. On a
  // shorter board it follows the floor in its scroll.
  const float floorHeight=v.room.arcade?ArcadeBoardHeight(v,rows,tables.W()):cardHeight;
  const bool follows=tables.H()-bar.Height(tables.W())-38*s<floorHeight;
  // A board that starts or stops scrolling, or changes size, can leave the
  // focused row out of sight with the focus unchanged: it scrolls back into
  // view as it does when the focus moves. A scroll of the player's own stays
  // put between.
  const RoomTablesShape shape{follows,tables.W(),tables.H()};
  if(!(shape==roomTablesShape_)){f.focusChanged=true;roomTablesShape_=shape;}
  if(!follows){const float width=tables.W();bar.Draw(d,v2::CutBottom(tables,bar.Height(width),12*s).min,width,tables);}
  ImGui::SetCursorScreenPos(v2::CutBottom(tables,18*s,8*s).min);
  DrawFocusedLine(f,13*s);
  // Watching with the game as a picture: the picture, cinema style, over one row of cards,
  // as large as the tables' place allows (the column grip widens it).
  const auto* vp=ImGui::GetMainViewport();
  const float ph=(std::min)(tables.W()*vp->Size.y/vp->Size.x,tables.H()-cardHeight-8*s);
  if(v.watchedPicture&&preferences_.pictureMode==netplay::PictureBig&&!narrow&&ph>=120*s){
   const float pw=ph*vp->Size.x/vp->Size.y;const v2::Box pic=v2::CutTop(tables,ph,8*s);
   const ImVec2 at(pic.min.x+(pic.W()-pw)*.5f,pic.min.y),to(at.x+pw,pic.max.y);
   showPicture(at,to);
  }
  if(v.room.arcade){
   ImGui::SetCursorScreenPos(tables.min);
   if(!follows)DrawArcadeBoard(v,rows,nav,action,ImVec2(tables.W(),tables.H()),v.room.arcade,false,f.focusChanged);
   else{
    // The floor at its least height in a scroll of its own, the bar after it, as DrawTableCards does.
    ImGui::BeginChild("Arcade floor",ImVec2(tables.W(),tables.H()),0,ImGuiWindowFlags_NoNavInputs);
    const ImVec2 start=ImGui::GetCursorScreenPos();
    const float width=ImGui::GetContentRegionAvail().x,boardHeight=ArcadeBoardHeight(v,rows,width);
    DrawArcadeBoard(v,rows,nav,action,ImVec2(width,boardHeight),v.room.arcade,true,f.focusChanged);
    bar.Draw(ImGui::GetWindowDrawList(),ImVec2(start.x,start.y+boardHeight+12*s),width,tables);
    ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(width,boardHeight+12*s+bar.Height(width)));
    ImGui::EndChild();
   }
  }
  else DrawTableCards(f,tables,cards,place,follows?&bar:nullptr);
 }
 // In the narrow tabs a folded panel is its heading alone.
 if(!narrow||part==1){
  if(narrow&&!showMembers)members.max.y=members.min.y+FoldedPanelHeight*s;
  DrawRoomMembersPanel(f,members,showMembers);
 }
 if(!narrow||part==2){
  if(narrow&&!showChat)chat.max.y=chat.min.y+FoldedPanelHeight*s;
  DrawRoomChatPanel(f,chat,showChat,unread);
 }
 if(!narrow&&!ultra)RoomGrips(area,v.CanEditRoomSettings());
 columnPicture_=false;
 ImGui::PopStyleColor();
 ImGui::PopStyleColor();ImGui::PopStyleVar();roomBoardFocus_=nav.Focus();
}

} }
