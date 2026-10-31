#include "shell_journey_support.hxx"
namespace {
// A deciding Arcade pair's prompt on a DirectInput stick, which has no
// Fighter or Options shortcut (MenuShortcuts): every answer is a row, so
// Character select is reached with the D-pad and chosen with Select.
void DecidingPromptOnStick() {
 using namespace sf4e;
 Harness a;a.view.inputDevice.type=input::PadDirectInput;
 input::MenuButtons stick;stick.confirm=1u<<18;stick.back=1u<<17;stick.confirmGlyph="LK";stick.backGlyph="MK";
 SetMenuGlyphs(input::PadDirectInput,stick);
 NoteMenuDevice(0);NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);
 Check(!MenuShortcutGlyph(MenuInput::Fighter)&&!MenuShortcutGlyph(MenuInput::Options),"The stick was given a Fighter or Options shortcut");
 JoinedRoom(a,16);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
 a.view.room.host=1;a.view.room.revision=3;a.view.room.arcade=room::ArcadeRoom;
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;a.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;a.view.room.members.push_back(peer);
 auto& t=a.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;
 a.view.room.cabinets[0]=1;a.view.room.cabinets[8]=2;
 a.Screen("room");
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 std::map<std::string,std::string> words;SetMenuCardTextProbe([&](const std::string& id,const std::string& text){words[id]=text;});
 // The set is over and the pair decides.
 t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=0;t.deciding=true;t.rematch[0]=t.rematch[1]=false;
 a.view.matchResult=false;a.Frame();
 a.view.session.match=netplay::MatchState::Playing;a.view.atMainMenu=false;a.view.matchResult=true;a.Frame();
 std::vector<std::string> ids;for(const auto& e:rows)ids.push_back(e.id);
 Check(a.shell.Navigation().Screen()=="post-match"&&ids==std::vector<std::string>{"rematch","new-opponent","fighter","back"},
  "A deciding Arcade prompt's buttons were not Rematch, New opponent, Character select and Back to room");
 const auto sent=a.actions.size();
 a.FocusOn("fighter");a.Press(MenuInput::Select);
 Check(words["pm-you"]==loc::T("postmatch.character_select")&&a.actions.size()==sent,"Character select chosen by D-pad and Select on a stick was not taken");
 // Once the game is over the room opens on fighter select.
 a.view.matchResult=false;a.view.session.match=netplay::MatchState::PostMatch;a.view.atMainMenu=true;t.phase=room::TablePhase::Waiting;a.open=true;a.Frame(0,5);
 Check(a.shell.Navigation().Screen()=="selection","Character select from a deciding prompt did not open fighter select");
 SetMenuEntriesProbe({});SetMenuCardTextProbe({});
 SetMenuGlyphs(input::PadKeyboard,0,0);
}
}
static void ButtonPromptsRow() {
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
// Each journey has its own harness, one ImGui context at a time.
void ButtonPromptJourneys() {
 ButtonPromptsRow();
 DecidingPromptOnStick();
}
