#include "shell_journey_support.hxx"
#include "../ui/SettingsNavigation.hxx"

int main() try {
 using namespace sf4e;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const auto& entries){rows=entries;});
 const auto row=[&](const char* id)->const MenuEntry& {
  const auto it=std::find_if(rows.begin(),rows.end(),[&](const auto& e){return e.id==id;});
  if(it==rows.end())throw std::runtime_error(std::string("Missing setting: ")+id);return *it;
 };
 h.Choose("settings");h.Choose("interface");
 Check(rows.size()==3,"Interface is not grouped into three categories");
 for(const auto& category:std::vector<std::pair<const char*,std::vector<std::string>>>{
   {"menu-display",{"scale","language"}},
   {"match-hud",{"hud","hud-layout","hud-name-offset","hud-size","hud-position","hud-spacing"}},
   {"ready-alerts",{"ready-sound","ready-volume","ready-test"}}}) {
  h.Choose(category.first);
  Check(rows.size()==category.second.size(),"Category has unrelated or missing settings");
  for(std::size_t i=0;i<rows.size();++i)Check(rows[i].id==category.second[i],"Category order changed");
  h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="interface","Category Back skipped its parent");
 }
 // A dependent control explains exactly which setting to change first.
 h.view.preferences.readySound=false;h.Screen("ready-alerts");
 const auto soundReason=loc::Tf("menu.requires_value",loc::T("settings.ready_sound"),loc::T("common.on"));
 Check(!row("ready-volume").enabled&&row("ready-volume").detail==soundReason&&row("ready-test").detail==soundReason,"Disabled sound controls have no prerequisite");
 auto count=h.actions.size();h.Choose("ready-volume");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.size()==count,"Disabled sound control changed a preference");
 h.view.preferences.matchHudLayout=0;h.Screen("match-hud");
 Check(!row("hud-name-offset").enabled&&row("hud-name-offset").detail==loc::Tf("menu.requires_value",loc::T("settings.match_hud_layout"),MatchStripLayoutName(1)),"Name offset does not explain Split layout");
 // Locked values keep their descriptions and say how to become editable.
 h.view.canEditPreferences=false;JoinedRoom(h,18);h.Frame();
 for(const auto& setting:std::vector<std::pair<const char*,const char*>>{
  {"match-hud","hud-size"},{"ready-alerts","ready-volume"},{"training-replays","watched-picture"},
  {"discord","presence"},{"player","background-play"},{"profile","name"}}) {
  h.Screen(setting.first);const auto& e=row(setting.second);
  Check(!e.enabled&&e.detail.find(loc::T("settings.leave_room_to_edit"))==0,"Locked setting lacks a concrete reason");
 }
 h.Screen("menu-display");Check(row("language").enabled&&!row("scale").enabled,"Language inherited the match preference lock");
 h.view.session.room=netplay::RoomState::Idle;h.Frame();h.Screen("menu-display");
 Check(row("scale").detail==loc::T("settings.edit_at_main_menu"),"Offline restriction incorrectly asks to leave a room");
 // Text entry tells controller users that typing needs the keyboard before
 // they open the editor, in every supported language.
 for(int language=0;language<static_cast<int>(loc::Locale::Count);++language){
  loc::SetActive(static_cast<loc::Locale>(language));
  Check(TextRow("name","Name","",31).detail==loc::T("edit.instructions"),"Text entry lacks keyboard instructions");
 }
 loc::SetActive(loc::Locale::En);SetMenuEntriesProbe({});
 std::cout<<"Settings categories, prerequisites and keyboard instructions passed.\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
