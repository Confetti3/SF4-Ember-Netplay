// The room board (Room.dc.html): the tables, the focused row's line and the
// room's action bar on the left; Members over Chat in a column on the right
// (either side of the tables on a 32:9 screen, RoomPanelColumn.cxx); on a
// narrow screen one of the three at a time, under tabs that follow the focus.
// A Classic room's floor is its table cards, drawn here; an Arcade or King of
// the hill room's is its cabinets (RoomPanelArcade.cxx). The rows are built
// in RoomPanel.cxx (RoomBoardRows).
#include "RoomPanelBoard.hxx"
#include "Theme.hxx"
#include "RoomControls.hxx"
#include "RoomCards.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "LatencyMark.hxx"
#include "MenuProbes.hxx"
#include "../common/FighterCatalog.hxx"
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
 v2::Text(d,ImVec2(b.min.x+10*s,b.min.y+9*s),w-20*s,label,14*s,hovered||focused?c.text:c.muted);
 if(focused)v2::Focus(d,b.min,b.max);
 return w;
}

namespace {
bool HasPrefix(const std::string& id,const char* prefix){return id.compare(0,std::strlen(prefix),prefix)==0;}

// The room's action bar: every row the floor and the column do not draw
// (tables, cabinets, matchups, members, the panels' headings and their Hide
// and Show), as buttons on as few lines as their labels allow (v2::BarLines),
// each place at least 140 px. The invitation's copy buttons share one place,
// each with its short name, where the bar is wide enough for both.
class RoomActionBar {
public:
 RoomActionBar(const RoomBoardFrame& f,float width):f_(f){
  for(const auto& e:f.rows){
   if(!OnBar(e.id))continue;
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
 static bool OnBar(const std::string& id){
  for(const char* prefix:{"table-","cabinet-","matchup-","member-","room-hide-","room-show-"})if(HasPrefix(id,prefix))return false;
  return id!="room-members"&&id!="room-chat";
 }
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
  (v.room.delayPolicy==room::DelayPolicy::Own?std::string(loc::T("room.table_footer_own_delay"))+"     ":std::string())+(t.rules.format==room::SetFormat::Unlimited
  ?loc::Tf("room.table_footer",t.rules.roundCount,room::MatchRoundTime(v.room,t),t.queue.size(),watching)
  :loc::Tf("room.table_footer_set",SetLengthText(t.rules.format),RotationText(t.rules.rotation),t.queue.size(),watching));
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

// A Classic room's floor in `tables`: its cards, `columns` across, in their
// own scroll when there are more than fit; with `follows`, the action bar
// after them in that scroll.
void DrawTableCards(const RoomBoardFrame& f,const v2::Box& tables,int columns,float cardHeight,const PlaceExit& place,const RoomActionBar* follows) {
 const auto& v=f.v;const auto& nav=f.nav;const float s=f.s;const auto& c=v2::Colors();
 const bool onOwnPlace=OnOwnPlace(place,f.focus);
 const auto pointer=ImGui::GetIO().MouseDelta;const bool pointerMoved=pointer.x!=0||pointer.y!=0;
 const float cardWidth=(tables.W()-(columns-1)*8*s)/columns;
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
  const ImVec2 at(start.x+(n%columns)*(cardWidth+8*s),start.y+(n/columns)*(cardHeight+8*s));++n;
  const v2::Box b{at,ImVec2(at.x+cardWidth,at.y+cardHeight)};
  const bool focused=e.id==f.focus,choosing=nav.Choosing()&&nav.DialogId()==e.id&&!e.choices.empty();
  const auto options=StripOptions(f,e,t,choosing,!nav.Asking()&&onOwnPlace&&focused,place);
  // The strip's buttons go in first: the first item under the pointer is the one it is over.
  const v2::Box strip{ImVec2(b.min.x+14*s,b.max.y-44*s),ImVec2(b.max.x-14*s,b.max.y-12*s)};
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
  // TABLE n, its state, and the set's score (or vs) at the right.
  float x=b.min.x+14*s,y=b.min.y+12*s;const float w=b.W()-28*s;
  x+=v2::SectionHeading(td,ImVec2(x,y+4*s),loc::Tf("room.v2.table",t.id+1),c.muted)+8*s;
  const ImU32 tone=t.phase==room::TablePhase::Playing?c.accent:t.phase==room::TablePhase::Idle||t.phase==room::TablePhase::Closed?c.muted:
   ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
  v2::Chip(td,ImVec2(x,y),PhaseName(t),tone);
  const std::string middle=t.phase==room::TablePhase::Closed?std::string():t.p1&&t.p2&&!SetScoreText(t.score).empty()?SetScoreText(t.score):"vs";
  td->AddText(HeadingFont(),22*s,ImVec2(b.max.x-14*s-HeadingFont()->CalcTextSizeA(22*s,FLT_MAX,0,middle.c_str()).x,y),c.accent,middle.c_str());
  // The two seats as player tags: fighter and Ready on each one's chip.
  y+=32*s;
  const room::MemberId seats[]={t.p1,t.p2};
  for(int side=0;side<2;++side,y+=30*s){
   const auto* m=room::FindMember(v.room,seats[side]);
   if(!m){if(side==0||t.p1){td->AddText(ImGui::GetFont(),15*s,ImVec2(b.min.x+14*s,y+4*s),c.muted,loc::T("room.open_seat"));}continue;}
   v2::TagView tag;tag.name=m->name;tag.marks={m->country,m->badge};tag.ping=LatencyMarkFor(v.memberLatency,*m,v.room.localMember);
   const auto* fighter=selection::FindFighter(m->id==v.room.localMember?v.selectedFighter:m->fighter);
   tag.status=fighter?fighter->name:loc::T("room.fighter_not_shared");
   if(t.ready[side]&&(t.phase==room::TablePhase::Waiting||t.phase==room::TablePhase::Ready))tag.status+=std::string(" \xC2\xB7 ")+loc::T("room.status.ready");
   v2::Tag(td,ImVec2(b.min.x+14*s,y),w,tag,v2::TagSize::S);
  }
  const auto banner=DescribeTableBanner(v,t);
  if(!banner.text.empty())text(ImVec2(b.min.x+14*s,y),w,banner.text,13*s,ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)));
  if(options.empty())text(ImVec2(b.min.x+14*s,b.max.y-12*s-14*s),w,TableFooter(v,t),13*s,c.muted);
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
 float bottom=start.y+((n+columns-1)/columns)*(cardHeight+8*s)-8*s;
 if(follows){const float width=ImGui::GetContentRegionAvail().x;follows->Draw(td,ImVec2(start.x,bottom+12*s),width,tables);bottom+=12*s+follows->Height(width);}
 ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(tables.W(),bottom-start.y));
 ImGui::EndChild();
}
}

