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
#include "FighterSelector.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
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
 const auto* fighter=look.picked?selection::FindFighter(look.fighter):nullptr;
 if(!fighter)return loc::T("room.fighter_not_shared");
 const std::string name=look.random?loc::Tf("selection.random_fighter",fighter->name):std::string(fighter->name);
 if(look.costume<0||look.ultra<0)return name;
 selection::Pick pick;pick.fighter=look.fighter;pick.costume=look.costume;pick.color=(std::max)(0,look.color);
 return loc::Tf("room.seat_look",name,ShortCostumeLabel(pick),brief?ShortUltraLabel(look.ultra):UltraLabel(look.ultra));
}

namespace {
// The room's action bar: the board's Bar rows, every row the floor and the
// column do not draw (BoardAreaOf), as buttons on as few lines as their labels allow (v2::BarLines),
// each place at least 140 px.
class RoomActionBar {
public:
 RoomActionBar(const RoomBoardFrame& f):f_(f){
  for(const auto& e:f.rows)if(BoardAreaOf(e.id)==BoardArea::Bar)buttons_.push_back(&e);
  for(const auto* e:buttons_)places_.push_back((std::max)(140*f.s,Need(*e)));
 }
 // The bar's height `width` wide.
 float Height(float width) const {return (v2::BarLines(width,places_,12*f_.s)*60-12)*f_.s;}
 // The bar `width` wide from `at`, drawn in `d`; a focused popout's choices open over `column`.
 void Draw(ImDrawList* d,ImVec2 at,float width,const v2::Box& column) const {
  const float s=f_.s;
  const int lines=v2::BarLines(width,places_,12*s);
  const auto places=v2::BarPlaces({at,ImVec2(at.x+width,at.y+Height(width))},places_,12*s,lines,48*s);
  for(std::size_t i=0;i<buttons_.size();++i){
   const auto& e=*buttons_[i];const v2::Box& b=places[i];
   const bool hovered=f_.Press(e,b),focused=e.id==f_.focus,leave=e.id=="leave"||e.id=="stop-watching";
   // Ready is the one filled button; Leave room is drawn in the danger tone.
   v2::Button(d,b,e.label,e.id=="ready"?v2::ButtonKind::Primary:leave?v2::ButtonKind::Danger:v2::ButtonKind::Secondary,focused,hovered,f_.feedback.Enabled(e));
   if(focused&&e.popout)f_.choices={b,column};
  }
 }
private:
 // What a button's label needs: v2::Button's own 18 px each side.
 float Need(const MenuEntry& e) const {return HeadingFont()->CalcTextSizeA(17*f_.s,FLT_MAX,0,e.label.c_str()).x+36*f_.s;}
 const RoomBoardFrame& f_;
 std::vector<const MenuEntry*> buttons_;
 std::vector<float> places_;
};

// The board's one line for the focused row's explanation, where it says
// something the board does not show: never a table card's own first line (who
// sits, its state, its counts), which the card is. Lines after the first (the
// fighter summary under Ready, for example) and any elided text are reachable
// on hover.
void DrawFocusedLine(const RoomBoardFrame& f,float size) {
 // The focus now: the narrow board's tabs may have just moved it.
 const auto* e=f.Find(f.nav.Focus().c_str());
 if(!e)return;
 std::string elided;
 // What the entry's open choice says of its highlighted option beyond the
 // option's own label, which the strip shows; on a card that offers a place,
 // the same of the offer it lights.
 const auto& nav=f.nav;
 const MenuChoice* option=nav.Choosing()&&nav.DialogId()==e->id&&nav.ChoiceIndex()<e->choices.size()?&e->choices[nav.ChoiceIndex()]:nullptr;
 const auto index=RowKey<std::size_t>(e->id,"table-");
 const room::Table* table=index&&*index<f.v.room.tables.size()?&f.v.room.tables[*index]:nullptr;
 std::vector<MenuChoice> offers;
 if(!option&&table){offers=SeatChoices(f.v,*table);if(!offers.empty())option=&offers[LitOffer(offers,f.light,*table)];}
 const std::string choice=option&&option->detail!=option->label?option->detail:std::string();
 const auto newline=e->detail.find('\n');
 std::string line=!choice.empty()||table?choice:e->detail.substr(0,newline);
 // The board renders in place of the menu list, so it uses the same smoothed
 // verdict and the same wording. Presentation only: nav.Choose still gates on
 // the live entry and SendRoom still re-checks room authority at dispatch.
 const std::string reason=f.feedback.Enabled(*e)?"":loc::T(f.feedback.Pending(*e)?"room.updating":"common.unavailable");
 if(!reason.empty())line=line.empty()?reason:line+"  -  "+reason;
 if(line.empty())return;
 NoteDetailText(e->detail.substr(0,newline),e->detailText);
 // An offer the room refuses says why in the same tone as a refused row.
 const bool refused=!reason.empty()||(option&&!option->enabled);
 const auto p=ImGui::GetCursorScreenPos();const float lineWidth=ImGui::GetContentRegionAvail().x;
 ReportMenuCard("room-line",p,ImVec2(p.x+lineWidth,p.y+size));
 DrawCardText(elided,p,lineWidth,line,size,refused?ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)):v2::Colors().muted);
 if(newline!=std::string::npos)elided+=(elided.empty()?"":"\n")+e->detail;
 ImGui::Dummy(ImVec2(lineWidth,ImGui::GetFontSize()));
 FlushCardTip(elided);
}

