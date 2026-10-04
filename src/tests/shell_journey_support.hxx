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
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace sf4e::ui;
using Button=ControllerSample;
using Kind=sf4e::netplay::CommandKind;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
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
  // A grid's cells sit side by side, so Up and Down reach only some of them:
  // sweep each row sideways from the top.
  if(!at()){
   walk(MenuInput::Up);
   for(int row=0;row<100&&!at();++row){
    for(int i=0;i<8&&!at();++i)Press(MenuInput::Left);
    for(int i=0;i<8&&!at();++i)Press(MenuInput::Right);
    if(at())break;
    const std::string before=shell.Navigation().Focus();
    Press(MenuInput::Down);
    if(shell.Navigation().Focus()==before)break;
   }
  }
  if(!at())throw std::runtime_error(std::string("Journey item not reachable: ")+id+" on "+shell.Navigation().Screen()+" focused "+shell.Navigation().Focus());
 }
 void Choose(const char* id){FocusOn(id);Press(MenuInput::Select);}
 void Screen(const char* id){shell.Navigation().Home();if(std::string(id)!="home")shell.Navigation().Push(id);Frame();}
};
}