void ApplicationShell::DrawRoomBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,
                                     const MenuVisualFeedback& feedback) {
 boardDrawn_=true;
 const float s=Scale();
 const RoomBoardFrame f{v,rows,nav,action,feedback,nav.Focus(),nav.Focus()!=roomBoardFocus_,menu_.bodyChoices,s};
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
 const float logical=ImGui::GetContentRegionAvail().x/s;const bool narrow=logical<860,ultra=logical>=1960;
 v2::Box area{origin,ImVec2(origin.x+ImGui::GetContentRegionAvail().x,origin.y+height)};
 ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
 const bool showMembers=preferences_.roomShowMembers,showChat=preferences_.roomShowChat;
 const int part=HasPrefix(focus,"member-")||focus=="room-members"||focus=="room-hide-members"||focus=="room-show-members"?1:
  focus=="room-chat"||focus=="room-hide-chat"||focus=="room-show-chat"?2:0;
 if(narrow){
  ImGui::SetCursorScreenPos(area.min);
  const std::vector<std::string> labels{loc::T("room.v2.board"),std::string(loc::T("room.members"))+" "+std::to_string(v.room.members.size()),
   std::string(loc::T("room.chat"))+(unread?" "+std::to_string(unread):std::string())};
  // The tab the pointer pressed, or LB and RB stepping from the part on show,
  // wrapping as every tab strip does (StepTab).
  const int step=TabDelta(action);if(step)action={};
  const int clicked=v2::Tabs(labels,part);
  const int picked=clicked>=0?clicked:step?StepTab(part,step,static_cast<int>(labels.size())):-1;
  // Each tab's first row: the floor, the first member (the heading once folded), the message box (Show once folded).
  const char* const parts[]={v.room.arcade?"cabinet-":"table-",showMembers?"member-":"room-members",showChat?"room-chat":"room-show-chat"};
  if(picked>=0&&picked!=part&&!nav.Choosing()){
   const auto to=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return HasPrefix(e.id,parts[picked]);});
   if(to!=rows.end())nav.Focus(to->id,rows);
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
  if(watched!=v.room.tables.end())v2::Chip(d,ImVec2(at.x+8*s,at.y+8*s),std::string(loc::T("room.status.watching"))+" \xC2\xB7 "+loc::Tf("room.v2.table",watched->id+1),c.accent);
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
  const float cardHeight=(banners?182:160)*s;
  // The bar stays along the bottom while a row of cards fits over it and the
  // focused row's line; on a shorter board it follows the cards in their scroll.
  const bool follows=!v.room.arcade&&tables.H()-bar.Height(tables.W())-38*s<cardHeight;
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
  if(v.room.arcade){ImGui::SetCursorScreenPos(tables.min);DrawArcadeBoard(v,rows,nav,action,ImVec2(tables.W(),tables.H()),v.room.arcade);}
  else DrawTableCards(f,tables,ultra||logical>=1640?3:narrow&&tables.W()<640*s?1:2,cardHeight,place,follows?&bar:nullptr);
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
 if(!narrow&&!ultra)RoomGrips(area);
 columnPicture_=false;
 ImGui::PopStyleColor();
 ImGui::PopStyleColor();ImGui::PopStyleVar();roomBoardFocus_=nav.Focus();
}

} }
