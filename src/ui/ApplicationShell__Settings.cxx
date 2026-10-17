#include "ApplicationShell.hxx"
#include "SettingsNavigation.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include "../platform/LocaleWindows.hxx"
#include <algorithm>
#include <cstdio>
#include <imgui.h>

namespace sf4e { namespace ui {
namespace {
// "Automatic" first, then every locale by its own name.
std::vector<MenuChoice> LanguageChoices() {
    std::vector<MenuChoice> choices;std::string preference="auto";
    do {
        choices.push_back({preference,preference=="auto"?loc::Tf("settings.language.system_with",loc::NativeName(platform::ResolveUiLocale("auto"))):
            std::string(loc::NativeName(loc::ResolveLocale(preference,{},{})))});
        preference=std::string(loc::NextPreference(preference,1));
    } while(preference!="auto"&&choices.size()<64);
    return choices;
}
}
// Interface controls share the shell's draft and save path, like RoomSetupRows.
std::vector<MenuEntry> ApplicationShell::InterfaceRows(const ShellView& v,const std::string& screen,const char* reason,std::string& title) {
 std::vector<MenuEntry> rows;
 if(screen=="interface"){
  title=loc::T("settings.interface_title");
  rows={Row("menu-display",loc::T("settings.menu_display"),loc::T("settings.menu_display_detail")),
   Row("match-hud",loc::T("settings.match_hud"),loc::T("settings.match_hud_layout_detail")),
   Row("ready-alerts",loc::T("settings.ready_sound"),loc::T("settings.ready_sound_detail"))};
 }else if(screen=="match-hud"){
  title=loc::T("settings.match_hud");
  const char* hudSizes[]={loc::T("size.small"),loc::T("size.standard"),loc::T("size.large")};
  rows={Value("hud",loc::T("settings.match_hud"),preferences_.showMatchHud?loc::T("common.on"):loc::T("common.off"),reason,v.canEditPreferences),
   Value("hud-layout",loc::T("settings.match_hud_layout"),MatchStripLayoutName(preferences_.matchHudLayout),loc::T("settings.match_hud_layout_detail"),v.canEditPreferences),
   Value("hud-name-offset",loc::T("settings.name_offset"),MatchHudNameOffsetText(preferences_.matchHudNameOffset),
    preferences_.matchHudLayout==1?std::string(loc::T("settings.name_offset_detail")):loc::Tf("menu.requires_value",loc::T("settings.match_hud_layout"),MatchStripLayoutName(1)),v.canEditPreferences&&preferences_.matchHudLayout==1),
   Value("hud-size",loc::T("settings.match_hud_size"),hudSizes[(std::max)(0,(std::min)(2,preferences_.matchHudSize))],loc::T("settings.match_hud_size_detail"),v.canEditPreferences),
   Value("hud-position",loc::T("settings.match_hud_position"),MatchStripAnchorName(preferences_.matchHudAnchor),loc::T("settings.match_hud_position_detail"),v.canEditPreferences),
   Value("hud-spacing",loc::T("settings.edge_spacing"),preferences_.matchHudRaised?loc::T("spacing.raised"):loc::T("spacing.normal"),loc::T("settings.edge_spacing_detail"),v.canEditPreferences)};
 }else if(screen=="ready-alerts"){
  title=loc::T("settings.ready_sound");
  const auto soundReason=loc::Tf("menu.requires_value",loc::T("settings.ready_sound"),loc::T("common.on"));
  rows={Value("ready-sound",loc::T("settings.ready_sound"),preferences_.readySound?loc::T("common.on"):loc::T("common.off"),loc::T("settings.ready_sound_detail"),v.canEditPreferences),
   Value("ready-volume",loc::T("settings.ready_sound_volume"),std::to_string(preferences_.readySoundVolume)+"%",preferences_.readySound?std::string(loc::T("settings.ready_sound_volume_detail")):soundReason,v.canEditPreferences&&preferences_.readySound),
   Row("ready-test",loc::T("settings.ready_sound_test"),!v.canEditPreferences?reason:preferences_.readySound?std::string(loc::T("settings.ready_sound_test_detail")):soundReason,v.canEditPreferences&&preferences_.readySound)};
 }else if(screen=="menu-display"){
  title=loc::T("settings.menu_display");char size[32];std::snprintf(size,sizeof(size),"%.2fx",preferences_.interfaceScale);
  const auto languageValue=languagePreference_=="auto"?loc::Tf("settings.language.system_with",loc::NativeName(loc::Active())):
   std::string(loc::NativeName(loc::ResolveLocale(languagePreference_,{},{})));
  rows={Value("scale",loc::T("settings.interface_size"),size,reason,v.canEditPreferences),
   Value("language",loc::T("settings.language"),languageValue,languageSaveError_.empty()?std::string(loc::T("settings.language.detail")):languageSaveError_,true)};
  // Browsing the language list changes nothing until a choice is accepted.
  rows.back().choices=LanguageChoices();rows.back().chosen=languagePreference_;
 }
 return rows;
}
bool ApplicationShell::InterfaceActivate(const MenuAction& a,const ShellView& v,const Submit& submit) {
 if(IsInterfaceScreen(a.id)){menu_.navigation.Push(a.id);return true;}
 if(a.id=="ready-test"){
  // The volume on screen, which may not be saved yet.
  ShellAction r;r.command.generation=v.session.generation;r.previewSoundVolume=preferences_.readySoundVolume;
  if(!submit(std::move(r)))error_=loc::T("error.queue_failed");
  return true;
 }
 return false;
}
// HandleAdjust owns validation, dirty state and the common save deadline.
bool ApplicationShell::InterfaceAdjust(const MenuAction& a) {
 if(a.id=="hud-layout")preferences_.matchHudLayout=(std::max)(0,(std::min)(1,preferences_.matchHudLayout+a.delta));
  else if(a.id=="hud-name-offset")preferences_.matchHudNameOffset=(std::max)(-netplay::MaxMatchHudNameOffset,(std::min)(netplay::MaxMatchHudNameOffset,preferences_.matchHudNameOffset+2*a.delta));
  else if(a.id=="hud-size")preferences_.matchHudSize=(std::max)(0,(std::min)(2,preferences_.matchHudSize+a.delta));
  else if(a.id=="hud-position")preferences_.matchHudAnchor=(std::max)(0,(std::min)(netplay::MaxMatchHudAnchor,preferences_.matchHudAnchor+a.delta));
  else if(a.id=="hud-spacing")preferences_.matchHudRaised=a.delta>0;
  else if(a.id=="ready-sound")preferences_.readySound=a.delta>0;
  else if(a.id=="ready-volume")preferences_.readySoundVolume=(std::max)(10,(std::min)(100,preferences_.readySoundVolume+10*a.delta));
  else if(a.id=="scale")preferences_.interfaceScale=(std::max)(1.f,(std::min)(1.5f,preferences_.interfaceScale+.05f*a.delta));
  else if(a.id=="hud")preferences_.showMatchHud=a.delta>0;
  else return false;
  return true;
}
void ApplicationShell::SetLanguage(std::string preference) {
 // Not a netplay preference: it lives in the UI preferences file and saves on
 // its own deadline, so it stays out of the validate-and-save tail of HandleAdjust.
 if(!loc::ValidPreference(preference))return;
 languagePreference_=std::move(preference);
 loc::SetActive(platform::ResolveUiLocale(languagePreference_));
 languageDirty_=true;languageSaveError_.clear();languageSaveAt_=ImGui::GetTime()+.45;
}
} }
