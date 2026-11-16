#pragma once
// The harness the shell journey suites share: a headless ImGui frame loop
// around ApplicationShell that records what the shell submits.
#include "../ui/ApplicationShell.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "../ui/Theme.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../ui/MenuGlyphs.hxx"
#include "../ui/MenuPresentation.hxx"
#include "../ui/RecoveryMenu.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "imgui_test_support.hxx"
#include "shell_action_support.hxx"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace sf4e::ui;
using namespace sf4e::test;
namespace commands=sf4e::commands;
using Button=ControllerSample;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
// Whether the menu's own window of choices (GameMenu::DrawValuePanel: a value panel or a popout) was drawn in the frame just run.
bool ChoicesPanelDrawn(){const ImGuiWindow* window=ImGui::FindWindowByName("###Values");return window&&(window->Active||window->WasActive);}
// The fighter drawn right of `fighter` in USFIV's select order, which is where Right goes.
int DisplayedAfter(int fighter){const int* at=std::find(std::begin(sf4e::selection::RosterDisplayOrder),std::end(sf4e::selection::RosterDisplayOrder),fighter);return at[1];}
struct Harness {
 ApplicationShell shell;ShellView view;std::vector<ShellAction> actions;bool open=true,accept=true;
 // What the prompt over the WIN sent of the player's answer: nothing, now the runtime says it (kept to show the shell does not).
 std::vector<sf4e::room::Action> said;
 std::function<void()> selection=[]{},developer;
 HeadlessImGui imgui; // last: created after the shell and destroyed before it
 Harness(){
  view.canEditPreferences=view.canOpenRoom=view.helperReady=view.controllerReady=view.canChangeController=true;
  // The matches told are kept in memory, so a journey never reads or writes this PC's own file.
  shell.SetAnnouncedStore([]{return std::vector<sf4e::platform::AnnouncedMatch>{};},[](const std::vector<sf4e::platform::AnnouncedMatch>&){return true;});
 }
 void Frame(unsigned buttons=0,int count=1,float seconds=1.f/60){
  for(int i=0;i<count;++i){imgui.Use();auto& io=ImGui::GetIO();io.DeltaTime=seconds;SetMenuInput({buttons,0});ImGui::NewFrame();
   shell.Draw(view,&open,[&](ShellAction a){
    if(const auto* r=std::get_if<commands::RoomAction>(&a);r&&r->roomAction.kind==sf4e::room::ActionKind::PostMatchAnswer){said.push_back(r->roomAction);return accept;}
    actions.push_back(a);return accept;},selection,developer);
   ImGui::Render();}
 }
 // One frame that lets `seconds` of interface time pass (the shell's clock is ImGui's).
 void Wait(float seconds){Frame(0,1,seconds);}
 void Press(unsigned b){Frame();Frame(b);Frame();}
 void FocusOn(const char* id){
  const auto at=[&]{return shell.Navigation().Focus()==id;};
  // Up to the top, then down; each stops where focus stops moving.
  const auto walk=[&](unsigned direction){
   for(int i=0;i<100&&!at();++i){const std::string before=shell.Navigation().Focus();Press(direction);if(shell.Navigation().Focus()==before)break;}
  };
  Frame();walk(MenuInput::Up);walk(MenuInput::Down);
  // A grid's cells sit side by side and a board moves by where its rows are
  // drawn, so Up and Down reach only some of them: from each row reached so
  // far, try each direction, until the item is reached or no new row is.
  // Every hop is a press; returning to a row already reached is not.
  std::vector<std::string> reached{shell.Navigation().Focus()};
  for(std::size_t i=0;i<reached.size()&&i<400&&!at();++i)
   for(unsigned direction:{MenuInput::Up,MenuInput::Down,MenuInput::Left,MenuInput::Right}){
    shell.Navigation().Prefer(reached[i]);Frame();
    std::string lit=shell.Navigation().Lit(reached[i]);
    Press(direction);
    // A table card lights its places one by one before Left and Right leave
    // it: a press that only moved the card's light is pressed again.
    for(int k=0;k<4&&!at()&&reached[i].compare(0,6,"table-")==0&&shell.Navigation().Focus()==reached[i]&&shell.Navigation().Lit(reached[i])!=lit;++k){
     lit=shell.Navigation().Lit(reached[i]);Press(direction);
    }
    if(at())break;
    const std::string now=shell.Navigation().Focus();
    if(std::find(reached.begin(),reached.end(),now)==reached.end())reached.push_back(now);
   }
  if(!at()){std::string all;for(const auto& r:reached)all+=" "+r;throw std::runtime_error(std::string("Journey item not reachable: ")+id+" on "+shell.Navigation().Screen()+" focused "+shell.Navigation().Focus()+"; reached:"+all);}
 }
 void Choose(const char* id){FocusOn(id);Press(MenuInput::Select);}
 // A choice of a popout row, as a player makes it: open the row, step down to the choice, select it.
 void Pick(const char* row,const char* choice){
  std::vector<MenuEntry> shown;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){shown=r;});Frame();SetMenuEntriesProbe({});
  const auto entry=std::find_if(shown.begin(),shown.end(),[&](const MenuEntry& e){return e.id==row;});
  if(entry==shown.end())throw std::runtime_error(std::string("Journey popout not offered: ")+row);
  std::size_t at=0;while(at<entry->choices.size()&&entry->choices[at].id!=choice)++at;
  if(at==entry->choices.size())throw std::runtime_error(std::string("Journey choice not offered: ")+choice+" by "+row);
  Choose(row);for(std::size_t i=0;i<at;++i)Press(MenuInput::Down);Press(MenuInput::Select);
 }
 // A click on the focused row's pane button (PaneButton.hxx), after a frame to read where it is drawn. Says whether it
 // was drawn and the reason it gave under itself, if any.
 struct PaneClick{bool drawn=false;std::string reason;};
 PaneClick ClickPaneButton(){
  PaneClick out;ImVec2 min,max;
  SetMenuCardProbe([&](const char* id,ImVec2 a,ImVec2 b){if(std::strcmp(id,"pane-choice")==0){out.drawn=true;min=a;max=b;}});
  SetMenuCardTextProbe([&](const std::string& id,const std::string& text){if(id=="pane-choice")out.reason=text;});
  Frame();SetMenuCardProbe({});SetMenuCardTextProbe({});
  if(!out.drawn)return out;
  auto& io=ImGui::GetIO();io.AddMousePosEvent((min.x+max.x)*.5f,(min.y+max.y)*.5f);Frame();
  io.AddMouseButtonEvent(0,true);Frame();io.AddMouseButtonEvent(0,false);Frame(0,2);
  io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);Frame();
  return out;
 }
 void Screen(const char* id){shell.Navigation().Home();if(std::string(id)!="home")shell.Navigation().Push(id);Frame();}
};
// A room this player (member 1) has joined, with its tables numbered, and no
// more: the generation, the control's health, the host, the members and their
// seats are each journey's own, set where it joins. As in the runtime
// (ComputeGates), a joined room locks the player's settings but the ones it
// leaves open (PlayerPreferences::CopyRoomSettings).
void JoinedRoom(Harness& h,std::uint64_t epoch){
 h.view.session.room=sf4e::netplay::RoomState::Joined;h.view.room.roomEpoch=epoch;h.view.room.localMember=1;
 h.view.canEditPreferences=false;
 for(std::size_t i=0;i<h.view.room.tables.size();++i)h.view.room.tables[i].id=static_cast<std::uint8_t>(i);
}
}