// What every table shares, said once at the heading: the room's own rules and
// the first table's round rules, which a card repeats only where its own differ.
std::string SharedRulesText(const ShellView& v) {
 std::string text;
 const auto add=[&](const std::string& part){text+=(text.empty()?"":"  \xC2\xB7  ")+part;};
 if(v.room.practice)add(loc::T("room.table_footer_practice"));
 if(v.room.delayPolicy==room::DelayPolicy::Own)add(loc::T("room.table_footer_own_delay"));
 if(v.room.wiredOnly)add(loc::T("room.table_footer_wired_only"));
 add(TableRulesText(v,v.room.tables[0]));
 return text;
}

// Your own place's strip: Ready and Leave, or the leave question's answers
// while it is asked. A strip option is half a card wide, so Unready drops its
// "unlock fighter".
std::vector<TableCardOption> StripOptions(const RoomBoardFrame& f,const MenuEntry& e,const room::Table& t,bool choosing,const PlaceExit& place) {
 if(choosing){
  std::vector<TableCardOption> answers;
  for(const auto& c:e.choices)answers.push_back({c.label,c.enabled});
  return answers;
 }
 const auto mine=room::PlaceOf(f.v.room,f.v.room.localMember);
 const bool seated=mine.kind==room::Place::Kind::Seat;
 const auto ready=seated?DescribeReady(f.v,t,mine.seat):ReadyControl{};
 return {{ready.kind==ReadyControl::Unready?std::string(loc::T("room.unready.short")):e.hint,f.feedback.Enabled(e)&&(!seated||ready.kind!=ReadyControl::None)},
  {place.label,place.allowed}};
}

// A table card as the board draws it this frame: its row and table, what it
// offers this member (SeatChoices) and which of them it lights, its banner,
// and whether it is the member's own place.
struct TableCard {
 const MenuEntry* e;
 const room::Table* t;
 std::vector<MenuChoice> offers;
 std::size_t lit;
 TableBanner banner;
 bool own;
 // What it holds, for its height (TableCardShape.hxx).
 TableCardContent Content() const {
  const bool quiet=!t->p1&&!t->p2&&t->queue.empty()&&t->spectators.empty()&&t->watchingNext.empty()&&!own;
  return {quiet,!quiet&&!banner.text.empty(),own};
 }
};
std::vector<TableCard> TableCardsOf(const RoomBoardFrame& f,const PlaceExit& place) {
 std::vector<TableCard> cards;
 for(const auto& e:f.rows){
  const auto index=RowKey<std::size_t>(e.id,"table-");
  if(!index||*index>=f.v.room.tables.size())continue;
  const auto& t=f.v.room.tables[*index];
  TableCard tc{&e,&t,SeatChoices(f.v,t),0,DescribeTableBanner(f.v,t),place.table==static_cast<int>(t.id)};
  tc.lit=LitOffer(tc.offers,f.light,t);
  cards.push_back(std::move(tc));
 }
 return cards;
}

// A press on a table card that offers this member a place: Left and Right
// move its light along the offers and, past the first or the last, go on to
// the row that way as from any row (so a grid's next card and the Members
// column stay one press away); Select sends the offer the board last drew
// lit, or nothing when the card no longer offers it (a seat taken under the
// light: the card lights what is left first, and the next Select takes that).
void TakeTableOffer(RoomBoardFrame& f) {
 const auto kind=f.action.kind;
 if(kind!=MenuAction::Adjust&&kind!=MenuAction::Activate)return;
 const auto index=RowKey<std::size_t>(f.action.id,"table-");
 if(!index||*index>=f.v.room.tables.size())return;
 const auto& t=f.v.room.tables[*index];
 const auto offers=SeatChoices(f.v,t);
 if(offers.empty())return;
 if(kind==MenuAction::Adjust){
  const int to=static_cast<int>(LitOffer(offers,f.light,t))+f.action.delta;
  if(to>=0&&to<static_cast<int>(offers.size()))f.light={static_cast<int>(t.id),offers[static_cast<std::size_t>(to)].id};
  else if(f.nav.MoveByPlace(f.action.delta<0?-1:1,0,f.rows)){f.focus=f.nav.Focus();f.focusChanged=true;}
  f.action={};
  return;
 }
 const auto shown=std::find_if(offers.begin(),offers.end(),[&](const MenuChoice& o){return f.light.table==static_cast<int>(t.id)&&o.id==f.light.offer;});
 const std::string id=f.action.id;
 f.action=shown!=offers.end()&&shown->enabled?MenuAction{MenuAction::Chosen,id,shown->id}:MenuAction{};
}

