// A Classic room's table cards (docs/design/ROOM_TABLES.md): what each card
// holds, its buttons, the pointer on them, its drawing, and the scroll that
// lands on a card's edge. The boxes are TableCardShape.hxx's (LayOutTableCard);
// the rows, with their offers as places, are RoomPanel.cxx's (TableCardRows);
// the board around them is RoomPanelTables.cxx's.
#include "RoomPanelBoard.hxx"
#include "Theme.hxx"
#include "RoomCards.hxx"
#include "RowId.hxx"
#include "MenuProbes.hxx"
#include "LatencyMark.hxx"
#include "GameMenu.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace sf4e { namespace ui {
using namespace room_controls;
namespace {
// An offer that is a seat: it is drawn as that seat, not on the card's line.
int SeatOf(const MenuChoice& offer) {
 const auto* option=FindSeatOption(offer.id);
 return option?option->seat:-1;
}

// The buttons on a card's line: its offers that are not seats (Join queue,
// Watch); on your own place what leaves it; while the card asks a question
// (leaving a seat, ending the watched game), the question's answers.
struct CardButtons {
 enum class Kind { Offers, Place, Question } kind=Kind::Offers;
 std::vector<MenuChoice> choices;
};
CardButtons CardButtonsOf(const RoomBoardFrame& f,const TableFloor::Card& card,const PlaceExit& place) {
 const auto& e=*card.e;
 if(f.nav.Choosing()&&f.nav.DialogId()==e.id&&!e.choices.empty())return {CardButtons::Kind::Question,e.choices};
 if(card.own)return {CardButtons::Kind::Place,{{"leave-place",place.label,{},place.allowed}}};
 CardButtons offers;
 for(const auto& offer:e.segments)if(SeatOf(offer)<0)offers.choices.push_back(offer);
 return offers;
}

// What a card shows on its line, measured for its layout: TABLE n and its
// state, its name, its rules where they are not the first table's and its
// counts, and its score.
struct CardWords {
 std::string heading,phase,name,differs,score;
 TableCardInput input;
};
CardWords CardWordsOf(const RoomBoardFrame& f,const TableFloor::Card& card,const CardButtons& buttons,const v2::Box& b) {
 namespace c=table_card;
 const auto& v=f.v;const auto& t=*card.t;const float s=f.s;
 const auto width=[](const std::string& text,float size,ImFont* font){return text.empty()?0.f:font->CalcTextSizeA(size,FLT_MAX,0,text.c_str()).x;};
 CardWords w;
 w.heading=loc::Tf("room.v2.table",t.id+1);w.phase=PhaseName(t);w.name=t.name;
 const std::string rules=TableRulesText(v,t),counts=TableCountsText(t);
 w.differs=rules==TableRulesText(v,v.room.tables[0])?std::string():rules;
 if(!counts.empty())w.differs+=(w.differs.empty()?"":"  \xC2\xB7  ")+counts;
 // The set's score once both seats are taken, else vs; a quiet or closed table neither.
 if(!card.content.quiet&&t.phase!=room::TablePhase::Closed)w.score=t.p1&&t.p2?SetScoreText(t.score):"vs";
 auto& in=w.input;
 in.card={b.min.x,b.min.y,b.max.x,b.max.y};in.content=card.content;
 in.lead=width(w.heading,14*s,HeadingFont())+c::Space*s+v2::ChipWidth(w.phase)+c::Space*s;
 const float nameWidth=width(w.name,15*s,ImGui::GetFont()),differsWidth=width(w.differs,13*s,ImGui::GetFont());
 in.words=nameWidth+differsWidth+(nameWidth>0&&differsWidth>0?c::Space*s:0);
 in.score=width(w.score,22*s,HeadingFont());
 for(const auto& choice:buttons.choices)in.buttons.push_back(width(choice.label,13*s,ImGui::GetFont())+2*c::Space*s);
 return w;
}

// What the pointer did on a card's own buttons this frame: the one under it
// (moving or not), and the one pressed; -1 for none. Seats are the card's
// offers that are seats; buttons are its line's.
struct CardPointer { int seatOver=-1,seatPressed=-1,buttonOver=-1,buttonPressed=-1; };
// The card's buttons as the pointer's, before the card's own box: the first
// item under the pointer is the one it is over. A double click's second press
// is never a second request (a card that moved under it, an offer sent twice).
CardPointer PointCard(const RoomBoardFrame& f,const MenuEntry& e,const std::vector<std::pair<const MenuChoice*,v2::Box>>& seats,
                      const CardButtons& buttons,const TableCardLayout& at) {
 const auto& nav=f.nav;
 const bool second=ImGui::GetIO().MouseClickedLastCount[ImGuiMouseButton_Left]>1;
 // The open question is itself asked, so its own answers answer it.
 const bool takes=(buttons.kind==CardButtons::Kind::Question||!nav.Asking())&&!nav.Editing()&&!second;
 CardPointer pointer;
 const auto point=[&](const std::string& id,const v2::Box& box,int index,int& over,int& pressed){
  ImGui::SetCursorScreenPos(box.min);ImGui::PushID(e.id.c_str());ImGui::PushID(id.c_str());
  if(ImGui::InvisibleButton("##card-button",ImVec2(box.W(),box.H()))&&takes)pressed=index;
  if(ImGui::IsItemHovered())over=index;
  ImGui::PopID();ImGui::PopID();
  ReportMenuCard((e.id+"/"+id).c_str(),box.min,box.max);
 };
 for(std::size_t i=0;i<seats.size();++i)point(seats[i].first->id,seats[i].second,static_cast<int>(i),pointer.seatOver,pointer.seatPressed);
 for(std::size_t i=0;i<buttons.choices.size();++i){
  const auto& r=at.buttons[i];
  if(r.x1>r.x0)point(buttons.choices[i].id,v2::Box{ImVec2(r.x0,r.y0),ImVec2(r.x1,r.y1)},static_cast<int>(i),pointer.buttonOver,pointer.buttonPressed);
 }
 return pointer;
}

// What the pointer asks of the card: an offer it moves onto lights, as the
// pad's Left and Right would, and one it presses is sent (RoomAction refuses
// what the room would not take); a resting pointer leaves the light alone. On
// your own place its button leaves it; a question's answers answer it.
void TakePointer(const RoomBoardFrame& f,const MenuEntry& e,const CardPointer& pointer,const std::vector<std::pair<const MenuChoice*,v2::Box>>& seats,
                 const CardButtons& buttons) {
 auto& nav=f.nav;
 const auto moved=ImGui::GetIO().MouseDelta;const bool moving=moved.x!=0||moved.y!=0;
 const auto offer=[&](const MenuChoice& choice,bool pressed){
  if(moving||pressed)nav.Light(e.id,choice.id,f.rows);
  if(pressed&&choice.enabled)f.action={MenuAction::Chosen,e.id,choice.id};
 };
 if(pointer.seatOver>=0)offer(*seats[static_cast<std::size_t>(pointer.seatOver)].first,pointer.seatPressed==pointer.seatOver);
 if(pointer.buttonOver<0)return;
 const auto index=static_cast<std::size_t>(pointer.buttonOver);
 const bool pressed=pointer.buttonPressed==pointer.buttonOver;
 switch(buttons.kind){
 case CardButtons::Kind::Offers:offer(buttons.choices[index],pressed);break;
 case CardButtons::Kind::Place:if(pressed)f.action={MenuAction::Activate,"leave-place"};break;
 case CardButtons::Kind::Question:
  // The chosen answer follows the mouse, but only a moving one takes the choice.
  if(moving)nav.ChoiceIndex(index);
  if(pressed)f.action=nav.Pick(index,f.rows);
  break;
 }
}

// A button a card draws: lit (what the pad's Select takes, or a question's
// chosen answer) on the hover fill in the text's ink; under a pointer that
// has not lit it, the button's fill with a brighter edge; at rest the
// button's fill in muted ink; flat while it cannot be pressed. Lit is told by
// fill and ink alone: the card's ring is the focus's only one. With `plus`, a
// plus before the label (an open seat).
void DrawCardButton(const RoomBoardFrame& f,ImDrawList* d,const v2::Box& b,const MenuChoice& choice,bool lit,bool over,bool plus,float size,std::string& elided) {
 const float s=f.s;const auto& c=v2::Colors();
 const bool live=choice.enabled;
 const ImU32 fill=!live?c.frame:lit?c.hover:c.button,edge=!live?c.border:v2::color::Alpha(c.accent,over&&!lit?.8f:.35f);
 const ImU32 ink=live&&(lit||over)?c.text:c.muted;
 f.Ground(d,b,fill,edge);
 const float mark=plus?18*s:0,room=b.W()-16*s-mark;
 const float w=(std::min)(room,ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,choice.label.c_str()).x);
 const float x=b.min.x+(b.W()-w-mark)*.5f,mid=(b.min.y+b.max.y)*.5f;
 if(plus){
  const float arm=5*s,cx=x+arm;
  d->AddLine(ImVec2(cx-arm,mid),ImVec2(cx+arm,mid),ink,2*s);d->AddLine(ImVec2(cx,mid-arm),ImVec2(cx,mid+arm),ink,2*s);
 }
 DrawCardText(elided,ImVec2(x+mark,mid-size*.5f),w,choice.label,size,ink);
}

