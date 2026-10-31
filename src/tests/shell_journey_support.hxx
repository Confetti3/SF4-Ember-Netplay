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
using Button=ControllerSample;
using Kind=sf4e::netplay::CommandKind;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
// The fighter drawn right of `fighter` in USFIV's select order, which is where Right goes.
int DisplayedAfter(int fighter){const int* at=std::find(std::begin(sf4e::selection::RosterDisplayOrder),std::end(sf4e::selection::RosterDisplayOrder),fighter);return at[1];}
struct Harness {
 ApplicationShell shell;ShellView view;std::vector<ShellAction> actions;bool open=true,accept=true;
 std::function<void()> selection=[]{},developer;
 HeadlessImGui imgui; // last: created after the shell and destroyed before it
 Harness(){
  view.canEditPreferences=view.canOpenRoom=view.helperReady=view.controllerReady=view.canChangeController=true;
  // The matches told are kept in memory, so a journey never reads or writes this PC's own file.
  shell.SetAnnouncedStore([]{return std::vector<sf4e::platform::AnnouncedMatch>{};},[](const std::vector<sf4e::platform::AnnouncedMatch>&){return true;});
 }
 void Frame(unsigned buttons=0,int count=1,float seconds=1.f/60){
  for(int i=0;i<count;++i){auto& io=ImGui::GetIO();io.DeltaTime=seconds;SetMenuInput({buttons,0});ImGui::NewFrame();
   shell.Draw(view,&open,[&](ShellAction a){actions.push_back(a);return accept;},selection,developer);
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
    Press(direction);
    if(at())break;
    const std::string now=shell.Navigation().Focus();
    if(std::find(reached.begin(),reached.end(),now)==reached.end())reached.push_back(now);
   }
  if(!at()){std::string all;for(const auto& r:reached)all+=" "+r;throw std::runtime_error(std::string("Journey item not reachable: ")+id+" on "+shell.Navigation().Screen()+" focused "+shell.Navigation().Focus()+"; reached:"+all);}
 }
 void Choose(const char* id){FocusOn(id);Press(MenuInput::Select);}
 void Screen(const char* id){shell.Navigation().Home();if(std::string(id)!="home")shell.Navigation().Push(id);Frame();}
};
// A room this player (member 1) has joined, with its tables numbered, and no
// more: the generation, the control's health, the host, the members and their
// seats are each journey's own, set where it joins.
void JoinedRoom(Harness& h,std::uint64_t epoch){
 h.view.session.room=sf4e::netplay::RoomState::Joined;h.view.room.roomEpoch=epoch;h.view.room.localMember=1;
 for(std::size_t i=0;i<h.view.room.tables.size();++i)h.view.room.tables[i].id=static_cast<std::uint8_t>(i);
}
}
