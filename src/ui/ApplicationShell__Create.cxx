#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cstdint>

namespace sf4e { namespace ui {
namespace {
// `text` cut to at most `limit` bytes, never inside a character.
std::string CutUtf8(std::string text,std::size_t limit) {
    if(text.size()<=limit)return text;
    while(limit>0&&(static_cast<unsigned char>(text[limit])&0xC0)==0x80)--limit;
    text.resize(limit);return text;
}
}
// Create and Defaults edit the same room settings, so their rows are built together.
std::vector<MenuEntry> ApplicationShell::RoomSetupRows(const ShellView& v,const std::string& screen,bool opening,const char* reason,std::string& title) {
 using namespace netplay;
 std::vector<MenuEntry> rows;
 title=screen=="create"?loc::T("room.create_title"):loc::T("settings.gameplay_defaults_title");const bool can=screen=="create"?v.canOpenRoom:v.canEditPreferences;
 // A public room is made by the service with the public default rules; the
 // ones chosen here follow once the creator is in it as host (netplay::CreatedRules).
 const bool publicRoom=screen=="create"&&preferences_.roomPublic;
 const std::string bridge=publicRoom?identity_.UsableBridge(v):std::string();
 if(screen=="defaults")rows.push_back(preferences_.autoInputDelay?Value("delay",loc::T("settings.input_delay"),loc::T("settings.input_delay.auto"),
  loc::Tf("room.input_delay.auto.detail",AutoInputDelayMinimum,AutoInputDelayMaximum)+"\n"+reason,can):
  Value("delay",loc::T("settings.input_delay"),std::to_string(preferences_.inputDelay),loc::Tf("room.selected_delay.detail",MinimumInputDelay,MaximumInputDelay)+"\n"+reason,can));
 if(screen=="create")rows.push_back(Value("visibility",loc::T("public.visibility"),loc::T(publicRoom?"public.visibility.public":"public.visibility.private"),loc::T("public.visibility_detail"),can));
 rows.push_back(TextRow("room-name",loc::T("room.name"),preferences_.roomName,64,can));
 rows.push_back(Value("capacity",loc::T("room.capacity"),std::to_string(preferences_.roomCapacity),loc::T("room.capacity_detail"),can));
 // Only a private room can be a Practice room, which has no round time to choose.
 const bool privateCreate=screen=="create"&&!publicRoom,practice=privateCreate&&createPractice_;
 if(privateCreate)rows.push_back(PracticeRoomRow(practice,can));
 rows.push_back(DelayPolicyRow(preferences_.roomDelayPolicy,can));
 RuleRows(rows,publicRoom?preferences_.publicTableRules:preferences_.tableRules,can,reason,practice);
 if(screen=="create"){rows.push_back(opening?ConfirmRow("cancel-open",loc::T("room.stop_creating_action"),loc::T("room.stop_creating"),true):
  Row("host",loc::T(publicRoom?"public.create":"online.create"),loc::T(!publicRoom?"room.create_requirements":bridge.empty()?"public.create_needs_id":"public.create_requirements"),
   can&&preferences_.Valid()&&!publicRooms_.Busy()&&(!publicRoom||!bridge.empty())));
  if(!opening)rows.back().hint=loc::T(publicRoom?"public.create":"online.create");}
 return rows;
}
// Opening Create, and the rows on it that start a room.
void ApplicationShell::CreateActivate(const MenuAction& a,const ShellView& v,const Submit& submit) {
 using namespace netplay;
 if(a.id=="create")OpenCreate();
 else if(a.id=="pr-create"||a.id=="pr-none")OpenPublicCreate();
 else if(a.id=="host"&&preferences_.roomPublic){error_.clear();publicRooms_.Create(preferences_.roomName,preferences_.roomCapacity,preferences_.publicTableRules,preferences_.roomDelayPolicy);}
 else if(a.id=="host")Send(CommandKind::HostRoom,v,submit);
}
// Create's own choice, never one of the saved preferences.
bool ApplicationShell::CreateAdjust(const MenuAction& a) {
 if(a.id!="practice-room")return false;
 createPractice_=a.delta>0;
 return true;
}

// The public-room service is the one the Ember ID panel selects when its
// service list arrives. Public rooms, and Create while Visibility is Public
// (restored from the preferences or just chosen), both need it, so this is the
// one place that asks: once per stay in such a state, when no service is
// selected, and not over a request the panel is still waiting on.
void ApplicationShell::UpdatePublicBridge(const ShellView& v,const std::string& screen) {
 const bool needed=PublicRoomsPanel::Owns(screen)||RankingsPanel::Owns(screen)||(screen=="create"&&preferences_.roomPublic);
 if(!needed){publicBridgeAsked_=false;return;}
 if(publicBridgeAsked_)return;
 if(identity_.UsableBridge(v).empty()){
  if(identity_.Waiting())return;
  identity_.Probe();
 }
 publicBridgeAsked_=true;
}

// Create public room opens Create on Public, which stays the default until changed.
void ApplicationShell::OpenCreate() {
 createPractice_=false;menu_.navigation.Push("create");
}
void ApplicationShell::OpenPublicCreate() {
 if(!preferences_.roomPublic){preferences_.roomPublic=true;preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;}
 ApplyPublicDefaultName();
 error_.clear();OpenCreate();
}
// The room still has the private default name, which would list as "Private room":
// a public room is named for its host instead, within the name's own limit.
void ApplicationShell::ApplyPublicDefaultName() {
 if(preferences_.roomName!=netplay::PlayerPreferences{}.roomName)return;
 const std::size_t overhead=loc::Tf("public.default_name","").size();
 const std::string name=CutUtf8(preferences_.displayName,overhead<64?64-overhead:0);
 if(name.empty())return;
 preferences_.roomName=loc::Tf("public.default_name",name);
 preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;
}

// V2's Create, as the design's board: the room's settings and the table's rules
// in two panels (one above the other on a narrow screen), the focused row's
// words under them and the button that opens the room. The rows and their
// order are RoomSetupRows'; up and down still walk them in that order.
void ApplicationShell::DrawCreateBoard(const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,const MenuVisualFeedback& feedback) {
 const float s=Scale(),width=ImGui::GetContentRegionAvail().x,rowHeight=48*s,valueWidth=164*s,font=ImGui::GetFontSize();
 const auto& c=v2::Colors();
 auto* d=ImGui::GetWindowDrawList();
 const auto focus=nav.Focus();
 const auto mouse=ImGui::GetIO().MouseDelta;
 const bool pointerMoved=mouse.x!=0||mouse.y!=0,changed=focus!=createFocus_;
 createFocus_=focus;
 // Room: up to its delay policy; Rules: the rest; the button: Create or Stop.
 std::vector<const MenuEntry*> panels[2];const MenuEntry* button=nullptr;
 for(std::size_t i=0,p=0;i<rows.size();++i){
  if(rows[i].id=="host"||rows[i].id=="cancel-open")button=&rows[i];
  else{panels[p].push_back(&rows[i]);if(rows[i].id=="delay-policy")p=1;}
 }
 // A row's box: its value's ends step it, anything else on it is Select; the moving pointer focuses it.
 const auto press=[&](const MenuEntry& e,const v2::Box& b){
  ImGui::SetCursorScreenPos(b.min);ImGui::PushID(e.id.c_str());
  const bool pressed=ImGui::InvisibleButton("##create",ImVec2(b.W(),b.H())),hovered=ImGui::IsItemHovered();
  ImGui::PopID();
  if(e.id==focus&&changed)ImGui::SetScrollHereY(.5f);
  if(pressed&&!nav.Asking()&&!nav.Editing()&&!nav.Choosing()){
   nav.Focus(e.id,rows);
   const float mx=ImGui::GetMousePos().x,vx=b.max.x-14*s-valueWidth;
   const int step=e.adjustable&&e.enabled&&mx>=vx?(mx<vx+valueWidth*.5f?-1:1):0;
   action=step?MenuAction{MenuAction::Adjust,e.id,{},step}:nav.Choose(rows);
  }else if(hovered&&pointerMoved&&!nav.Choosing())nav.Focus(e.id,rows);
  return hovered;
 };
 const auto origin=ImGui::GetCursorScreenPos();
 const bool across=!v2::Narrow(ImGui::GetWindowWidth(),s);
 const float panelWidth=across?(width-24*s)*.5f:width;
 const char* names[]={loc::T("screen.room"),loc::T("room.table_rules")};
 float bottom=origin.y;
 for(int i=0;i<2;++i){
  const ImVec2 at(origin.x+(across?i*(panelWidth+24*s):0),across||!i?origin.y:bottom+12*s);
  const float inner=panels[i].size()*(rowHeight+4*s);
  const v2::Box panel{at,ImVec2(at.x+panelWidth,at.y+40*s+12*s+inner)};
  d->AddRectFilled(panel.min,panel.max,v2::color::Alpha(c.panel,.94f),4*s);
  d->AddLine(ImVec2(at.x,at.y+40*s),ImVec2(panel.max.x,at.y+40*s),c.border,s);
  d->AddRect(panel.min,panel.max,c.border,4*s,0,s);
  d->AddText(HeadingFont(),20*s,ImVec2(at.x+16*s,at.y+10*s),c.text,names[i]);
  float y=at.y+48*s;
  for(const MenuEntry* row:panels[i]){
   const auto& e=*row;
   const v2::Box r{ImVec2(at.x+12*s,y),ImVec2(panel.max.x-12*s,y+rowHeight)};
   y+=rowHeight+4*s;
   const bool focused=e.id==focus,hovered=press(e,r),enabled=feedback.Enabled(e);
   if(focused||hovered)d->AddRectFilled(r.min,r.max,focused?c.header:c.hover,4*s);
   const ImU32 text=enabled?c.text:c.muted,arrow=enabled&&e.adjustable?c.accent:c.muted;
   const float ty=r.min.y+(rowHeight-font)*.5f,vx=r.max.x-14*s-valueWidth;
   d->PushClipRect(r.min,ImVec2(vx-8*s,r.max.y),true);
   d->AddText(ImVec2(r.min.x+14*s,ty),text,e.label.c_str());
   d->PopClipRect();
   // The value between the arrows that step it, or with a chevron when Select opens it.
   if(e.userText||e.text)NoteUserText(e.value);
   const std::string value=e.value.empty()&&e.adjustable?loc::T("common.not_set"):e.value;
   const float tw=(std::min)(ImGui::CalcTextSize(value.c_str()).x,valueWidth-40*s);
   d->PushClipRect(ImVec2(vx+20*s,r.min.y),ImVec2(vx+valueWidth-20*s,r.max.y),true);
   d->AddText(ImVec2(vx+(valueWidth-tw)*.5f,ty),e.valueHighlighted?c.accent:text,value.c_str());
   d->PopClipRect();
   if(e.adjustable)d->AddText(ImVec2(vx,ty),arrow,"<");
   d->AddText(ImVec2(r.max.x-14*s-ImGui::CalcTextSize(">").x,ty),arrow,">");
   if(focused){
    v2::Focus(d,r.min,r.max);
    if(!e.choices.empty())menu_.bodyChoices={r,{panel.min,ImVec2(panel.max.x,origin.y+height)}};
   }
  }
  bottom=(std::max)(bottom,panel.max.y);
 }
 // The focused row's words, then the button.
 const auto shown=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==focus;});
 const v2::Box words{ImVec2(origin.x,bottom+12*s),ImVec2(origin.x+width,bottom+12*s+52*s)};
 d->AddRectFilled(words.min,words.max,c.frame,4*s);
 d->AddRect(words.min,words.max,c.border,4*s,0,s);
 if(shown!=rows.end()){
  d->PushClipRect(words.min,words.max,true);
  d->AddText(HeadingFont(),16*s,ImVec2(words.min.x+16*s,words.min.y+6*s),c.text,shown->label.c_str());
  d->AddText(ImGui::GetFont(),15*s,ImVec2(words.min.x+16*s,words.min.y+28*s),c.muted,shown->detail.substr(0,shown->detail.find('\n')).c_str());
  d->PopClipRect();
 }
 float end=words.max.y;
 if(button){
  const auto& e=*button;
  const bool focused=e.id==focus,enabled=feedback.Enabled(e);
  const float w=(std::min)(width,(std::max)(260*s,ImGui::CalcTextSize(e.label.c_str()).x+48*s));
  const v2::Box b{ImVec2(origin.x+width-w,end+12*s),ImVec2(origin.x+width,end+12*s+52*s)};
  const bool hovered=press(e,b);
  d->AddRectFilled(b.min,b.max,!enabled?c.button:focused||hovered?c.accentLight:c.accent,4*s);
  if(focused)d->AddRect(b.min,b.max,c.text,4*s,0,3*s);
  const float tw=HeadingFont()->CalcTextSizeA(18*s,FLT_MAX,0,e.label.c_str()).x;
  d->AddText(HeadingFont(),18*s,ImVec2(b.min.x+(b.W()-tw)*.5f,b.min.y+17*s),enabled?c.ink:c.muted,e.label.c_str());
  end=b.max.y;
 }
 ImGui::SetCursorScreenPos(ImVec2(origin.x,end));
 ImGui::Dummy(ImVec2(width,0));
}
} }