// A taken seat in `box`: the fighter's picture, as tall as the box, at its
// outer end (the right one for a versus card's second seat), and in the rest
// the player's tag over their fighter, costume and Ultra, with Ready on its
// inner side while they are ready, and their win streak at this table on the
// tag's inner side where it fits. A versus card centres the lines between the
// picture and the score, so the pair stands even about it; a stacked seat
// starts them beside the picture. The picture is the fighter they picked,
// else their main (SeatLookOf).
void DrawSeat(const RoomBoardFrame& f,ImDrawList* d,const v2::Box& box,const room::Table& t,const room::Member& m,int side,bool versus,bool marked,std::string& elided) {
 const auto& v=f.v;const float s=f.s,face=box.H(),gap=table_card::FaceGap*s;const auto& c=v2::Colors();
 const bool mirrored=versus&&side==1;
 const v2::Box tile=mirrored?v2::Box{ImVec2(box.max.x-face,box.min.y),box.max}:v2::Box{box.min,ImVec2(box.min.x+face,box.max.y)};
 const float left=mirrored?box.min.x:tile.max.x+gap,right=mirrored?tile.min.x-gap:box.max.x,room=right-left;
 const auto widthOf=[](const std::string& value,float size){return ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,value.c_str()).x;};
 // Where a line `width` wide starts: centred on a versus card, else at the start.
 const auto lineAt=[&](float width){return left+(versus?(std::max)(0.f,room-width)*.5f:0.f);};
 const float tagLine=v2::ChipFont()+8*s,lineSize=13*s,lineGap=2*s;
 const auto look=SeatLookOf(v,m);
 // The two lines together, centred on the picture.
 const float top=box.min.y+(std::max)(0.f,(face-tagLine-lineGap-lineSize)*.5f);
 DrawCharacterPortrait(SeatPicture(look,f.art),tile.min,tile.max,c.frame);
 if(const auto points=PointsOf(v,m,look.fighter))v2::LevelBand(d,tile,points->points);
 if(marked)d->AddRect(ImVec2(tile.min.x-2*s,tile.min.y-2*s),ImVec2(tile.max.x+2*s,tile.max.y+2*s),ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)),3*s,0,2*s);
 const auto tag=MemberTag(v,m);
 const std::string streak=StreakText(t,m.id);
 const float tagWidth=(std::min)(room,v2::TagWidth(tag,v2::TagSize::S)),streakWidth=streak.empty()?0:v2::ChipWidth(streak)+8*s;
 const bool streaked=streakWidth>0&&tagWidth+streakWidth<=room;
 const float tagAt=lineAt(tagWidth+(streaked?streakWidth:0));
 v2::Tag(d,ImVec2(mirrored&&streaked?tagAt+streakWidth:tagAt,top),tagWidth,tag,v2::TagSize::S);
 if(streaked)v2::Chip(d,ImVec2(mirrored?tagAt:tagAt+tagWidth+8*s,top),streak,c.accent);
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
}