// A button a card draws for one of its offers: lit (what the pad's Select
// takes) or under the pointer on the hover tone in the text's ink, at rest on
// the button's in muted ink, flat while the room refuses it. Lit is told by
// fill and ink alone: the card's ring is the focus's only one. With `plus`, a
// plus before the label (an open seat).
void DrawOfferButton(const RoomBoardFrame& f,ImDrawList* d,const v2::Box& b,const MenuChoice& offer,bool lit,bool over,bool plus,float size,std::string& elided) {
 const float s=f.s;const auto& c=v2::Colors();
 const bool live=offer.enabled,bright=live&&(lit||over);
 const ImU32 ink=bright?c.text:c.muted;
 f.Ground(d,b,!live?c.frame:bright?c.hover:c.button,live?v2::color::Alpha(c.accent,.35f):c.border);
 const float mark=plus?18*s:0,room=b.W()-16*s-mark;
 const float w=(std::min)(room,ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,offer.label.c_str()).x);
 const float x=b.min.x+(b.W()-w-mark)*.5f,mid=(b.min.y+b.max.y)*.5f;
 if(plus){
  const float arm=5*s,cx=x+arm;
  d->AddLine(ImVec2(cx-arm,mid),ImVec2(cx+arm,mid),ink,2*s);d->AddLine(ImVec2(cx,mid-arm),ImVec2(cx,mid+arm),ink,2*s);
 }
 DrawCardText(elided,ImVec2(x+mark,mid-size*.5f),w,offer.label,size,ink);
}
// An open seat this member cannot take (they have a place, or the queue goes
// first): its frame and "Open seat", muted.
void DrawOpenSeat(const RoomBoardFrame& f,ImDrawList* d,const v2::Box& b,std::string& elided) {
 const auto& c=v2::Colors();
 f.Ground(d,b,v2::color::Alpha(c.frame,.55f),c.border);
 DrawCardText(elided,ImVec2(b.min.x+8*f.s,(b.min.y+b.max.y-15*f.s)*.5f),b.W()-16*f.s,loc::T("room.open_seat"),15*f.s,c.muted,true);
}

