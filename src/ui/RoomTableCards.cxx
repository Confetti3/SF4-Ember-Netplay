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

// A card's buttons, in the strip at its foot (on its line where the strips
// stand aside): its offers that are not seats (Join queue, Watch); on your own
// place Ready (a seat), what leaves it and Table options; while the card asks
// a question (leaving a seat, ending the watched game), the question's answers.
struct CardButtons {
 enum class Kind { Offers, Place, Question } kind=Kind::Offers;
 std::vector<MenuChoice> choices;
};
CardButtons CardButtonsOf(const RoomBoardFrame& f,const TableFloor::Card& card,const PlaceExit& place) {
 const auto& e=*card.e;
 if(f.nav.Choosing()&&f.nav.DialogId()==e.id&&!e.choices.empty())return {CardButtons::Kind::Question,e.choices};
 if(card.own)return {CardButtons::Kind::Place,OwnPlaceChoices(f.v,*card.t,place)};
 CardButtons offers;
 for(const auto& offer:e.segments)if(SeatOf(offer)<0)offers.choices.push_back(offer);
 return offers;
}

// What a card shows on its line, measured for its layout: TABLE n and its
// state, its name, its rules where they are not the first table's and its
// counts, and its score at the size its step gives it.
struct CardWords {
 std::string heading,phase,name,differs,score;
 TableCardInput input;
};
CardWords CardWordsOf(const RoomBoardFrame& f,const TableCardShape& shape,const TableFloor::Card& card,const CardButtons& buttons,const v2::Box& b) {
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
 in.score=width(w.score,TypeOf(shape.StepOf(card.content,b.H())).score*s,HeadingFont());
 // A hair over the button's own padding (v2::CardButton keeps Space each side), so a label that fits exactly is not cut by rounding.
 // A strip's labels are drawn at the seat buttons' size (DrawCard), a line's at 13 px.
 const float labelSize=(card.content.strip?TypeOf(shape.StepOf(card.content,b.H())).offer:13)*s;
 for(const auto& choice:buttons.choices)in.buttons.push_back(std::ceil(width(choice.label,labelSize,ImGui::GetFont()))+2*c::Space*s+2*s);
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
// your own place its buttons (Ready, Leave, Table options) are sent even where they cannot
// be, for RoomAction to say why; a question's answers answer it.
void TakePointer(const RoomBoardFrame& f,const MenuEntry& e,const CardPointer& pointer,const std::vector<std::pair<const MenuChoice*,v2::Box>>& seats,
                 const CardButtons& buttons) {
 auto& nav=f.nav;
 const auto moved=ImGui::GetIO().MouseDelta;const bool moving=moved.x!=0||moved.y!=0;
 const auto offer=[&](const MenuChoice& choice,bool pressed,bool sentDisabled=false){
  if(moving||pressed)nav.Light(e.id,choice.id,f.rows);
  if(pressed&&(choice.enabled||sentDisabled))f.action={MenuAction::Chosen,e.id,choice.id};
 };
 if(pointer.seatOver>=0)offer(*seats[static_cast<std::size_t>(pointer.seatOver)].first,pointer.seatPressed==pointer.seatOver);
 if(pointer.buttonOver<0)return;
 const auto index=static_cast<std::size_t>(pointer.buttonOver);
 const bool pressed=pointer.buttonPressed==pointer.buttonOver;
 switch(buttons.kind){
 case CardButtons::Kind::Offers:offer(buttons.choices[index],pressed);break;
 case CardButtons::Kind::Place:offer(buttons.choices[index],pressed,true);break;
 case CardButtons::Kind::Question:
  // The chosen answer follows the mouse, but only a moving one takes the choice.
  if(moving)nav.PointChoice(index);
  if(pressed)f.action=nav.Pick(index,f.rows);
  break;
 }
}

// What a seat says under its player's fighter, in order, as many as its
// step shows: their win streak at this table (a chip), then how long they
// have been idle, else their sets won and lost in the room.
struct SeatNote { std::string text; bool chip; };
std::vector<SeatNote> SeatNotes(const ShellView& v,const room::Table& t,const room::Member& m) {
 std::vector<SeatNote> notes;
 if(auto streak=StreakText(t,m.id);!streak.empty())notes.push_back({std::move(streak),true});
 std::string idle=IdleText(m,v.roomElapsedMs);
 if(idle.empty()&&(m.setsWon||m.setsLost))idle=loc::Tf("profile.record_value",static_cast<int>(m.setsWon),static_cast<int>(m.setsLost));
 if(!idle.empty())notes.push_back({std::move(idle),false});
 return notes;
}

// What a Compact seat says beside the name, in the streak's slot: the win
// streak (a chip), else how long they have been idle; nothing for neither.
SeatNote BesideName(const ShellView& v,const room::Table& t,const room::Member& m) {
 if(auto streak=StreakText(t,m.id);!streak.empty())return {std::move(streak),true};
 return {IdleText(m,v.roomElapsedMs),false};
}

// A taken seat in `box`: the fighter's picture in `tile` (the layout's, at
// the seat's outer end: the right one for a versus card's second seat), and
// in the rest the player's tag (its name in the heading size on a Roomy card)
// over their fighter, costume and Ultra, with Ready on its inner side while
// they are ready; on a Compact card their win streak at this table, else
// their idle time, on the tag's inner side where it fits (PlaceSeatTag), on a
// taller one the lines its step shows
// under the fighter (SeatNotes). The lines stand beside the picture, so a
// player reads as one with their fighter: from its end on the first seat and
// a stacked one, up to its start on a versus card's second seat. The picture is the fighter they picked,
// else their main (SeatLookOf), on a cabinet's screen as an Arcade room shows
// its players (DrawSeatScreen), the screen filling the tile.
void DrawSeat(const RoomBoardFrame& f,ImDrawList* d,const v2::Box& box,const v2::Box& tile,const room::Table& t,const room::Member& m,int side,bool versus,
              const TableCardType& type,bool marked,std::string& elided) {
 const auto& v=f.v;const float s=f.s,gap=table_card::FaceGap*s;const auto& c=v2::Colors();
 const bool mirrored=versus&&side==1;
 const float left=mirrored?box.min.x:tile.max.x+gap,right=mirrored?tile.min.x-gap:box.max.x,room=(std::max)(0.f,right-left);
 const auto widthOf=[](const std::string& value,float size){return ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,value.c_str()).x;};
 // Where a line `width` wide starts: beside the picture, so at the lines' start, or for a versus card's second seat ending at their end.
 const auto lineAt=[&](float width){return mirrored?left+(std::max)(0.f,room-width):left;};
 const v2::TagSize tagSize=type.largeName?v2::TagSize::M:v2::TagSize::S;
 // As v2::Tag lays its one line out: the name's size or a chip's, whichever is taller.
 const float chipLine=v2::ChipFont()+8*s,tagLine=(std::max)(type.largeName?22*s:ImGui::GetFontSize(),chipLine);
 const float lineSize=type.fighter*s,lineGap=(type.largeName?6:type.notes?4:2)*s,noteSize=13*s,noteGap=4*s;
 const auto look=SeatLookOf(v,m);
 // The notes that fit beside the picture, and all the lines together centred on it.
 const auto notes=SeatNotes(v,t,m);
 std::size_t shown=(std::min)(notes.size(),static_cast<std::size_t>(type.notes));
 const auto block=[&](std::size_t count){
  float height=tagLine+lineGap+lineSize;
  for(std::size_t i=0;i<count;++i)height+=noteGap+(notes[i].chip?chipLine:noteSize);
  return height;
 };
 while(shown>0&&block(shown)>box.H())--shown;
 const float top=box.min.y+(std::max)(0.f,(box.H()-block(shown))*.5f);
 // One tone for every table: the accent while its game is played, quiet otherwise. The Ultra's mark only on a
 // screen with the room for it (a Medium card's and up); the fighter's line names it on every card.
 const bool marksUltra=look.picked&&tile.H()>=table_card::MediumSeat*s-.5f;
 const v2::Box glass=DrawSeatScreen(d,tile,SeatPicture(look,f.art),t.phase==room::TablePhase::Playing?c.accent:c.muted,marksUltra?look.ultra:-1,s,SeatFacing(versus,side));
 if(const auto points=PointsOf(v,m,look.fighter))v2::LevelBand(d,glass,points->points);
 if(marked)d->AddRect(ImVec2(tile.min.x-2*s,tile.min.y-2*s),ImVec2(tile.max.x+2*s,tile.max.y+2*s),ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)),3*s,0,2*s);
 const auto tag=MemberTag(v,m);
 // A Compact card's streak, else their idle time, stands beside the name where it fits; a taller card's are notes.
 const SeatNote beside=type.notes?SeatNote{}:BesideName(v,t,m);
 const float tagWidth=(std::min)(room,v2::TagWidth(tag,tagSize)),
  besideWidth=beside.text.empty()?0:beside.chip?v2::ChipWidth(beside.text):widthOf(beside.text,noteSize);
 const auto at=PlaceSeatTag(left,room,tagWidth,besideWidth,versus,mirrored,s);
 v2::Tag(d,ImVec2(at.tag,top),tagWidth,tag,tagSize);
 if(at.noted){
  if(beside.chip)v2::Chip(d,ImVec2(at.note,top+(tagLine-chipLine)*.5f),beside.text,c.accent);
  else DrawCardText(elided,ImVec2(at.note,top+(tagLine-noteSize)*.5f),besideWidth,beside.text,noteSize,c.muted);
 }
 // The fighter, costume and Ultra, and Ready on their inner side (towards
 // the score) while the seat is ready. The Ultra's short name where the long
 // one does not fit.
 const bool ready=t.ready[side]&&(t.phase==room::TablePhase::Waiting||t.phase==room::TablePhase::Ready);
 const std::string readyWord=ready?loc::T("room.status.ready"):"";
 const float chip=ready?v2::ChipWidth(readyWord)+8*s:0,y=top+tagLine+lineGap;
 std::string name=SeatCaption(look);
 if(widthOf(name,lineSize)>room-chip)name=SeatCaption(look,true);
 const float nameWidth=(std::max)(0.f,(std::min)(room-chip,widthOf(name,lineSize))),x=lineAt(nameWidth+chip);
 DrawCardText(elided,ImVec2(mirrored?x+chip:x,y),room-chip,name,lineSize,c.muted);
 if(ready)v2::Chip(d,ImVec2(mirrored?x:x+nameWidth+8*s,y+(lineSize-chipLine)*.5f),readyWord,palette::Ready);
 float under=y+lineSize;
 for(std::size_t i=0;i<shown;++i){
  const auto& note=notes[i];
  under+=noteGap;
  if(note.chip){v2::Chip(d,ImVec2(lineAt((std::min)(room,v2::ChipWidth(note.text))),under),note.text,c.accent);under+=chipLine;continue;}
  DrawCardText(elided,ImVec2(lineAt((std::min)(room,widthOf(note.text,noteSize))),under),room,note.text,noteSize,c.muted);
  under+=noteSize;
 }
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