// A card's line: TABLE n and its state at its start, then its name and what
// differs from the room, each centred on the line whatever its height.
void DrawCardLine(const RoomBoardFrame& f,ImDrawList* d,const room::Table& t,const CardWords& w,const TableCardLayout& at,std::string& elided) {
 namespace c=table_card;
 const float s=f.s,mid=(at.head.y0+at.head.y1)*.5f;const auto& col=v2::Colors();
 float x=at.head.x0;
 x+=v2::SectionHeading(d,ImVec2(x,mid-7*s),w.heading,col.muted)+c::Space*s;
 const ImU32 tone=t.phase==room::TablePhase::Playing?col.accent:t.phase==room::TablePhase::Idle||t.phase==room::TablePhase::Closed?col.muted:
  ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
 v2::Chip(d,ImVec2(x,mid-(v2::ChipFont()+8*s)*.5f),w.phase,tone);
 // What differs takes its width first; the host's name for the table the rest.
 const float room=(std::max)(0.f,at.words.x1-at.words.x0);
 const float differs=w.differs.empty()?0:(std::min)(room,ImGui::GetFont()->CalcTextSizeA(13*s,FLT_MAX,0,w.differs.c_str()).x);
 if(differs>0)DrawCardText(elided,ImVec2(at.words.x1-differs,mid-6.5f*s),differs,w.differs,13*s,col.muted);
 if(!w.name.empty()){
  NoteUserText(w.name);
  DrawCardText(elided,ImVec2(at.words.x0,mid-7.5f*s),room-(differs>0?differs+c::Space*s:0),w.name,15*s,col.text);
 }
}

