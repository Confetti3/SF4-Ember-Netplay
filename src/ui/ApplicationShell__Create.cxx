#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
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
 // A saved template sets the rows below; they can still be changed after.
 const auto& templates=preferences_.roomTemplates;
 if(screen=="create"&&!templates.empty()){
  const bool chosen=createTemplate_>=0&&createTemplate_<static_cast<int>(templates.size());
  rows.push_back(Value("template",loc::T("room.template"),chosen?templates[createTemplate_].name:std::string(loc::T("room.template.none")),loc::T("room.template.detail"),can));
  rows.back().userText=chosen;
  if(chosen){rows.push_back(ConfirmRow("delete-template",loc::T("room.template.delete"),loc::Tf("room.template.delete.detail",templates[createTemplate_].name),can));rows.back().detailText=DetailText::Name;}
 }
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
 else if(a.id=="host"&&preferences_.roomPublic){error_.clear();publicRooms_.Create(preferences_.roomName,preferences_.roomCapacity,CreateTables(preferences_.publicTableRules),preferences_.roomDelayPolicy);}
 else if(a.id=="delete-template"&&createTemplate_>=0&&createTemplate_<static_cast<int>(preferences_.roomTemplates.size())){
  preferences_.roomTemplates.erase(preferences_.roomTemplates.begin()+createTemplate_);
  createTemplate_=-1;createTables_.reset();preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;
 }
 else if(a.id=="host")Send(CommandKind::HostRoom,v,submit);
}
// Create's own choice, never one of the saved preferences.
bool ApplicationShell::CreateAdjust(const MenuAction& a) {
 if(a.id=="template"){
  // None, then each saved template in turn.
  const int count=static_cast<int>(preferences_.roomTemplates.size());
  ApplyRoomTemplate((createTemplate_+1+(a.delta>0?1:count))%(count+1)-1);
  return true;
 }
 if(a.id!="practice-room")return false;
 createPractice_=a.delta>0;
 return true;
}
void ApplicationShell::ApplyRoomTemplate(int index) {
 createTemplate_=-1;createTables_.reset();
 if(index<0||index>=static_cast<int>(preferences_.roomTemplates.size()))return;
 // A copy: preferences_ is replaced below.
 const auto saved=preferences_.roomTemplates[index];
 auto next=preferences_;
 next.roomName=saved.roomName;next.roomCapacity=saved.capacity;next.roomPublic=saved.isPublic;next.roomDelayPolicy=saved.delayPolicy;
 (saved.isPublic?next.publicTableRules:next.tableRules)=saved.tables.rules[0];
 if(!next.Valid()){error_=loc::T("error.invalid_value");return;}
 preferences_=std::move(next);preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;error_.clear();
 createTemplate_=index;createTables_=saved.tables;createPractice_=saved.practice;
}
// The template's tables while the rows still show its rules; rules changed
// on the rows since go to every table, and the template's names stay.
netplay::TableSetup ApplicationShell::CreateTables(const room::Rules& rules) const {
 if(createTables_&&createTables_->rules[0]==rules)return *createTables_;
 auto tables=netplay::TableSetup::Same(rules);
 if(createTables_)tables.names=createTables_->names;
 return tables;
}
void ApplicationShell::SaveRoomTemplate(const room::Snapshot& room,const std::string& name) {
 netplay::RoomTemplate saved;
 saved.name=CutUtf8(name,32);saved.roomName=room.name;saved.capacity=room.capacity;saved.isPublic=room.serverOwned;saved.practice=room.practice;saved.delayPolicy=room.delayPolicy;
 for(const auto& table:room.tables)if(table.id<room::TableCount){saved.tables.rules[table.id]=table.rules;saved.tables.names[table.id]=table.name;}
 if(!saved.Valid()){error_=loc::T("error.invalid_value");return;}
 auto& templates=preferences_.roomTemplates;
 // The same name is saved over; a new one needs a free place.
 const auto same=std::find_if(templates.begin(),templates.end(),[&](const netplay::RoomTemplate& t){return t.name==saved.name;});
 if(same!=templates.end())*same=saved;
 else if(templates.size()>=netplay::MaxRoomTemplates){error_=loc::Tf("room.template.full",static_cast<int>(netplay::MaxRoomTemplates));return;}
 else templates.push_back(saved);
 preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;error_.clear();
 notice_=loc::Tf("room.template.saved",saved.name);noticeTone_=Tone::Success;noticeUntil_=ImGui::GetTime()+4;
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
 createPractice_=false;createTemplate_=-1;createTables_.reset();menu_.navigation.Push("create");
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
} }
