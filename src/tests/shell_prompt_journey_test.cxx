#include "shell_journey_support.hxx"
void ButtonPromptJourneys() {
 using namespace sf4e;
 using namespace sf4e::gamepads;
 Harness h; h.view.inputDevice.type=input::PadXInput;
 h.view.gameplayBrand={Brand::PlayStation,PlayStationArt::PS5};
 SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B,"LP","LK",h.view.gameplayBrand);
 NoteMenuDevice(0); NoteMenuDevice(MenuInput::Down); NoteMenuDevice(0);
 std::vector<MenuEntry> rows;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
 h.Screen("player");
 const auto row=[&]() -> const MenuEntry& {
  auto found=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="button-prompts";});
  Check(found!=rows.end(),"Button prompts row missing"); return *found;
 };
 const auto prompts=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="button-prompts";});
 Check(prompts!=rows.begin()&&(prompts-1)->id=="keyboard"&&(prompts+1)->id=="controls","Button prompts row is not between Keyboard and Fighting buttons");
 Check(row().adjustable&&row().enabled&&!row().choices.empty()&&row().label=="Button prompts","Button prompts is not a standard enabled Value with its values offered");
 Check(row().value=="Automatic"&&row().detail.find("Showing PlayStation buttons.")!=std::string::npos&&MenuPromptBrand()==h.view.gameplayBrand,"Automatic did not detect the gameplay pad");
 // Select opens the values in a panel and changes nothing; Back closes it.
 const auto beforeSelect=h.actions.size(); h.Choose("button-prompts");
 Check(h.actions.size()==beforeSelect&&h.shell.Navigation().Choosing(),"Select did not open Button prompts' values, or activated it");
 h.Press(MenuInput::Back);Check(!h.shell.Navigation().Choosing()&&h.shell.Navigation().Screen()=="player","Back did not close Button prompts' values");
 const ButtonPrompts choices[]={ButtonPrompts::Xbox,ButtonPrompts::PlayStation,ButtonPrompts::Nintendo};
 const PromptBrand brands[]={{Brand::Xbox},{Brand::PlayStation,PlayStationArt::PS4},{Brand::Nintendo}};
 for(unsigned i=0;i<3;++i) {
  const auto before=h.actions.size(); h.Press(MenuInput::Right);
  Check(MenuPromptBrand()==brands[i],"Button prompts did not apply before autosave");
  Check(h.actions.size()==before,"Button prompts bypassed normal autosave");
  h.Frame(0,45);
  Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.buttonPrompts==choices[i],"Button prompts did not autosave the next style");
  h.view.preferences=h.actions.back().preferences;h.Frame();
 }
 h.Press(MenuInput::Right);Check(MenuPromptBrand()==PromptBrand{Brand::Nintendo},"Button prompts exceeded its last value");
 for(int i=2;i>=0;--i) {
  h.Press(MenuInput::Left);h.Frame(0,45);
  const auto want=static_cast<ButtonPrompts>(i);
  Check(h.actions.back().preferences.buttonPrompts==want,"Button prompts did not autosave the previous style");
  h.view.preferences=h.actions.back().preferences;h.Frame();
 }
 Check(MenuPromptBrand()==h.view.gameplayBrand,"Automatic did not restore the PS5 variant");
 h.Press(MenuInput::Left);Check(MenuPromptBrand()==h.view.gameplayBrand,"Button prompts exceeded Automatic");
 h.view.canEditPreferences=false;h.Frame();
 Check(!row().enabled&&row().detail==loc::T("settings.leave_room_to_edit"),"Locked Button prompts has the wrong rule or detail");
 const auto count=h.actions.size();h.Press(MenuInput::Right);h.Frame(0,45);Check(h.actions.size()==count,"Locked Button prompts changed preferences");
 h.view.canEditPreferences=true;h.view.gameplayBrand={Brand::Stick};h.Frame();
 Check(row().value=="Automatic"&&row().detail.find("Showing Fight buttons buttons.")!=std::string::npos,"Automatic Stick has the wrong value");
 h.FocusOn("controls");
 SetMenuEntriesProbe({});
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="home","Button prompts Back did not leave Player");
 // Detection is about the assigned gameplay pad, independent of the keyboard
 // being used last to navigate, which continues to show keyboard prompts.
 ImGui::GetIO().AddKeyEvent(ImGuiKey_UpArrow,true);h.Frame();NoteMenuDevice(0);
 Check(KeyboardPrompts(),"Keyboard use did not keep keyboard prompts");
 ImGui::GetIO().AddKeyEvent(ImGuiKey_UpArrow,false);h.Frame();
 NoteMenuDevice(0);NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);
 SetMenuGlyphs(input::PadKeyboard,0,0);
}