// A taken seat in `box`: the fighter's picture, as tall as the box, at its
// outer end (the right one for a versus card's second seat), and in the rest
// the player's tag over their fighter, costume and Ultra, with Ready on its
// inner side while they are ready. Where the box is tall enough a third line
// says the player's win streak at this table, else how long they have been
// idle, as the Classic card did. A versus card centres the lines between the
// picture and the score, so the pair stands even about it; a stacked seat
// starts them beside the picture. The picture is the fighter they picked,
// else their main (SeatLookOf).
void DrawSeat(const RoomBoardFrame& f,ImDrawList* d,const v2::Box& box,const room::Table& t,int side,bool versus,bool marked,std::string& elided) {
 const auto& v=f.v;const float s=f.s,face=box.H(),gap=table_card::FaceGap*s;const auto& c=v2::Colors();
 const auto* m=room::FindMember(v.room,side?t.p2:t.p1);
 if(!m)return;
 const bool mirrored=versus&&side==1;
 const v2::Box tile=mirrored?v2::Box{ImVec2(box.max.x-face,box.min.y),box.max}:v2::Box{box.min,ImVec2(box.min.x+face,box.max.y)};
 const float left=mirrored?box.min.x:tile.max.x+gap,right=mirrored?tile.min.x-gap:box.max.x,room=right-left;
 const auto widthOf=[](const std::string& value,float size){return ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,value.c_str()).x;};
 // Where a line `width` wide starts: centred on a versus card, else at the start.
 const auto lineAt=[&](float width){return left+(versus?(std::max)(0.f,room-width)*.5f:0.f);};
 // The lines stand close: a compact card's picture holds all three.
 const float tagLine=v2::ChipFont()+8*s,lineSize=13*s,lineGap=2*s;
 const auto look=SeatLookOf(v,*m);
 // The third line: a streak takes it from idle time.
 const std::string streak=StreakText(t,m->id),note=streak.empty()?IdleText(*m,v.roomElapsedMs):streak;
 const bool third=!note.empty()&&face>=tagLine+2*(lineGap+lineSize);
 // The lines together, centred on the picture.
 const float block=tagLine+lineGap+lineSize+(third?lineGap+lineSize:0),top=box.min.y+(std::max)(0.f,(face-block)*.5f);
 DrawCharacterPortrait(SeatPicture(look,f.art),tile.min,tile.max,c.frame);
 if(const auto points=PointsOf(v,*m,look.fighter))v2::LevelBand(d,tile,points->points);
 if(marked)d->AddRect(ImVec2(tile.min.x-2*s,tile.min.y-2*s),ImVec2(tile.max.x+2*s,tile.max.y+2*s),ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)),3*s,0,2*s);
 const auto tag=MemberTag(v,*m);
 const float tagWidth=(std::min)(room,v2::TagWidth(tag,v2::TagSize::S));
 v2::Tag(d,ImVec2(lineAt(tagWidth),top),tagWidth,tag,v2::TagSize::S);
 // The fighter, costume and Ultra, and Ready on their inner side (towards
 // the score) while the seat is ready. The Ultra's short name where the long
 // one does not fit.
 const bool ready=t.ready[side]&&(t.phase==room::TablePhase::Waiting||t.phase==room::TablePhase::Ready);
 const std::string readyWord=ready?loc::T("room.status.ready"):"";
 const float chip=ready?v2::ChipWidth(readyWord)+8*s:0,y=top+tagLine+lineGap;
 std::string name=SeatCaption(look);
 if(widthOf(name,lineSize)>room-chip)name=SeatCaption(look,true);
 const float nameWidth=(std::min)(room-chip,widthOf(name,lineSize)),x=lineAt(nameWidth+chip);
 DrawCardText(elided,ImVec2(mirrored?x+chip:x,y),room-chip,name,lineSize,c.muted);
 if(ready)v2::Chip(d,ImVec2(mirrored?x:x+nameWidth+8*s,y+(lineSize-v2::ChipFont()-8*s)*.5f),readyWord,palette::Ready);
 if(third){
  const float w=(std::min)(room,widthOf(note,lineSize));
  DrawCardText(elided,ImVec2(lineAt(w),y+lineSize+lineGap),room,note,lineSize,streak.empty()?c.muted:c.accent);
 }
}

// The floor's scroll lands on a card's top, or on its end. The focus, when it
// has just moved onto a row in this scroll (`reveal`), shows whole with the
// least move; otherwise a scroll of the player's own (the wheel, a dragged
// bar) settles on the nearest once let go.
void SettleOnCard(const RoomBoardFrame& f,const TableCardPlaces& places,ImVec2 start,bool reveal) {
 const float most=ImGui::GetScrollMaxY(),now=ImGui::GetScrollY(),view=ImGui::GetWindowHeight();
 if(most<=0||(!reveal&&ImGui::IsAnyItemActive()))return;
 std::vector<float> edges;
 for(const float top:places.tops)if(top<most&&(edges.empty()||top>edges.back()))edges.push_back(top);
 edges.push_back(most);
 const MenuRect* focus=reveal?f.nav.PlacedNow(f.nav.Focus()):nullptr;
 const float y0=focus?focus->y0-start.y:0,y1=focus?focus->y1-start.y:0;
 float best=now,move=FLT_MAX;
 for(const float edge:edges){
  if(focus&&(y0<edge-.5f||y1>edge+view+.5f))continue;
  if(std::fabs(edge-now)<move){best=edge;move=std::fabs(edge-now);}
 }
 // A focus taller than the view shows from its top.
 if(focus&&move==FLT_MAX)for(const float edge:edges)if(edge<=y0+.5f)best=edge;
 // Set even where it stands, so a focus row's own centring (PressEntry) does not take it mid-card.
 if(reveal||std::fabs(best-now)>.5f)ImGui::SetScrollY(best);
}

