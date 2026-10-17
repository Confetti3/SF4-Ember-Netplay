#include "shell_journey_support.hxx"
#include "../ui/RoomBoardNavigation.hxx"
#include <queue>

int main() try {
 using namespace sf4e;
 // Every rendered action is reachable by the directions on every pad, at
 // narrow, two-column and three-column widths. Down follows the visual grid.
 for(float width : {600.f, 950.f, 1500.f}) {
  std::vector<MenuEntry> rows;
  for(int i=0;i<4;++i)rows.push_back(Row("table-"+std::to_string(i),"Table",""));
  for(int i=0;i<7;++i)rows.push_back(Row("member-"+std::to_string(i),"Member",""));
  for(int i=0;i<13;++i)rows.push_back(Row("action-"+std::to_string(i),"Action",""));
  LinkRoomBoard(rows,width);
  std::set<std::string> visited;std::queue<std::string> pending;pending.push(rows.front().id);
  while(!pending.empty()) {
   const auto id=pending.front();pending.pop();if(!visited.insert(id).second)continue;
   for(unsigned direction : {MenuInput::Up,MenuInput::Down,MenuInput::Left,MenuInput::Right}) {
    MenuNavigation nav;nav.Focus(id,rows);nav.Update({},rows);nav.Update({direction,1},rows);pending.push(nav.Focus());
   }
  }
  Check(visited.size()==rows.size(),"A room item cannot be reached with directions");
  if(width>=820) {
   MenuNavigation nav;nav.Focus("action-0",rows);nav.Update({},rows);nav.Update({MenuInput::Down,1},rows);
   Check(nav.Focus()=="action-"+std::to_string(RoomActionColumns(width)),"Down does not follow the drawn toolbar column");
  }
 }
 Harness h;h.Frame();JoinedRoom(h,12);
 h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;
 h.view.room.host=2;h.view.room.name="Menu review";
 room::Member local;local.id=1;local.name="You";local.table=2;local.seat=0;
 room::Member peer;peer.id=2;peer.name="Host";peer.table=2;peer.seat=1;
 h.view.room.members={local,peer};h.view.room.tables[2].p1=1;h.view.room.tables[2].p2=2;
 h.view.room.tables[2].phase=room::TablePhase::Waiting;
 h.view.canEditSelection=true;h.view.canReady=true;
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const auto& entries){rows=entries;});
 const auto find=[&](const char* id)->const MenuEntry& {
  const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  if(it==rows.end())throw std::runtime_error(std::string("Missing visible action: ")+id);return *it;
 };
 h.Screen("room");
 for(const char* id:{"leave-place","selection","options","player","settings","room-chat","room-members","leave"}) {
  find(id);h.FocusOn(id);
 }
 // Back from our table has exactly the same navigation meaning as anywhere else.
 h.FocusOn("table-2");auto before=h.actions.size();h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="home"&&h.actions.size()==before,"Back changed the player's seat");
 h.Screen("room");h.FocusOn("table-0");h.Choose("leave-place");
 Check(h.actions.size()==before+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&
  h.actions.back().roomAction.table==2,"Visible Leave acted on the highlighted table instead of our place");
 h.view.room.tables[2].score[0]=1;before=h.actions.size();h.Choose("leave-place");
 Check(h.shell.Navigation().Choosing()&&h.shell.Navigation().ChoiceIndex()==0,"Leaving a score did not ask with Stay selected");
 h.Press(MenuInput::Back);Check(h.actions.size()==before,"Cancelling Leave sent a command");
 h.view.room.tables[2].score[0]=0;h.view.room.tables[2].phase=room::TablePhase::Playing;h.Frame();
 Check(!find("leave-place").enabled&&!find("leave-place").detail.empty(),"Blocked Leave lacks a visible reason");
 h.Choose("leave-place");Check(h.actions.size()==before,"Blocked Leave sent a command");
 h.view.room.tables[2].phase=room::TablePhase::Waiting;h.view.room.members[0].seat=-1;
 h.view.room.tables[2].p1=0;h.view.room.tables[2].queue={1};h.Frame();
 Check(find("leave-place").label==loc::T("room.leave_queue"),"Queue has no named Leave action");
 h.Choose("leave-place");Check(h.actions.size()==before+1,"Visible Leave queue did not act");
 // Settings and controller pages are visible; Back returns to the room.
 h.Choose("player");Check(h.shell.Navigation().Screen()=="player","Controller settings not reachable");
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="room","Controller Back lost its caller");
 h.Choose("settings");h.Choose("interface");h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Options shortcut not available");
 h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="interface","Shortcut erased the settings return path");
 SetMenuEntriesProbe({});
 std::cout<<"Room actions, directional navigation and return paths passed.\n";
 return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
