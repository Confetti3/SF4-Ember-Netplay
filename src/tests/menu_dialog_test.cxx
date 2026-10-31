// The rendered menu's dialogs, as every GameMenu screen has them: a dialog
// owns the input, the hint bar's hints press their buttons, and a held
// direction repeats in a notice and on the on-screen keyboard. Linked into
// the ControllerNavigation test, whose main runs them in its order.
#include "../ui/ApplicationShell.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "../ui/Theme.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/MenuGlyphs.hxx"
#include "../ui/MenuPresentation.hxx"
#include "imgui_test_support.hxx"
#include <imgui.h>
#include <imgui_internal.h>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace sf4e::ui;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}
// The rules every GameMenu screen shares with the room: an open dialog owns
// the input (Select takes its highlighted button, Back cancels), pad Select
// accepts a draft, information rows offer no Select, a reader
// scrolls and closes, a list choice opens on the saved option, and only a
// moving pointer takes the selection.
void DialogContract(){
 HeadlessImGui imgui;GameMenu menu;SetMenuGlyphs(3,0x40000,0x20000);
 menu.shortcutHints={{"X","Fighter"}};
 auto language=Value("language","Language","B","Pick one");language.choices={{"a","A"},{"b","B"},{"c","C"}};language.chosen="b";
 auto info=Row("record","Record","12 wins");info.info=true;
 auto reader=Row("licence","Licence","Long text");reader.reading=true;
 std::vector<MenuEntry> rows={ConfirmRow("leave","Leave room","Disconnect"),TextRow("name","Name","Kate",31),info,reader,language};
 std::map<std::string,ImVec2> centres;std::map<std::string,std::pair<ImVec2,ImVec2>> boxes;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){centres[id]=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);boxes[id]={min,max};});
 auto frame=[&](unsigned held=0){imgui.io.DeltaTime=1.f/60;SetMenuInput({held,0});ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(imgui.io.DisplaySize);ImGui::Begin("Dialog test",nullptr,ImGuiWindowFlags_NoDecoration);
  const auto a=menu.Draw("TEST",rows);ImGui::End();ImGui::Render();return a;};
 auto press=[&](unsigned held){frame();const auto a=frame(held);frame();return a;};
 frame();frame();
 press(MenuInput::Select);frame();
 Check(menu.navigation.Confirming(),"Select did not open the confirmation");
 press(MenuInput::Right);frame();Check(menu.navigation.ConfirmSelected(),"Right did not highlight the confirmation's action");
 press(MenuInput::Back);Check(!menu.navigation.Confirming(),"Back did not cancel the confirmation");
 // The opening press can arrive with pointer movement over the action button.
 // Preserve Cancel until a later movement deliberately selects that button.
 imgui.io.AddMousePosEvent(1,1);frame();frame();
 const auto confirmAt=centres.at("confirm/1");
 imgui.io.AddMousePosEvent(confirmAt.x,confirmAt.y);frame(MenuInput::Select);frame();
 Check(menu.navigation.Confirming()&&!menu.navigation.ConfirmSelected(),"Opening pointer movement replaced the safe confirmation default");
 const auto declined=press(MenuInput::Select);
 Check(declined.kind==MenuAction::None&&!menu.navigation.Confirming(),"Default confirmation activated the action");
 press(MenuInput::Select);frame();
 Check(menu.navigation.Confirming()&&!menu.navigation.ConfirmSelected(),"Reopened confirmation inherited Send or a resting pointer");
 imgui.io.AddMousePosEvent(confirmAt.x+2,confirmAt.y);frame();
 Check(menu.navigation.ConfirmSelected(),"Pointer movement after opening did not select the action");
 const auto confirmed=press(MenuInput::Select);
 Check(confirmed.kind==MenuAction::Activate&&confirmed.id=="leave","Explicit pointer selection did not confirm the action");
 imgui.io.AddMousePosEvent(1,1);frame();
 menu.navigation.Focus("name",rows);press(MenuInput::Select);frame();
 Check(menu.navigation.Editing(),"Select did not open the editor");
 // The pad types on the on-screen keyboard, where Start is Done.
 const auto accepted=press(MenuInput::Start);
 Check(accepted.kind==MenuAction::TextAccepted&&accepted.text=="Kate","Controller Start did not accept the draft");
 // With the keyboard's prompts the editor has Cancel and Accept buttons: a
 // pointer moved onto Cancel keeps that highlight, and Select then cancels
 // rather than accepting.
 SetMenuGlyphs(sf4e::input::PadKeyboard,0,0);
 press(MenuInput::Select);imgui.io.AddMousePosEvent(1,1);frame();
 {const auto at=centres.at("edit/0");imgui.io.AddMousePosEvent(at.x,at.y);frame();frame();}
 Check(menu.navigation.Editing()&&!menu.navigation.EditAccepts(),"The pointer's Cancel highlight was not kept");
 Check(press(MenuInput::Select).kind==MenuAction::None&&!menu.navigation.Editing(),"Select after the pointer chose Cancel accepted the draft");
 SetMenuGlyphs(3,0x40000,0x20000);
 imgui.io.AddMousePosEvent(1,1);frame();
 menu.ShowNotice(NoticeDialog{"Advice","Heading",{{"OK"},{"Don't show again",NoticeResult::HideGameSettings}}});frame();frame();
 Check(menu.NoticeOpen(),"The notice did not open");
 press(MenuInput::Right);
 press(MenuInput::Select);Check(menu.TakeNoticeResult()==NoticeResult::HideGameSettings&&!menu.NoticeOpen(),"The notice's second button was not taken");
 Check(menu.TakeNoticeResult()==NoticeResult::None,"A notice result was read twice");
 // The first button is highlighted first and Select takes it; Back never takes the highlighted one.
 menu.ShowNotice(NoticeDialog{"Advice","Heading",{{"OK"},{"Don't show again",NoticeResult::HideGameSettings}}});frame();frame();
 press(MenuInput::Select);Check(menu.TakeNoticeResult()==NoticeResult::Acknowledged&&!menu.NoticeOpen(),"The first button did not read as acknowledged");
 menu.ShowNotice(NoticeDialog{"Advice","Heading",{{"OK"},{"Don't show again",NoticeResult::HideGameSettings}}});frame();frame();
 press(MenuInput::Right);press(MenuInput::Back);Check(menu.TakeNoticeResult()==NoticeResult::Dismissed&&!menu.NoticeOpen(),"Back took the highlighted button");
 // More than two buttons stop at both ends, and a new notice drops an answer nobody took.
 menu.ShowNotice(NoticeDialog{"Three","Heading",{{"First"},{"Second",NoticeResult::CreatePublicRoom},{"Third",NoticeResult::OpenButtonLayout}}});frame();frame();
 press(MenuInput::Left);press(MenuInput::Right);press(MenuInput::Right);press(MenuInput::Right);frame();
 press(MenuInput::Select);Check(menu.TakeNoticeResult()==NoticeResult::OpenButtonLayout,"The third button's result was lost");
 menu.ShowNotice("One");frame();frame();press(MenuInput::Select);menu.ShowNotice("Two");
 Check(menu.TakeNoticeResult()==NoticeResult::None,"A new notice inherited the last one's result");
 menu.CloseNotice();frame();frame();
 menu.navigation.Focus("record",rows);frame();frame();
 Check(press(MenuInput::Select).kind==MenuAction::None,"An information row offers Select");
 menu.navigation.Focus("licence",rows);frame();frame();
 press(MenuInput::Select);frame();Check(menu.navigation.Reading(),"Select did not open the reader");
 frame(MenuInput::Down);frame(MenuInput::Down);
 Check(press(MenuInput::Back).kind==MenuAction::None&&!menu.navigation.Reading()&&menu.navigation.Screen()=="home","Back did more than close the reader");
 menu.navigation.Focus("language",rows);frame();frame();
 press(MenuInput::Select);Check(menu.navigation.Choosing()&&menu.navigation.ChoiceIndex()==1,"The choice did not open on the saved option");
 // Three options sit side by side, so Right steps to the next.
 press(MenuInput::Right);const auto chosen=press(MenuInput::Select);
 Check(chosen.kind==MenuAction::Chosen&&chosen.id=="language"&&chosen.text=="c","Right and Select did not choose the next option");
 // A click is one intent: the value's middle opens the list without changing
 // it, its arrow adjusts without opening, and Back closes the list.
 const auto click=[&](ImVec2 at){imgui.io.AddMousePosEvent(at.x,at.y);frame();imgui.io.AddMouseButtonEvent(0,true);const auto a=frame();
  imgui.io.AddMouseButtonEvent(0,false);const auto b=frame();frame();return a.kind!=MenuAction::None?a:b;};
 const auto row=boxes.at("language");const float y=(row.first.y+row.second.y)*.5f;
 auto a=click(ImVec2(row.second.x-110,y));
 Check(a.kind!=MenuAction::Adjust&&menu.navigation.Choosing(),"Clicking the language value changed it or did not open the list");
 a=press(MenuInput::Back);
 Check(a.kind==MenuAction::None&&!menu.navigation.Choosing(),"Back chose something or left the list open");
 a=click(ImVec2(row.second.x-12,y));
 Check(a.kind==MenuAction::Adjust&&a.delta==1&&!menu.navigation.Choosing(),"The language arrow did not only step the value");
 // Narrow, a long label puts the value under it: a click on the label, even
 // straight over the value's arrow, opens the list; only the value's own line
 // steps it. A card and a board's row (GameMenu::DrawBoardRow) alike.
 {const std::string label=rows[4].label;rows[4].label="A language with a name long enough to need a line of its own";
  imgui.io.DisplaySize=ImVec2(400,900);frame();frame();
  const auto stacked=boxes.at("language");
  a=click(ImVec2(stacked.second.x-12,stacked.first.y+10));
  Check(a.kind!=MenuAction::Adjust&&menu.navigation.Choosing(),"A click on a stacked card's label over its arrow adjusted it");
  press(MenuInput::Back);
  a=click(ImVec2(stacked.second.x-12,stacked.second.y-12));
  Check(a.kind==MenuAction::Adjust&&a.delta==1&&!menu.navigation.Choosing(),"A stacked card's right arrow did not step it");
  v2::Box drawn{};
  const GameMenu::Body board=[&](const std::vector<MenuEntry>& entries,MenuNavigation&,MenuAction& action,float,const MenuVisualFeedback&){
   drawn=menu.DrawBoardRow(entries,entries[4],ImGui::GetCursorScreenPos(),ImGui::GetContentRegionAvail().x,action,{}).box;};
  const auto once=[&]{imgui.io.DeltaTime=1.f/60;SetMenuInput({0,0});ImGui::NewFrame();
   ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(imgui.io.DisplaySize);ImGui::Begin("Dialog test",nullptr,ImGuiWindowFlags_NoDecoration);
   const auto r=menu.Draw("TEST",rows,"",{},1,{},board);ImGui::End();ImGui::Render();return r;};
  const auto boardClick=[&](ImVec2 at){
   imgui.io.AddMousePosEvent(at.x,at.y);once();imgui.io.AddMouseButtonEvent(0,true);const auto down=once();
   imgui.io.AddMouseButtonEvent(0,false);const auto up=once();once();return down.kind!=MenuAction::None?down:up;};
  once();once();
  Check(drawn.H()>50,"The board's long row kept its value beside its label");
  a=boardClick(ImVec2(drawn.max.x-16,drawn.min.y+10));
  Check(a.kind!=MenuAction::Adjust&&menu.navigation.Choosing(),"A click on a stacked board row's label over its arrow adjusted it");
  press(MenuInput::Back);
  a=boardClick(ImVec2(drawn.max.x-16,drawn.max.y-12));
  Check(a.kind==MenuAction::Adjust&&a.delta==1&&!menu.navigation.Choosing(),"A stacked board row's right arrow did not step it");
  rows[4].label=label;imgui.io.DisplaySize=ImVec2(1280,960);frame();frame();}
 // The pointer: resting it on a row changes nothing; moving it there selects.
 menu.navigation.Focus("leave",rows);imgui.io.AddMousePosEvent(1,1);frame();frame();
 const auto target=centres.at("record");imgui.io.AddMousePosEvent(target.x,target.y);frame();
 Check(menu.navigation.Focus()=="record","A moving pointer did not select the row under it");
 press(MenuInput::Up);frame();frame();
 Check(menu.navigation.Focus()=="name","A resting pointer took the selection back from the pad");
 // Readers keep their own scroll: scroll one long document, close it, and
 // the next opens at its heading; so does the first, read again.
 {std::string text;for(int i=0;i<200;++i)text+="Line "+std::to_string(i)+"\n";
  auto first=Row("first","First",text);first.reading=true;auto second=Row("second","Second",text);second.reading=true;
  rows={first,second};
  const auto top=[&]{const auto reader=boxes.at("reader"),heading=boxes.at("reader-heading");return heading.first.y>=reader.first.y-.5f;};
  menu.navigation.Focus("first",rows);press(MenuInput::Select);frame();
  Check(menu.navigation.Reading()&&top(),"A reader did not open at its heading");
  for(int i=0;i<60;++i)frame(MenuInput::Down);frame();
  Check(!top(),"Holding Down did not scroll the reader");
  press(MenuInput::Back);menu.navigation.Focus("second",rows);press(MenuInput::Select);frame();
  Check(menu.navigation.ReadingId()=="second"&&top(),"A second reader inherited the first one's scroll");
  press(MenuInput::Back);menu.navigation.Focus("first",rows);press(MenuInput::Select);frame();
  Check(top(),"A reread document did not open at its heading");
  press(MenuInput::Back);}
 // A notice over an editor owns Enter; the editor resumes with its draft.
 rows={TextRow("name","Name","Kate",31)};menu.navigation.Focus("name",rows);
 imgui.io.AddMousePosEvent(1,1);frame();press(MenuInput::Select);frame();
 menu.ShowNotice("Advice");frame();frame();
 imgui.io.AddKeyEvent(ImGuiKey_Enter,true);frame();
 Check(!menu.NoticeOpen()&&menu.navigation.Editing()&&menu.navigation.Draft()=="Kate","Enter did not dismiss only the editor's notice");
 imgui.io.AddKeyEvent(ImGuiKey_Enter,false);frame();frame();
 // Single-line editors still accept either Enter key, even with Cancel lit.
 for(const auto enter:{ImGuiKey_Enter,ImGuiKey_KeypadEnter}) {
  menu.navigation.EditAccepts(false);imgui.io.AddKeyEvent(enter,true);const auto accepted=frame();
  Check(accepted.kind==MenuAction::TextAccepted&&accepted.text=="Kate"&&!menu.navigation.Editing(),"Single-line Enter did not accept the draft");
  imgui.io.AddKeyEvent(enter,false);frame();frame();
  if(enter==ImGuiKey_Enter){press(MenuInput::Select);frame();}
 }
 SetMenuTextProbe({});SetMenuCardProbe({});
}
// A hint on the bar presses its button: the click is read on the next frame
// as the pad's press is, by whatever owns the input then. An open choice's
// Select hint picks its highlighted option; the screen's shortcuts wait while
// it is open, as their buttons do; and a confirmation's popup takes the
// pointer, so under it the hints only name its buttons.
void HintPresses(){
 HeadlessImGui imgui;GameMenu menu;SetMenuGlyphs(3,0x40000,0x20000);
 imgui.io.DisplaySize=ImVec2(1280,960);
 menu.shortcutHints={{"X","Fighter",MenuInput::Fighter}};
 auto language=Value("language","Language","B","Pick one");language.choices={{"a","A"},{"b","B"},{"c","C"}};language.chosen="b";
 std::vector<MenuEntry> rows={language,ConfirmRow("leave","Leave room","Disconnect")};
 // A board, so the choice opens as a value panel beside its row, not as a popup.
 const GameMenu::Body board=[&](const std::vector<MenuEntry>& entries,MenuNavigation&,MenuAction& action,float,const MenuVisualFeedback&){
  for(const auto& e:entries){const auto at=ImGui::GetCursorScreenPos();const auto box=menu.DrawBoardRow(entries,e,at,400,action,{}).box;ImGui::SetCursorScreenPos(ImVec2(at.x,box.max.y+8));}};
 auto frame=[&](unsigned held=0){imgui.io.DeltaTime=1.f/60;SetMenuInput({held,0});ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(imgui.io.DisplaySize);ImGui::Begin("Hint test",nullptr,ImGuiWindowFlags_NoDecoration);
  const auto a=menu.Draw("TEST",rows,"",{},1,{},board);ImGui::End();ImGui::Render();return a;};
 auto press=[&](unsigned held){frame();const auto a=frame(held);frame();return a;};
 // A click at x on the bar, and the first thing the menu did with it.
 const float barY=imgui.io.DisplaySize.y-28*Scale(),firstX=v2::SideMargin(imgui.io.DisplaySize.x,Scale())+10;
 auto click=[&](float x){imgui.io.AddMousePosEvent(x,barY);frame();imgui.io.AddMouseButtonEvent(0,true);MenuAction a=frame();
  imgui.io.AddMouseButtonEvent(0,false);for(int i=0;i<3;++i){const auto b=frame();if(a.kind==MenuAction::None)a=b;}return a;};
 frame();frame();
 // The Fighter hint is the bar's last, so a sweep from the right meets it first.
 float fighterX=-1;
 for(float x=imgui.io.DisplaySize.x-40;x>0&&fighterX<0;x-=8){
  const auto a=click(x);
  if(a.kind==MenuAction::Shortcut&&a.delta==static_cast<int>(MenuInput::Fighter))fighterX=x;
  else Check(a.kind==MenuAction::None,"A click right of the bar's last hint did something");
 }
 Check(fighterX>0,"The Fighter hint did not press Fighter");
 // With no dialog open the first hint is Select on the focused row: it opens the row's values.
 menu.navigation.Focus("language",rows);
 Check(click(firstX).kind==MenuAction::None&&menu.navigation.Choosing(),"The Select hint did not open the row's values");
 Check(press(MenuInput::Fighter).kind==MenuAction::None&&menu.navigation.Choosing(),"The Fighter button acted while the choice was open");
 // Nowhere on the bar does a click press a shortcut while a choice is open
 // (a click off its panel closes it, so it opens again for each).
 for(float x=imgui.io.DisplaySize.x-40;x>0;x-=8){
  if(!menu.navigation.Choosing()){menu.navigation.Focus("language",rows);press(MenuInput::Select);}
  Check(menu.navigation.Choosing(),"The choice did not open again");
  Check(click(x).kind!=MenuAction::Shortcut,"A hint pressed a shortcut while the choice was open");
 }
 // The choice's Select hint picks the option the pad has highlighted.
 menu.navigation.Focus("language",rows);press(MenuInput::Select);press(MenuInput::Right);
 Check(menu.navigation.Choosing()&&menu.navigation.ChoiceIndex()==2,"Right did not highlight the third option");
 const auto picked=click(firstX);
 Check(picked.kind==MenuAction::Chosen&&picked.id=="language"&&picked.text=="c"&&!menu.navigation.Choosing(),"The choice's Select hint did not pick its highlighted option");
 // A confirmation: a click on the bar under it answers nothing, and its own Select still does.
 menu.navigation.Focus("leave",rows);press(MenuInput::Select);frame();press(MenuInput::Right);
 Check(menu.navigation.Confirming()&&menu.navigation.ConfirmSelected(),"The confirmation did not open on its action");
 const auto under=click(firstX);
 Check(under.kind==MenuAction::None&&menu.navigation.Confirming()&&menu.navigation.ConfirmSelected(),"A click on the bar under the confirmation answered it");
 const auto confirmed=press(MenuInput::Select);
 Check(confirmed.kind==MenuAction::Activate&&confirmed.id=="leave","The confirmation's Select did not answer it");
 // Under a popup the bar's hints are labels (v2::Hint::labelOnly): the same
 // click that presses a hint presses nothing on a label.
 const auto bar=[&](bool labelOnly){
  int hit=-1;
  for(int step=0;step<4;++step){
   if(step==1)imgui.io.AddMouseButtonEvent(0,true);
   if(step==2)imgui.io.AddMouseButtonEvent(0,false);
   imgui.io.DeltaTime=1.f/60;ImGui::NewFrame();
   ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(imgui.io.DisplaySize);ImGui::Begin("Bar test",nullptr,ImGuiWindowFlags_NoDecoration);
   const int h=v2::HintBar({ImVec2(0,imgui.io.DisplaySize.y-56*Scale()),imgui.io.DisplaySize},{{MenuSelectGlyph(),"Select",labelOnly}},"",0);
   if(h>=0)hit=h;
   ImGui::End();ImGui::Render();
  }
  return hit;
 };
 imgui.io.AddMousePosEvent(firstX,barY);
 Check(bar(false)==0,"A click on a hint did not press it");
 Check(bar(true)<0,"A click on a hint drawn as a label pressed it");
}
// A held direction repeats in a notice and on the on-screen keyboard as it
// does in a list and a choice (DirectionRepeat): once at once, then on.
void DialogRepeat(){
 HeadlessImGui imgui;GameMenu menu;SetMenuGlyphs(3,0x40000,0x20000);
 std::vector<MenuEntry> rows={TextRow("name","Name","",31)};
 auto frame=[&](unsigned held=0){imgui.io.DeltaTime=1.f/60;SetMenuInput({held,0});ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(imgui.io.DisplaySize);ImGui::Begin("Repeat test",nullptr,ImGuiWindowFlags_NoDecoration);
  const auto a=menu.Draw("TEST",rows);ImGui::End();ImGui::Render();return a;};
 auto press=[&](unsigned held){frame();const auto a=frame(held);frame();return a;};
 // Over half a second is the first step, one after the delay and more after it: past a third button.
 const auto hold=[&](unsigned held){for(int i=0;i<40;++i)frame(held);frame();};
 frame();frame();
 menu.ShowNotice(NoticeDialog{"Three","Heading",{{"First"},{"Second",NoticeResult::CreatePublicRoom},{"Third",NoticeResult::OpenButtonLayout}}});frame();frame();
 hold(MenuInput::Right);press(MenuInput::Select);
 Check(menu.TakeNoticeResult()==NoticeResult::OpenButtonLayout,"Holding Right in a notice stepped once");
 frame();frame();
 // The on-screen keyboard opens on q; holding Right walks past w.
 press(MenuInput::Select);frame();
 Check(menu.navigation.Editing(),"Select did not open the editor");
 hold(MenuInput::Right);press(MenuInput::Select);
 const std::string typed=menu.navigation.Draft();
 Check(typed.size()==1&&typed!="q"&&typed!="w","Holding Right on the on-screen keyboard stepped once");
 press(MenuInput::Back);Check(!menu.navigation.Editing(),"Back did not close the editor");
}