// What a card shows over its ground at its step: its line, score, seats (a
// player, an offer lit while the pad's Select takes it, as tall as its seat's
// place, or for a seat this member cannot take "Open seat" in muted words, no
// box), buttons and banner.
void DrawCard(const RoomBoardFrame& f,ImDrawList* d,const TableFloor& floor,const TableFloor::Card& card,const CardWords& w,const TableCardLayout& at,
              const std::vector<std::pair<const MenuChoice*,v2::Box>>& seats,const CardButtons& buttons,const CardPointer& pointer) {
 const auto& v=f.v;const auto& t=*card.t;const auto& e=*card.e;const float s=f.s;const auto& c=v2::Colors();
 const auto box=[](const MenuRect& r){return v2::Box{ImVec2(r.x0,r.y0),ImVec2(r.x1,r.y1)};};
 const TableCardType type=TypeOf(at.step);
 std::string elided;
 DrawCardLine(f,d,t,w,at,elided);
 if(at.score.x1>at.score.x0){
  const float size=type.score*s,width=HeadingFont()->CalcTextSizeA(size,FLT_MAX,0,w.score.c_str()).x;
  // A versus card's score in the middle between the seats; a stacked card's at its line's end. Live in the accent while the game is played.
  const ImU32 tone=t.phase==room::TablePhase::Playing?c.accent:t.p1&&t.p2?c.text:c.muted;
  d->AddText(HeadingFont(),size,ImVec2((at.score.x0+at.score.x1-width)*.5f,(at.score.y0+at.score.y1-size)*.5f),tone,w.score.c_str());
 }
 const std::string& lit=f.nav.Lit(e.id);
 const float offerSize=type.offer*s;
 for(int side=0;side<2;++side){
  const auto seat=box(at.seats[static_cast<std::size_t>(side)]);
  if(seat.max.x<=seat.min.x)continue;
  if(const auto* m=room::FindMember(v.room,side?t.p2:t.p1)){
   DrawSeat(f,d,seat,box(at.faces[static_cast<std::size_t>(side)]),t,*m,side,floor.shape.versus,type,card.banner.seat==side,elided);
   continue;
  }
  const auto offered=std::find_if(seats.begin(),seats.end(),[&](const std::pair<const MenuChoice*,v2::Box>& o){return SeatOf(*o.first)==side;});
  if(offered==seats.end()){
   DrawCardText(elided,ImVec2(seat.min.x,(seat.min.y+seat.max.y-offerSize)*.5f),seat.W(),loc::T("room.open_seat"),offerSize,c.muted,true);
   continue;
  }
  const int index=static_cast<int>(offered-seats.begin());
  bool cut=false;
  v2::CardButton(d,seat,offered->first->label,offerSize,offered->first->id==lit,pointer.seatOver==index,offered->first->enabled,true,&cut);
  if(cut)elided+=(elided.empty()?"":"\n")+offered->first->label;
 }
 for(std::size_t i=0;i<buttons.choices.size();++i){
  const auto& r=at.buttons[i];
  if(r.x1<=r.x0)continue;
  const bool chosen=buttons.kind==CardButtons::Kind::Question?i==f.nav.ChoiceIndex():buttons.choices[i].id==lit;
  // Pressed on the line's whole height, drawn inside it; a quiet card's answers fill the seats' place, worded as its
  // seats; a strip's buttons are drawn as pressed, worded as the seat buttons are.
  const bool large=card.content.quiet||card.content.strip;
  const float inset=large?0:table_card::HeadInset*s;
  bool cut=false;
  // Ready on your own place is the bar's Ready: the same filled button.
  const bool primary=buttons.kind==CardButtons::Kind::Place&&buttons.choices[i].id==OwnPlace::Ready;
  v2::CardButton(d,v2::Box{ImVec2(r.x0,r.y0+inset),ImVec2(r.x1,r.y1-inset)},buttons.choices[i].label,large?offerSize:13*s,chosen,
   pointer.buttonOver==static_cast<int>(i),buttons.choices[i].enabled,false,&cut,primary);
  if(cut)elided+=(elided.empty()?"":"\n")+buttons.choices[i].label;
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

// The cards fill `height` less `after` (PlaceTableCards), beside the scroll's
// bar where even at their least they do not fit it.
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
  // A card's buttons stand in a strip at its foot, under the seats: your own
  // place's, whatever they are asked or offer, and another table's offers that
  // are not seats (Join queue, Watch). PlaceTableCards may stand the strips aside.
  card.content.strip=own||!seatsOnly;
  contents.push_back(card.content);
  floor.cards.push_back(std::move(card));
 }
 const float area=height-after;
 const auto shapeAt=[&](float w){
  floor.width=w;floor.shape=ShapeTableCards(w,gridColumns,grid,f.s);floor.places=PlaceTableCards(floor.shape,contents,area);
  // Where the strips stand aside (the column is too short for them) each card's buttons are on its line.
  for(std::size_t i=0;i<floor.cards.size();++i)floor.cards[i].content.strip=contents[i].strip&&!floor.places.stripAside;
 };
 shapeAt(width);
 // Cards that do not fit the scroll stand beside its bar, at the width they are drawn at.
 if(floor.places.scrolls)shapeAt(width-ImGui::GetStyle().ScrollbarSize);
 return floor;
}

