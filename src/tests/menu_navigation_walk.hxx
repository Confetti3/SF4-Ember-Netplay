#pragma once
#include "../ui/MenuNavigation.hxx"
#include <queue>
#include <set>
// Explore the actual directional transitions. step draws the current layout
// or updates a pure navigation model; restoring a node never activates a row.
template<class Step,class Reached>
bool WalkMenuNavigation(sf4e::ui::MenuNavigation& nav,Step step,Reached reached) {
 using namespace sf4e::ui;
 std::set<std::string> seen;std::queue<MenuNavigation> pending;pending.push(nav);
 while(!pending.empty()){
  const auto from=pending.front();pending.pop();
  if(!seen.insert(from.Focus()).second)continue;
  for(unsigned direction:{MenuInput::Up,MenuInput::Down,MenuInput::Left,MenuInput::Right}){
   nav=from;step(0);if(reached())return true;
   step(direction);if(reached())return true;
   if(!seen.count(nav.Focus()))pending.push(nav);
  }
 }
 return false;
}
