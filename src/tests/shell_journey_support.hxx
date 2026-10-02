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
 }
 void Frame(unsigned buttons=0,int count=1){
  for(int i=0;i<count;++i){auto& io=ImGui::GetIO();io.DeltaTime=1.f/60;SetMenuInput({buttons,0});ImGui::NewFrame();
   shell.Draw(view,&open,[&](ShellAction a){actions.push_back(a);return accept;},selection,developer);
   ImGui::Render();}
 }
 void Press(unsigned b){Frame();Frame(b);Frame();}
 void FocusOn(const char* id){
  Frame();for(int i=0;i<100&&shell.Navigation().Focus()!=id;++i)Press(MenuInput::Up);
  for(int i=0;i<100&&shell.Navigation().Focus()!=id;++i)Press(MenuInput::Down);
  if(shell.Navigation().Focus()!=id)throw std::runtime_error(std::string("Journey item not reachable: ")+id+" on "+shell.Navigation().Screen()+" focused "+shell.Navigation().Focus());
 }
 void Choose(const char* id){FocusOn(id);Press(MenuInput::Select);}
 void Screen(const char* id){shell.Navigation().Home();if(std::string(id)!="home")shell.Navigation().Push(id);Frame();}
};
}
