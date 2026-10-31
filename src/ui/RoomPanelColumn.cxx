// The room board's column (Room.dc.html): Members over Chat beside the
// tables, the player's layout of it (PlayerPreferences::roomColumn, roomSplit,
// roomShowMembers, roomShowChat), the grips that drag it and Settings' picture
// of it. The board itself is drawn by DrawRoomBoard (RoomPanelTables.cxx).
#include "RoomPanelBoard.hxx"
#include "Theme.hxx"
#include "MenuProbes.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace sf4e { namespace ui {

void ApplicationShell::ArrangeRoom(v2::Box area,v2::Box& tables,v2::Box& members,v2::Box& chat,v2::Box* picture) const {
 // A folded panel is its heading's strip and the other takes the column; both
 // folded, the two strips in a narrow column and the tables the rest.
 const float s=Scale(),strip=FoldedPanelHeight*s;
 const bool showMembers=preferences_.roomShowMembers,showChat=preferences_.roomShowChat;
 tables=area;
 chat=v2::CutRight(tables,showMembers||showChat?area.W()*preferences_.roomColumn/100:FoldedColumnWidth*s,24*s);
 if(columnPicture_){
  const auto* vp=ImGui::GetMainViewport();
  const v2::Box top=v2::CutTop(chat,(chat.W()-24*s)*vp->Size.y/vp->Size.x+24*s,16*s);if(picture)*picture=top;
 }
 if(showMembers&&showChat)members=v2::CutTop(chat,(chat.H()-16*s)*preferences_.roomSplit/100,16*s);
 else if(showMembers){members=chat;chat=v2::CutBottom(members,strip,16*s);}
 else{members=v2::CutTop(chat,strip,16*s);if(!showChat)chat.max.y=chat.min.y+strip;}
}
void ApplicationShell::RoomGrips(v2::Box area) {
 v2::Box tables,members,chat;ArrangeRoom(area,tables,members,chat);
 const float column=(tables.max.x+members.min.x)*.5f,split=(members.max.y+chat.min.y)*.5f;
 const bool hasColumn=preferences_.roomShowMembers||preferences_.roomShowChat,hasSplit=preferences_.roomShowMembers&&preferences_.roomShowChat;
 const float x=hasColumn?v2::Grip("##room-column",ImVec2(column,area.min.y),ImVec2(column,area.max.y)):NAN;
 const float y=hasSplit?v2::Grip("##room-split",ImVec2(chat.min.x,split),ImVec2(chat.max.x,split)):NAN;
 // The pointer's share, snapped to the grip's steps from its default.
 const auto snap=[](float percent,int low,int high,int base,int step){return (std::max)(low,(std::min)(high,base+static_cast<int>(std::lround((percent-base)/step))*step));};
 auto next=preferences_;
 if(!std::isnan(x))next.roomColumn=snap((area.max.x-x)/area.W()*100,netplay::MinRoomColumn,netplay::MaxRoomColumn,netplay::DefaultRoomColumn,netplay::RoomColumnStep);
 if(!std::isnan(y))next.roomSplit=snap((y-members.min.y)/(chat.max.y-members.min.y)*100,netplay::MinRoomSplit,netplay::MaxRoomSplit,netplay::DefaultRoomSplit,netplay::RoomSplitStep);
 if(next.roomColumn!=preferences_.roomColumn||next.roomSplit!=preferences_.roomSplit){preferences_=next;preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;}
}
void ApplicationShell::DrawRoomLayoutPreview() {
 // Customise.dc.html: the tables as four cards, members over chat with their shares, and the grips.
 const float s=Scale(),w=ImGui::GetContentRegionAvail().x,h=(std::max)(120*s,(std::min)(w*.6f,ImGui::GetContentRegionAvail().y-8*s));
 const auto p=ImGui::GetCursorScreenPos();const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
 const v2::Box area{p,ImVec2(p.x+w,p.y+h)};
 d->AddRectFilled(area.min,area.max,c.ink,4*s);d->AddRect(area.min,area.max,c.border,4*s,0,s);
 v2::Box tables,members,chat;const v2::Box inside=v2::Inset(area,10*s,10*s);ArrangeRoom(inside,tables,members,chat);
 const auto heading=[&](const v2::Box& b,const std::string& label){v2::SectionHeading(d,b.min,label);};
 heading(tables,loc::T("room.v2.tables"));
 const float cw=(tables.W()-6*s)/2,ch=(std::min)(64*s,(tables.H()-26*s)/2);
 for(int i=0;i<4;++i){const ImVec2 at(tables.min.x+(i%2)*(cw+6*s),tables.min.y+20*s+(i/2)*(ch+6*s));d->AddRectFilled(at,ImVec2(at.x+cw,at.y+ch),c.frame,4*s);d->AddRect(at,ImVec2(at.x+cw,at.y+ch),c.border,4*s,0,s);}
 const auto pane=[&](const v2::Box& b,const char* key,int share,int lines){
  d->AddRectFilled(b.min,b.max,c.panel,4*s);
  const v2::Box in=v2::Inset(b,10*s,8*s);heading(in,std::string(loc::T(key))+" \xC2\xB7 "+std::to_string(share)+"%");
  for(int i=0;i<lines;++i){const float y=in.min.y+22*s+i*18*s;if(y+12*s<=in.max.y)d->AddRectFilled(ImVec2(in.min.x,y),ImVec2(in.max.x,y+12*s),c.button,3*s);}
 };
 const bool both=preferences_.roomShowMembers&&preferences_.roomShowChat;
 const auto folded=[&](const v2::Box& b,const char* key){d->AddRectFilled(b.min,b.max,c.panel,4*s);heading(v2::Inset(b,10*s,8*s),loc::T(key));};
 if(preferences_.roomShowMembers)pane(members,"room.members",both?preferences_.roomSplit:100,3);else folded(members,"room.members");
 if(preferences_.roomShowChat)pane(chat,"room.chat",both?100-preferences_.roomSplit:100,2);else folded(chat,"room.chat");
 RoomGrips(inside);
 ImGui::SetCursorScreenPos(p);ImGui::Dummy(ImVec2(w,h));
}