// What a card shows over its ground: its line, score, seats (a player, an
// offer lit while the pad's Select takes it, or for a seat this member cannot
// take "Open seat" in muted words, no box), buttons and banner.
void DrawCard(const RoomBoardFrame& f,ImDrawList* d,const TableFloor& floor,const TableFloor::Card& card,const CardWords& w,const TableCardLayout& at,
              const std::vector<std::pair<const MenuChoice*,v2::Box>>& seats,const CardButtons& buttons,const CardPointer& pointer) {
 const auto& v=f.v;const auto& t=*card.t;const auto& e=*card.e;const float s=f.s;const auto& c=v2::Colors();
 const auto box=[](const MenuRect& r){return v2::Box{ImVec2(r.x0,r.y0),ImVec2(r.x1,r.y1)};};
 std::string elided;
 DrawCardLine(f,d,t,w,at,elided);
 if(at.score.x1>at.score.x0){
  const float size=22*s,width=HeadingFont()->CalcTextSizeA(size,FLT_MAX,0,w.score.c_str()).x;
  // A versus card's score in the middle between the seats; a stacked card's at its line's end.
  d->AddText(HeadingFont(),size,ImVec2((at.score.x0+at.score.x1-width)*.5f,(at.score.y0+at.score.y1-size)*.5f),t.p1&&t.p2?c.accent:c.muted,w.score.c_str());
 }
 const std::string& lit=f.nav.Lit(e.id);
 for(int side=0;side<2;++side){
  const auto seat=box(at.seats[static_cast<std::size_t>(side)]);
  if(const auto* m=room::FindMember(v.room,side?t.p2:t.p1)){DrawSeat(f,d,seat,t,*m,side,floor.shape.versus,card.banner.seat==side,elided);continue;}
  const auto offered=std::find_if(seats.begin(),seats.end(),[&](const std::pair<const MenuChoice*,v2::Box>& o){return SeatOf(*o.first)==side;});
  if(offered==seats.end()){
   DrawCardText(elided,ImVec2(seat.min.x,(seat.min.y+seat.max.y-15*s)*.5f),seat.W(),loc::T("room.open_seat"),15*s,c.muted,true);
   continue;
  }
  const int index=static_cast<int>(offered-seats.begin());
  DrawCardButton(f,d,seat,*offered->first,offered->first->id==lit,pointer.seatOver==index,true,15*s,elided);
 }
 for(std::size_t i=0;i<buttons.choices.size();++i){
  const auto& r=at.buttons[i];
  if(r.x1<=r.x0)continue;
  const bool chosen=buttons.kind==CardButtons::Kind::Question?i==f.nav.ChoiceIndex():buttons.kind==CardButtons::Kind::Offers&&buttons.choices[i].id==lit;
  DrawCardButton(f,d,box(r),buttons.choices[i],chosen,pointer.buttonOver==static_cast<int>(i),false,13*s,elided);
 }
 if(at.banner.y1>at.banner.y0)DrawCardText(elided,ImVec2(at.banner.x0,(at.banner.y0+at.banner.y1-13*s)*.5f),at.banner.x1-at.banner.x0,card.banner.text,13*s,
  ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)));
 // Flushed once per card, while the card's button is still the hovered item.
 FlushCardTip(elided);
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
}

TableFloor ShapeFloor(const RoomBoardFrame& f,const PlaceExit& place,float width,float height,float after,int gridColumns,bool grid) {
 TableFloor floor;
 std::vector<TableCardContent> contents;
 for(const auto& e:f.rows){
  const auto index=RowKey<std::size_t>(e.id,"table-");
  if(!index||*index>=f.v.room.tables.size())continue;
  const auto& t=f.v.room.tables[*index];
  const bool own=place.table==static_cast<int>(t.id);
  // Quiet: nobody at the table, and nothing on offer but its seats.
  const bool seatsOnly=std::all_of(e.segments.begin(),e.segments.end(),[](const MenuChoice& offer){return SeatOf(offer)>=0;});
  const bool quiet=!t.p1&&!t.p2&&t.queue.empty()&&t.spectators.empty()&&t.watchingNext.empty()&&!own&&seatsOnly;
  TableFloor::Card card{&e,&t,{quiet,false},quiet?TableBanner{}:DescribeTableBanner(f.v,t),own};
  card.content.banner=!card.banner.text.empty();
  contents.push_back(card.content);
  floor.cards.push_back(std::move(card));
 }
 const auto shapeAt=[&](float w){floor.width=w;floor.shape=ShapeTableCards(w,gridColumns,grid,f.s);floor.places=PlaceTableCards(floor.shape,contents);};
 shapeAt(width);
 // Cards that do not fit the scroll stand beside its bar, at the width they are drawn at.
 if(floor.places.height+after>height)shapeAt(width-ImGui::GetStyle().ScrollbarSize);
 return floor;
}