// A Classic room's floor in `tables`, its cards where `floor` stands them, in
// their own scroll when they do not fit; `follows` after them in that scroll.
//  A versus card:  TABLE 1 [phase] name                        rules  Queue 1 · Watching 2
//                  [screen] name, fighter (notes)   2 - 1   name, fighter (notes) [screen]
//                  [        Join queue        ]  [          Watch          ]
//  A stacked card: TABLE 1 [phase] name                              counts        2 - 1
//                  [screen] name, fighter
//                  [screen] name, fighter
//                  [        Join queue        ]  [          Watch          ]
//  Your own card:  as a versus card, and under the seats
//                  [   Ready up   ]  [  Leave seat  ]  [ Table options ]
//  Too short a column for the strips: every card's buttons on its line instead
//                  TABLE 1 [phase] name      rules  counts  [Join queue] [Watch]
//  A quiet card:   TABLE 3 [Open]
//                  [      + Play as P1      ]  [      + Play as P2      ]
//  Its line:       TABLE 3 [Open]   [+ Play as P1]   [+ Play as P2]
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
  const auto words=CardWordsOf(f,floor.shape,card,buttons,b);
  const auto layout=LayOutTableCard(floor.shape,words.input);
  // The offers that are seats, each in its seat's box.
  std::vector<std::pair<const MenuChoice*,v2::Box>> seats;
  for(const auto& offer:e.segments)
   if(const int side=SeatOf(offer);side>=0&&layout.seats[static_cast<std::size_t>(side)].x1>layout.seats[static_cast<std::size_t>(side)].x0){
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
