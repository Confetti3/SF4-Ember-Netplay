#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
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
 // Any new room has a type; only a private one can be a Practice room, which
 // has no round time to choose.
 const bool privateCreate=screen=="create"&&!publicRoom,practice=privateCreate&&createPractice_,arcade=screen=="create"&&createArcade_;
 if(screen=="create")rows.push_back(RoomTypeRow(createArcade_,can));
 if(privateCreate)rows.push_back(PracticeRoomRow(practice,can));
 rows.push_back(DelayPolicyRow(preferences_.roomDelayPolicy,can));
 RuleRows(rows,publicRoom?preferences_.publicTableRules:preferences_.tableRules,can,reason,practice);
 // An Arcade pair rematches after every set, so nobody stays or rotates.
 if(arcade)rows.erase(std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="rotation";}));
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
 else if(a.id=="host"&&preferences_.roomPublic){error_.clear();publicRooms_.Create(preferences_.roomName,preferences_.roomCapacity,preferences_.publicTableRules,preferences_.roomDelayPolicy,createArcade_);}
 else if(a.id=="host")Send(CommandKind::HostRoom,v,submit);
}
// Create's own choice, never one of the saved preferences.
bool ApplicationShell::CreateAdjust(const MenuAction& a) {
 if(a.id=="room-type"){createArcade_=static_cast<room::RoomType>((std::max)(0,(std::min)(2,static_cast<int>(createArcade_)+a.delta)));return true;}
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
 createPractice_=false;createArcade_=room::ClassicRoom;menu_.navigation.Push("create");
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
 const float s=Scale(),width=ImGui::GetContentRegionAvail().x,rowHeight=48*s,valueWidth=164*s;
 auto* d=ImGui::GetWindowDrawList();
 const auto focus=nav.Focus();
 const bool changed=focus!=createFocus_;
 createFocus_=focus;
 // Room: up to its delay policy; Rules: the rest; the button: Create or Stop.
 std::vector<const MenuEntry*> panels[2];const MenuEntry* button=nullptr;
 for(std::size_t i=0,p=0;i<rows.size();++i){
  if(rows[i].id=="host"||rows[i].id=="cancel-open")button=&rows[i];
  else{panels[p].push_back(&rows[i]);if(rows[i].id=="delay-policy")p=1;}
 }
 const auto origin=ImGui::GetCursorScreenPos();
 const bool across=!v2::Narrow(ImGui::GetWindowWidth(),s);
 const float panelWidth=across?(width-24*s)*.5f:width;
 const char* names[]={loc::T("screen.room"),loc::T("room.table_rules")};
 float bottom=origin.y;
 for(int i=0;i<2;++i){
  const ImVec2 at(origin.x+(across?i*(panelWidth+24*s):0),across||!i?origin.y:bottom+12*s);
  const float inner=panels[i].size()*(rowHeight+4*s);
  const v2::Box panel{at,ImVec2(at.x+panelWidth,at.y+40*s+12*s+inner)};
  float y=v2::Panel(d,panel,names[i]).min.y+8*s;
  for(const MenuEntry* row:panels[i]){
   const auto& e=*row;
   const v2::Box r{ImVec2(at.x+12*s,y),ImVec2(panel.max.x-12*s,y+rowHeight)};
   y+=rowHeight+4*s;
   const bool focused=e.id==focus,hovered=PressEntry(nav,rows,e,r,action,changed,valueWidth).hovered;
   v2::RowGround(d,r,focused,hovered);
   DrawMenuRow(d,e,r,feedback.Enabled(e),valueWidth,14*s);
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
 v2::Words(d,words,shown!=rows.end()?shown->label:std::string(),shown!=rows.end()?shown->detail:std::string(),false);
 float end=words.max.y;
 if(button){
  const auto& e=*button;
  const float w=(std::min)(width,(std::max)(260*s,ImGui::CalcTextSize(e.label.c_str()).x+48*s));
  const v2::Box b{ImVec2(origin.x+width-w,end+12*s),ImVec2(origin.x+width,end+12*s+48*s)};
  v2::Button(d,b,e.label,v2::ButtonKind::Primary,e.id==focus,PressEntry(nav,rows,e,b,action,changed).hovered,feedback.Enabled(e));
  end=b.max.y;
 }
 ImGui::SetCursorScreenPos(ImVec2(origin.x,end));
 ImGui::Dummy(ImVec2(width,0));
}
} }