// A Classic room's floor in `tables`: `cards` where `places` stands them
// (TableCardShape.hxx), in their own scroll when they do not fit; with
// `follows`, the action bar after them in that scroll.
//  A versus card:  TABLE 1 [phase] name                  rules  Queue 1 · 2 watching
//                  [face] name, fighter, streak   2 - 1   name, fighter, streak [face]
//                  (your own place: Ready | Leave seat)
//  A stacked card: TABLE 1 [phase] name                          counts     2 - 1
//                  [face] name, fighter
//                  [face] name, fighter
//  A quiet card:   TABLE 3 [Open]   [+ Play as P1]   [+ Play as P2]
// A full table's Join queue and Watch stand at its head's right while it is focused.
void DrawTableCards(const RoomBoardFrame& f,const v2::Box& tables,const TableCardShape& shape,const std::vector<TableCard>& cards,
                    const TableCardPlaces& places,const PlaceExit& place,const RoomActionBar* follows) {
 namespace card=table_card;
 const auto& v=f.v;const auto& nav=f.nav;const float s=f.s;const auto& c=v2::Colors();
 const auto pointer=ImGui::GetIO().MouseDelta;const bool pointerMoved=pointer.x!=0||pointer.y!=0;
 const int columns=shape.columns;
 std::string elided;
 const auto text=[&](ImVec2 p,float width,const std::string& value,float size,ImU32 color){DrawCardText(elided,p,width,value,size,color);};
 const auto widthOf=[](const std::string& value,float size){return ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,value.c_str()).x;};
 const std::string sharedRules=TableRulesText(v,v.room.tables[0]);
 ImGui::SetCursorScreenPos(tables.min);
 ImGui::BeginChild("Room tables",ImVec2(tables.W(),tables.H()),0,ImGuiWindowFlags_NoNavInputs);
 auto* td=ImGui::GetWindowDrawList();
 const auto start=ImGui::GetCursorScreenPos();
 // Across what the scroll leaves beside its bar, while it shows one.
 const float floorWidth=ImGui::GetContentRegionAvail().x,cardWidth=(floorWidth-(columns-1)*card::Between*s)/columns;
 // The focus is a row of this scroll's: a card, or the bar when it follows them.
 bool reveal=f.focusChanged&&follows&&BoardAreaOf(f.focus)==BoardArea::Bar;
 TableLight shown;
 for(std::size_t n=0;n<cards.size();++n){
  const auto& tc=cards[n];const auto& e=*tc.e;const auto& t=*tc.t;
  const ImVec2 at(start.x+static_cast<float>(static_cast<int>(n)%columns)*(cardWidth+card::Between*s),start.y+places.tops[n]);
  const v2::Box b{at,ImVec2(at.x+cardWidth,at.y+places.heights[n])};
  const bool focused=e.id==f.focus,choosing=nav.Choosing()&&nav.DialogId()==e.id&&!e.choices.empty();
  const bool quiet=tc.Content().quiet,line=quiet&&shape.quietLine,versus=shape.versus;
  if(focused)reveal=reveal||f.focusChanged;
  // The set's score once both seats are taken, else vs; a closed or quiet table neither.
  const bool paired=t.p1&&t.p2;
  const std::string score=quiet||t.phase==room::TablePhase::Closed?std::string():paired?SetScoreText(t.score):"vs";
  const float scoreSize=22*s,scoreWidth=score.empty()?0:HeadingFont()->CalcTextSizeA(scoreSize,FLT_MAX,0,score.c_str()).x;
  // The head's line: a quiet line's centred in its height.
  const float y=b.min.y+(line?(card::Quiet-card::Head)*.5f*s:0);
  const std::string heading=loc::Tf("room.v2.table",t.id+1),phase=PhaseName(t);
  const float headWidth=HeadingFont()->CalcTextSizeA(14*s,FLT_MAX,0,heading.c_str()).x+8*s+v2::ChipWidth(phase)+10*s;
  // The seats: a quiet line's beside its head, else under it; a versus
  // card's at its two ends with the score between them, a stacked card's one
  // over the other.
  const float seatTop=b.min.y+card::Head*s,inner=b.W()-2*card::Pad*s;
  const std::string rules=TableRulesText(v,t),counts=TableCountsText(t);
  v2::Box seats[2];
  if(quiet){
   const float lead=line?(std::max)({card::QuietHeadLeast*s,headWidth+12*s,t.name.empty()&&rules==sharedRules?0.f:inner*.4f}):0;
   const float top=line?b.min.y+(card::Quiet-card::QuietButton)*.5f*s:seatTop,left=b.min.x+card::Pad*s+lead,half=(b.max.x-card::Pad*s-left-card::Between*s)*.5f;
   seats[0]={ImVec2(left,top),ImVec2(left+half,top+card::QuietButton*s)};
   seats[1]={ImVec2(left+half+card::Between*s,top),ImVec2(b.max.x-card::Pad*s,top+card::QuietButton*s)};
  }else if(versus){
   const float middle=(std::max)(card::Middle*s,scoreWidth+32*s),half=(inner-middle)*.5f;
   seats[0]={ImVec2(b.min.x+card::Pad*s,seatTop),ImVec2(b.min.x+card::Pad*s+half,seatTop+card::Face*s)};
   seats[1]={ImVec2(b.max.x-card::Pad*s-half,seatTop),ImVec2(b.max.x-card::Pad*s,seatTop+card::Face*s)};
  }else for(int side=0;side<2;++side){const float top=seatTop+side*(card::SeatRow+card::SeatRowGap)*s;seats[side]={ImVec2(b.min.x+card::Pad*s,top),ImVec2(b.max.x-card::Pad*s,top+card::SeatRow*s)};}
  // The head's right end: a stacked card's score, then the offers that are
  // not seats (the queue and watching) while the card is focused.
  float headEnd=b.max.x-card::Pad*s-(versus||quiet||score.empty()?0:scoreWidth+12*s);
  std::vector<v2::Box> offerBoxes(tc.offers.size());
  for(std::size_t i=tc.offers.size();i-->0;){
   const auto* option=FindSeatOption(tc.offers[i].id);
   if(option&&option->seat>=0){offerBoxes[i]=seats[option->seat];continue;}
   if(!focused)continue;
   const float w=widthOf(tc.offers[i].label,14*s)+24*s;
   offerBoxes[i]={ImVec2(headEnd-w,y+4*s),ImVec2(headEnd,y+(card::Head-4)*s)};
   headEnd-=w+card::Between*s;
  }
  // Your own place's strip at the foot. Its buttons go in first, then the
  // offers': the first item under the pointer is the one it is over.
  const auto options=tc.own?StripOptions(f,e,t,choosing,place):std::vector<TableCardOption>{};
  const v2::Box strip{ImVec2(b.min.x+card::Pad*s,b.max.y-(card::Foot+card::StripButton)*s),ImVec2(b.max.x-card::Pad*s,b.max.y-card::Foot*s)};
  const float ow=options.empty()?0:(strip.W()-(options.size()-1)*card::Between*s)/options.size();
  int over=-1,pressed=-1,offerOver=-1,offerPressed=-1;
  for(std::size_t i=0;i<options.size();++i){
   ImGui::SetCursorScreenPos(ImVec2(strip.min.x+i*(ow+card::Between*s),strip.min.y));ImGui::PushID(e.id.c_str());ImGui::PushID(static_cast<int>(i));
   // The open question is itself asked, so its own answers answer it.
   if(ImGui::InvisibleButton("##option",ImVec2(ow,strip.H()))&&(choosing||!nav.Asking())&&!nav.Editing())pressed=static_cast<int>(i);
   if(ImGui::IsItemHovered())over=static_cast<int>(i);
   ImGui::PopID();ImGui::PopID();
   ReportMenuCard((e.id+"/"+std::to_string(i)).c_str(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
  }
  for(std::size_t i=0;i<tc.offers.size();++i){
   const v2::Box& o=offerBoxes[i];
   if(o.W()<=0)continue;
   ImGui::SetCursorScreenPos(o.min);ImGui::PushID(e.id.c_str());ImGui::PushID(tc.offers[i].id.c_str());
   if(ImGui::InvisibleButton("##offer",ImVec2(o.W(),o.H()))&&!nav.Asking()&&!nav.Editing())offerPressed=static_cast<int>(i);
   if(ImGui::IsItemHovered())offerOver=static_cast<int>(i);
   ImGui::PopID();ImGui::PopID();
   ReportMenuCard((e.id+"/"+tc.offers[i].id).c_str(),o.min,o.max);
  }
  // The card itself: a press focuses it where its own buttons are its actions.
  const bool hovered=f.Press(e,b,!OnOwnPlace(place,e.id)&&tc.offers.empty());
  if(choosing){
   // The chosen answer follows the mouse, but only a moving one takes the choice.
   if(over>=0&&pointerMoved)f.nav.ChoiceIndex(static_cast<std::size_t>(over));
   if(pressed>=0)f.action=f.nav.Pick(static_cast<std::size_t>(pressed),f.rows);
  }else if(pressed==0){f.nav.Focus(e.id,f.rows);f.action=f.nav.Choose(f.rows);}
  else if(pressed==1)f.action={MenuAction::Activate,"leave-place"};
  // The pointer lights the offer it moves onto and focuses its card; a
  // resting one leaves the pad's light. A press sends the offer it is on.
  std::size_t lit=tc.lit;
  if(offerOver>=0&&pointerMoved&&!nav.Choosing()){lit=static_cast<std::size_t>(offerOver);f.nav.Focus(e.id,f.rows);}
  if(offerPressed>=0&&tc.offers[static_cast<std::size_t>(offerPressed)].enabled){
   lit=static_cast<std::size_t>(offerPressed);f.nav.Focus(e.id,f.rows);
   f.action={MenuAction::Chosen,e.id,tc.offers[lit].id};
  }
  const bool lightOn=!tc.offers.empty()&&f.nav.Focus()==e.id;
  if(lightOn)shown={static_cast<int>(t.id),tc.offers[lit].id};
  // The strip is the question, as it stands after this frame's presses: the
  // menu draws no panel of the same choices over the board.
  const bool chooserOpen=nav.Choosing()&&nav.DialogId()==e.id&&!e.choices.empty();
  if(chooserOpen)f.choices.inPlace={e.choices.size(),over>=0};
  // An open question keeps its card in sight: scrolled away, its strip would take a press unseen.
  if(!ImGui::IsItemVisible()){reveal=reveal||chooserOpen;continue;}
  v2::CardGround(td,b,focused,hovered,tc.own);
  // TABLE n, its state, and the host's name for it.
  float x=b.min.x+card::Pad*s;
  x+=v2::SectionHeading(td,ImVec2(x,y+9*s),heading,c.muted)+8*s;
  const ImU32 tone=t.phase==room::TablePhase::Playing?c.accent:t.phase==room::TablePhase::Idle||t.phase==room::TablePhase::Closed?c.muted:
   ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
  x+=v2::Chip(td,ImVec2(x,y+6*s),phase,tone)+10*s;
  // At the head's right, what this table does not share with the room: its
  // rules where they differ, who waits and who watches. A quiet line's go
  // before its seats.
  const float end=quiet&&line?seats[0].min.x-12*s:headEnd;
  std::string differs=rules==sharedRules?std::string():rules;
  if(!counts.empty())differs+=(differs.empty()?"":"  \xC2\xB7  ")+counts;
  const float differsWidth=differs.empty()?0:(std::min)((std::max)(0.f,end-x),widthOf(differs,13*s));
  if(differsWidth>0)text(ImVec2(end-differsWidth,y+10*s),differsWidth,differs,13*s,c.muted);
  if(!t.name.empty()){NoteUserText(t.name);text(ImVec2(x,y+8*s),end-x-(differsWidth>0?differsWidth+12*s:0),t.name,15*s,c.text);}
  if(!score.empty()){
   const ImU32 scoreTone=paired?c.accent:c.muted;
   // A versus card's score in the middle between the seats, at the height of
   // the players' names; a stacked card's at the head's right.
   if(versus)td->AddText(HeadingFont(),scoreSize,ImVec2(seats[0].max.x+(seats[1].min.x-seats[0].max.x-scoreWidth)*.5f,seatTop+(card::Face*s-scoreSize)*.5f),scoreTone,score.c_str());
   else td->AddText(HeadingFont(),scoreSize,ImVec2(b.max.x-card::Pad*s-scoreWidth,y+5*s),scoreTone,score.c_str());
  }
  // The seats: a player, an offer (lit while the pad's Select takes it), or an open seat this member cannot take.
  for(int side=0;side<2;++side){
   if(room::FindMember(v.room,side?t.p2:t.p1)){DrawSeat(f,td,seats[side],t,side,versus,tc.banner.seat==side,elided);continue;}
   const auto offered=std::find_if(tc.offers.begin(),tc.offers.end(),[&](const MenuChoice& o){const auto* option=FindSeatOption(o.id);return option&&option->seat==side;});
   if(offered==tc.offers.end()){DrawOpenSeat(f,td,seats[side],elided);continue;}
   const auto i=static_cast<std::size_t>(offered-tc.offers.begin());
   DrawOfferButton(f,td,seats[side],*offered,lightOn&&i==lit,offerOver==static_cast<int>(i),true,15*s,elided);
  }
  for(std::size_t i=0;i<tc.offers.size();++i){
   const auto* option=FindSeatOption(tc.offers[i].id);
   if(offerBoxes[i].W()>0&&!(option&&option->seat>=0))DrawOfferButton(f,td,offerBoxes[i],tc.offers[i],lightOn&&i==lit,offerOver==static_cast<int>(i),false,14*s,elided);
  }
  if(!tc.banner.text.empty())text(ImVec2(b.min.x+card::Pad*s,seats[1].max.y+3*s),inner,tc.banner.text,13*s,ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)));
  for(std::size_t i=0;i<options.size();++i){
   const v2::Box o{ImVec2(strip.min.x+i*(ow+card::Between*s),strip.min.y),ImVec2(strip.min.x+i*(ow+card::Between*s)+ow,strip.max.y)};
   const bool chosen=choosing&&i==nav.ChoiceIndex();
   f.Ground(td,o,chosen?c.header:over==static_cast<int>(i)?c.hover:c.button,v2::color::Alpha(c.accent,.35f));
   DrawCardText(elided,ImVec2(o.min.x+8*s,o.min.y+(o.H()-15*s)*.5f),ow-16*s,options[i].label,15*s,options[i].live?c.text:c.muted,true);
   if(chosen)v2::Focus(td,o.min,o.max,false);
  }
  // Flushed once per card, while the card's button is still the hovered item.
  FlushCardTip(elided);
  if(focused)v2::Focus(td,b.min,b.max);
 }
 f.light=shown;
 float bottom=start.y+places.height;
 if(follows){follows->Draw(td,ImVec2(start.x,bottom+12*s),floorWidth,tables);bottom+=12*s+follows->Height(floorWidth);}
 ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(floorWidth,bottom-start.y));
 SettleOnCard(f,places,start,reveal);
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
 RoomBoardFrame f{v,rows,nav,action,feedback,nav.Focus(),nav.Focus()!=roomBoardFocus_,menu_.bodyChoices,s,RoomFighterArt(preferences_),tableLight_};
 if(!room::RulesOf(v.room).cabinets)TakeTableOffer(f);
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
  // The tables, under their heading, what they all play by and how many play and watch.
  std::size_t playing=0,watching=0;
  for(const auto& t:v.room.tables){playing+=t.phase==room::TablePhase::Playing;watching+=t.spectators.size()+t.watchingNext.size();}
  const float headingWidth=v2::SectionHeading(d,tables.min,loc::T("room.v2.tables"),c.muted);
  // The rules give way first where the heading's line holds only the counts.
  const auto widthAt15=[&](const std::string& text){return ImGui::GetFont()->CalcTextSizeA(15*s,FLT_MAX,0,text.c_str()).x;};
  const float summaryRoom=(std::max)(0.f,tables.W()-headingWidth-24*s);
  std::string summary=loc::Tf("room.v2.tables_summary",playing,watching);
  if(!room::RulesOf(v.room).cabinets)
   if(const std::string ruled=SharedRulesText(v)+"  \xC2\xB7  "+summary;widthAt15(ruled)<=summaryRoom)summary=ruled;
  const float summaryWidth=(std::min)(summaryRoom,widthAt15(summary));
  v2::Text(d,ImVec2(tables.max.x-summaryWidth,tables.min.y),summaryWidth,summary,15*s,c.muted);
  d->AddLine(ImVec2(tables.min.x+headingWidth+12*s,tables.min.y+9*s),ImVec2(tables.max.x-summaryWidth-12*s,tables.min.y+9*s),c.border,s);
  v2::CutTop(tables,20*s,12*s);
  // The room's actions along the bottom, then the focused row's line over them.
  const RoomActionBar bar(f);
  const auto cardShape=ShapeTableCards(tables.W(),ultra||logical>=1640?3:narrow&&tables.W()<640*s?1:2,preferences_.roomTableGrid,s);
  const auto tableCards=TableCardsOf(f,place);
  std::vector<TableCardContent> contents;
  for(const auto& tc:tableCards)contents.push_back(tc.Content());
  const auto places=PlaceTableCards(cardShape,contents);
  // A row of cards: the tallest one standing, else a seated card's height.
  const float cardHeight=tableCards.empty()?cardShape.Height({}):places.tallest;
  // The bar stays along the bottom while the floor fits over it and the
  // focused row's line: a row of cards, or an Arcade room's whole floor. On a
  // shorter board it follows the floor in its scroll.
  const float floorHeight=room::RulesOf(v.room).cabinets?ArcadeBoardHeight(v,rows,tables.W()):cardHeight;
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
  if(room::RulesOf(v.room).cabinets){
   ImGui::SetCursorScreenPos(tables.min);
   if(!follows)DrawArcadeBoard(v,rows,nav,action,ImVec2(tables.W(),tables.H()),v.room.type,false,f.focusChanged);
   else{
    // The floor at its least height in a scroll of its own, the bar after it, as DrawTableCards does.
    ImGui::BeginChild("Arcade floor",ImVec2(tables.W(),tables.H()),0,ImGuiWindowFlags_NoNavInputs);
    const ImVec2 start=ImGui::GetCursorScreenPos();
    const float width=ImGui::GetContentRegionAvail().x,height=ArcadeBoardHeight(v,rows,width);
    DrawArcadeBoard(v,rows,nav,action,ImVec2(width,height),v.room.type,true,f.focusChanged);
    bar.Draw(ImGui::GetWindowDrawList(),ImVec2(start.x,start.y+height+12*s),width,tables);
    ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(width,height+12*s+bar.Height(width)));
    ImGui::EndChild();
   }
  }
  else DrawTableCards(f,tables,cardShape,tableCards,places,place,follows?&bar:nullptr);
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