// A Classic room's floor in `tables`, its cards where `floor` stands them, in
// their own scroll when they do not fit; `follows` after them in that scroll.
//  A versus card:  TABLE 1 [phase] name     rules  Queue 1 · Watching 2  [Join queue] [Watch]
//                  [face] name, fighter            2 - 1            name, fighter [face]
//  A stacked card: TABLE 1 [phase] name                              counts        2 - 1
//                  [face] name, fighter
//                  [face] name, fighter
//  A quiet card:   TABLE 3 [Open]   [+ Play as P1]   [+ Play as P2]
void DrawTableCards(const RoomBoardFrame& f,const v2::Box& tables,const TableFloor& floor,const PlaceExit& place,const FloorFollow& follows) {
 namespace c=table_card;
 const float s=f.s;const int columns=floor.shape.columns;
 ImGui::SetCursorScreenPos(tables.min);
 ImGui::BeginChild("Room tables",ImVec2(tables.W(),tables.H()),0,ImGuiWindowFlags_NoNavInputs);
 auto* d=ImGui::GetWindowDrawList();
 const auto start=ImGui::GetCursorScreenPos();
 const float cardWidth=(floor.width-(columns-1)*c::Between*s)/columns;
 // The focus is a row of this scroll's: a card, or the bar when it follows them.
 bool reveal=f.focusChanged&&follows&&BoardAreaOf(f.focus)==BoardArea::Bar;
 for(std::size_t n=0;n<floor.cards.size();++n){
  const auto& card=floor.cards[n];const auto& e=*card.e;
  const ImVec2 at(start.x+static_cast<float>(static_cast<int>(n)%columns)*(cardWidth+c::Between*s),start.y+floor.places.tops[n]);
  const v2::Box b{at,ImVec2(at.x+cardWidth,at.y+floor.places.heights[n])};
  const auto buttons=CardButtonsOf(f,card,place);
  const auto words=CardWordsOf(f,card,buttons,b);
  const auto layout=LayOutTableCard(floor.shape,words.input);
  // The offers that are seats, each in its seat's box.
  std::vector<std::pair<const MenuChoice*,v2::Box>> seats;
  for(const auto& offer:e.segments)
   if(const int side=SeatOf(offer);side>=0){
    const auto& r=layout.seats[static_cast<std::size_t>(side)];
    seats.push_back({&offer,v2::Box{ImVec2(r.x0,r.y0),ImVec2(r.x1,r.y1)}});
   }
  const auto pointer=PointCard(f,e,seats,buttons,layout);
  // The card itself: a press focuses it where its own buttons are its actions.
  const bool hovered=f.Press(e,b,!OnOwnPlace(place,e.id)&&e.segments.empty());
  TakePointer(f,e,pointer,seats,buttons);
  // The question's buttons are its answers: the menu draws no panel of the same choices over the board.
  const bool asking=buttons.kind==CardButtons::Kind::Question&&f.nav.Choosing()&&f.nav.DialogId()==e.id;
  if(asking)f.choices.inPlace={buttons.choices.size(),pointer.buttonOver>=0};
  if(e.id==f.focus)reveal=reveal||f.focusChanged;
  // An open question keeps its card in sight: scrolled away, its buttons would take a press unseen.
  if(!ImGui::IsItemVisible()){reveal=reveal||asking;continue;}
  v2::CardGround(d,b,e.id==f.focus,hovered,card.own);
  DrawCard(f,d,floor,card,words,layout,seats,buttons,pointer);
  if(e.id==f.focus)v2::Focus(d,b.min,b.max);
 }
 float bottom=start.y+floor.places.height;
 if(follows)bottom+=12*s+follows(d,ImVec2(start.x,bottom+12*s),floor.width);
 ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(floor.width,bottom-start.y));
 SettleOnCard(f,floor.places,start,reveal);
 ImGui::EndChild();
}
} }