namespace {
// A panel's Hide or Show, whichever its rows hold this frame.
const MenuEntry* PanelSwitch(const RoomBoardFrame& f,const char* hide,const char* show) {
 const auto* e=f.Find(hide);
 return e?e:f.Find(show);
}
}

void ApplicationShell::DrawRoomMembersPanel(const RoomBoardFrame& f,v2::Box box,bool shown) const {
 const float s=f.s;const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
 f.Panel(d,box);v2::Box b=v2::Inset(box,12*s,8*s);
 v2::Box head=v2::CutTop(b,32*s,4*s);
 if(const auto* hide=PanelSwitch(f,"room-hide-members","room-show-members"))v2::CutRight(head,f.HideButton(d,*hide,ImVec2(head.max.x,head.min.y)),6*s);
 if(const auto* list=f.Find("room-members")){
  const bool hovered=f.Press(*list,head),focused=list->id==f.focus;
  v2::RowGround(d,head,focused,hovered);
  v2::SectionHeading(d,ImVec2(head.min.x+8*s,head.min.y+9*s),loc::Tf("room.members_heading",f.v.room.members.size(),f.v.room.capacity),c.muted);
  if(focused)v2::Focus(d,head.min,head.max);
 }
 if(!shown)return;
 ImGui::SetCursorScreenPos(b.min);
 ImGui::BeginChild("Room members",ImVec2(b.W(),b.H()),0,ImGuiWindowFlags_NoNavInputs);
 auto* md=ImGui::GetWindowDrawList();
 const auto start=ImGui::GetCursorScreenPos();float y=start.y;std::string group;
 for(const auto& e:f.rows){
  if(e.id.compare(0,7,"member-"))continue;
  if(e.group!=group){group=e.group;v2::SectionHeading(md,ImVec2(start.x+4*s,y+8*s),group,c.accent);y+=30*s;}
  const v2::Box r{ImVec2(start.x,y),ImVec2(start.x+b.W(),y+36*s)};y+=40*s;
  const bool hovered=f.Press(e,r),focused=e.id==f.focus;
  if(!ImGui::IsItemVisible())continue;
  v2::RowGround(md,r,focused,hovered);
  const auto view=RoomMemberCard(f.v,e);
  v2::TagView tag;tag.name=view.name;tag.marks=view.marks;tag.ping=view.latency;tag.status=view.status;
  v2::Tag(md,ImVec2(r.min.x+10*s,r.min.y+8*s),r.W()-20*s,tag,v2::TagSize::S);
  if(focused)v2::Focus(md,r.min,r.max);
  if(focused&&e.popout)f.choices={r,b};
 }
 ImGui::SetCursorScreenPos(start);ImGui::Dummy(ImVec2(b.W(),y-start.y));
 ImGui::EndChild();
}

void ApplicationShell::DrawRoomChatPanel(const RoomBoardFrame& f,v2::Box box,bool shown,unsigned unread) {
 const float s=f.s;const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
 f.Panel(d,box);v2::Box b=v2::Inset(box,12*s,8*s);
 v2::Box head=v2::CutTop(b,32*s,4*s);
 if(const auto* hide=PanelSwitch(f,"room-hide-chat","room-show-chat"))v2::CutRight(head,f.HideButton(d,*hide,ImVec2(head.max.x,head.min.y)),6*s);
 v2::SectionHeading(d,ImVec2(head.min.x,head.min.y+9*s),loc::T("room.chat_heading"),c.muted);
 if(unread){const std::string count=loc::Tf("room.v2.unread",unread);v2::Chip(d,ImVec2(head.max.x-v2::ChipWidth(count),head.min.y+6*s),count,c.accent);}
 if(!shown)return;
 const v2::Box message=v2::CutBottom(b,44*s,6*s);
 if(const auto* compose=f.Find("room-chat")){
  const bool hovered=f.Press(*compose,message),focused=compose->id==f.focus;
  f.Ground(d,message,focused?c.header:hovered?c.hover:c.frame,c.border);
  d->AddText(ImGui::GetFont(),16*s,ImVec2(message.min.x+12*s,message.min.y+14*s),c.muted,loc::T("room.v2.type_message"));
  if(focused)v2::Focus(d,message.min,message.max);
 }
 ImGui::SetCursorScreenPos(b.min);
 ImGui::BeginChild("Room chat",ImVec2(b.W(),b.H()),0,ImGuiWindowFlags_NoNavInputs);
 DrawChatLog(f.v,true);
 // Follow new messages only while the reader is already at the bottom.
 if(ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-1||chatShown_==0)ImGui::SetScrollHereY(1.f);
 chatShown_=transcript_.Lines().size();
 ImGui::EndChild();
}

} }
