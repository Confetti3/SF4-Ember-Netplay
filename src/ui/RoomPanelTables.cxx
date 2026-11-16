// The room board (Room.dc.html): the tables, the focused row's line and the
// room's action bar on the left; Members over Chat in a column on the right
// (either side of the tables on a 32:9 screen, RoomPanelColumn.cxx); on a
// narrow screen one of the three at a time, under tabs that follow the focus.
// A Classic room's floor is its table cards (RoomTableCards.cxx); an Arcade
// or King of the hill room's is its cabinets (RoomPanelArcade.cxx). Here: the
// board's layout, the tables' heading, the action bar and the focused row's
// line. The rows are built in RoomPanel.cxx (RoomBoardRows).
#include "RoomPanelBoard.hxx"
#include "Theme.hxx"
#include "RoomCards.hxx"
#include "RowId.hxx"
#include "MenuProbes.hxx"
#include "SelectionLabels.hxx"
#include "../common/GameCatalog.hxx"
#include "../common/Localization.hxx"
#include "../common/ProductIdentity.hxx"
#include <algorithm>
#include <cfloat>

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
 v2::Ground(d,b.min,b.max,fill,v2::Corner());v2::Edge(d,b.min,b.max,edge,v2::Corner(),s);
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
 SeatLook look;look.picked=picked>=0;
 // Unpicked, the main stands for them, or nobody (Branding::roomFacesArePicks).
 look.fighter=look.picked?picked:ember::Product().branding.roomFacesArePicks?-1:own?v.preferences.mainFighter:m.mainFighter;
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
 // option's own label, which the card shows; on a card that offers places,
 // the same of the place it lights.
 const auto& nav=f.nav;
 const MenuChoice* option=nav.Choosing()&&nav.DialogId()==e->id&&nav.ChoiceIndex()<e->choices.size()?&e->choices[nav.ChoiceIndex()]:nullptr;
 if(!option){
  const auto lit=std::find_if(e->segments.begin(),e->segments.end(),[&](const MenuChoice& o){return o.id==nav.Lit(e->id);});
  if(lit!=e->segments.end())option=&*lit;
 }
 const bool card=RowKey<std::size_t>(e->id,"table-").has_value();
 const std::string choice=option&&option->detail!=option->label?option->detail:std::string();
 const auto newline=e->detail.find('\n');
 std::string line=!choice.empty()||card?choice:e->detail.substr(0,newline);
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

// The tables' heading: TABLES, then at its right the rules every table plays
// by (the room's own and the first table's) and how many play and watch,
// saying nothing of a count that is none. The counts give way first; rules
// that do not fit beside the heading alone take a paragraph of their own
// under it, at the board's small size, wrapping rather than cut. Returns the
// height the heading takes with the space under it: a paragraph takes its
// room from that space, so a table fits under it as under one line.
float DrawTablesHeading(ImDrawList* d,const ShellView& v,const v2::Box& tables,float s) {
 const auto& c=v2::Colors();
 std::size_t playing=0,watching=0;
 for(const auto& t:v.room.tables){playing+=t.phase==room::TablePhase::Playing;watching+=t.spectators.size()+t.watchingNext.size();}
 std::string counts=playing?loc::Tf("room.v2.tables_playing",playing):std::string();
 if(watching)counts+=(counts.empty()?"":"  \xC2\xB7  ")+loc::Tf("room.table_watching",watching);
 const std::string rules=room::RulesOf(v.room).cabinets?std::string():RoomRulesText(v,v.room.tables[0]);
 const float headingWidth=v2::SectionHeading(d,tables.min,loc::T("room.v2.tables"),c.muted);
 const auto widthOf=[&](const std::string& text){return text.empty()?0.f:ImGui::GetFont()->CalcTextSizeA(15*s,FLT_MAX,0,text.c_str()).x;};
 const float room=(std::max)(0.f,tables.W()-headingWidth-24*s);
 const std::string both=rules.empty()?counts:counts.empty()?rules:rules+"  \xC2\xB7  "+counts;
 const bool ownLine=!rules.empty()&&widthOf(rules)>room;
 const std::string beside=widthOf(both)<=room?both:ownLine?(widthOf(counts)<=room?counts:std::string()):rules;
 const float besideWidth=(std::min)(room,widthOf(beside));
 if(besideWidth>0)v2::Text(d,ImVec2(tables.max.x-besideWidth,tables.min.y),besideWidth,beside,15*s,c.muted);
 d->AddLine(ImVec2(tables.min.x+headingWidth+12*s,tables.min.y+9*s),ImVec2(tables.max.x-(besideWidth>0?besideWidth+12*s:0),tables.min.y+9*s),c.border,s);
 if(!ownLine)return 32*s;
 const ImVec2 at(tables.min.x,tables.min.y+24*s);
 const float height=v2::Paragraph(d,at,tables.W(),rules,13*s,c.muted);
 ReportMenuCard("tables-rules",at,ImVec2(at.x+tables.W(),at.y+height));
 return 30*s+height;
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
 if(OnOwnPlace(place,focus)&&((action.kind==MenuAction::Back&&!action.keyboard)||leaveKey))action={MenuAction::Activate,OwnPlace::Leave};
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
  // The tables, under their heading.
  v2::CutTop(tables,DrawTablesHeading(d,v,tables,s),0);
  // The room's actions along the bottom, then the focused row's line over them.
  const RoomActionBar bar(f);
  const int gridColumns=ultra||logical>=1640?3:narrow&&tables.W()<640*s?1:2;
  // The floor at the board's whole width: its tallest row at its least is the least it needs.
  const auto whole=ShapeFloor(f,place,tables.W(),FLT_MAX,0,gridColumns,preferences_.roomTableGrid);
  // A row of cards at their least: the tallest one, else a seated card's least.
  const float cardHeight=whole.cards.empty()?whole.shape.Least({}):whole.places.leastRow;
  // The bar stays along the bottom while the floor fits over it and the
  // focused row's line: a row of cards, or an Arcade room's whole floor. On a
  // shorter board it follows the floor in its scroll.
  const float floorHeight=room::RulesOf(v.room).cabinets?ArcadeBoardHeight(v,rows,tables.W()):cardHeight;
  const bool follows=tables.H()-bar.Height(tables.W())-38*s<floorHeight;
  // A board that starts or stops scrolling, changes size, or whose cards grow
  // or shrink (a banner, a pair sitting down) can leave the focused row out
  // of sight with the focus unchanged: it scrolls back into view as it does
  // when the focus moves. A scroll of the player's own stays put between.
  const RoomTablesShape shape{follows,tables.W(),tables.H(),whole.places.leastHeight};
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
    const float width=ImGui::GetContentRegionAvail().x,boardHeight=ArcadeBoardHeight(v,rows,width);
    DrawArcadeBoard(v,rows,nav,action,ImVec2(width,boardHeight),v.room.type,true,f.focusChanged);
    bar.Draw(ImGui::GetWindowDrawList(),ImVec2(start.x,start.y+boardHeight+12*s),width,tables);
    ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(width,boardHeight+12*s+bar.Height(width)));
    ImGui::EndChild();
   }
  }
  else{
   // Shaped again at the height the cards have now, beside the scroll's bar where they need it.
   const float after=follows?12*s+bar.Height(tables.W()):0;
   const auto floor=ShapeFloor(f,place,tables.W(),tables.H(),after,gridColumns,preferences_.roomTableGrid);
   DrawTableCards(f,tables,floor,place,follows?FloorFollow([&](ImDrawList* drawn,ImVec2 at,float width){bar.Draw(drawn,at,width,tables);return bar.Height(width);}):FloorFollow());
  }
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
