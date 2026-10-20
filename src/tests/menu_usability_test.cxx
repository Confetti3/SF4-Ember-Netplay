#include "shell_journey_support.hxx"
#include "../ui/RoomBoardNavigation.hxx"
#include <queue>
#include "../netplay/UnseatedInputDelay.hxx"

int main() try {
 using namespace sf4e;
 // Every rendered action is reachable by the directions on every pad, at
 // narrow, two-column and three-column widths. Down follows the visual grid.
 for(std::size_t columns : {1u,2u,3u}) {
  std::vector<MenuEntry> rows;
  for(int i=0;i<4;++i)rows.push_back(Row("table-"+std::to_string(i),"Table",""));
  for(int i=0;i<7;++i)rows.push_back(Row("member-"+std::to_string(i),"Member",""));
  for(int i=0;i<13;++i)rows.push_back(Row("action-"+std::to_string(i),"Action",""));
  rows.push_back(Row("inline-chat","Message",""));
  LinkRoomBoard(rows,columns);
  std::set<std::string> visited;std::queue<std::string> pending;pending.push(rows.front().id);
  while(!pending.empty()) {
   const auto id=pending.front();pending.pop();if(!visited.insert(id).second)continue;
   for(unsigned direction : {MenuInput::Up,MenuInput::Down,MenuInput::Left,MenuInput::Right}) {
    MenuNavigation nav;nav.Focus(id,rows);nav.Update({},rows);nav.Update({direction,1},rows);pending.push(nav.Focus());
   }
  }
  Check(visited.size()==rows.size(),"A room item cannot be reached with directions");
  if(columns>1) {
   MenuNavigation nav;nav.Focus("action-0",rows);nav.Update({},rows);nav.Update({MenuInput::Down,1},rows);
   Check(nav.Focus()=="action-"+std::to_string(columns),"Down does not follow the drawn toolbar column");
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
 for(const char* id:{"room-invite","settings","room-chat","room-members","leave","inline-chat"}) {
  find(id);h.FocusOn(id);
 }
 // Every visible table action is selected using real directional input, not a synthetic leave shortcut.
 for(const auto* duplicate:{"leave-place","selection","options","player","room-admin","copy","copy-short"})
  Check(std::none_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==duplicate;}),"A duplicate toolbar settings/invitation route remains");
 auto& table=h.view.room.tables[2];
 auto count=h.actions.size();h.FocusOn("table-2");h.Frame();h.Frame(MenuInput::Select);
 Check(h.shell.Navigation().Choosing()&&h.actions.size()==count,"Opening our table readied instead of opening actions");
 h.Frame(MenuInput::Select,30);h.Frame();
 Check(h.actions.size()==count,"Holding Confirm through the chooser readied us");
 h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==count+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue,"Controller could not select Leave directly on its table");
 // Ready is a deliberate second press. Back from the chooser never leaves.
 count=h.actions.size();h.Choose("table-2");h.Press(MenuInput::Back);
 Check(h.actions.size()==count&&h.shell.Navigation().Screen()=="room","Cancelling table actions changed our place");
 h.Choose("table-2");h.Press(MenuInput::Select);
 Check(h.actions.size()==count+1&&h.actions.back().command.kind==Kind::Ready,"Explicit Ready choice failed");
 // Seated players reach Fighter through the table, without a duplicate toolbar entry.
 FighterSelector selector;selection::Pick pick;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};
 h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.shell.Navigation().Screen()=="selection","The table's Fighter choice did not open selection");
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="room","Fighter Back did not return to the room");
 // Ready may be unavailable with no opponent, but Leave must remain accessible.
 table.p2=0;h.Frame();count=h.actions.size();h.Choose("table-2");
 Check(!find("table-2").choices[0].enabled&&find("table-2").choices[2].enabled,"No opponent disabled the entire table");
 h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==count+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue,"No-opponent table trapped the controller");
 table.p2=2;table.score[0]=1;h.Frame();count=h.actions.size();
 h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.shell.Navigation().Choosing()&&h.shell.Navigation().ChoiceIndex()==0&&h.actions.size()==count,"Scored table Leave skipped the confirmation");
 h.Press(MenuInput::Back);Check(h.actions.size()==count,"Cancelling scored departure sent a command");
 table.score[0]=0;table.phase=room::TablePhase::Ready;table.ready[0]=table.ready[1]=true;table.spectatorHold=true;h.Frame();
 h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==count+1&&h.actions.back().roomAction.kind==room::ActionKind::Unready,"Cancel start left the seat or failed");
 table.phase=room::TablePhase::Waiting;table.spectatorHold=false;table.ready[0]=table.ready[1]=false;h.Frame();
 // A changed action meaning closes the chooser; an old Cancel start cannot turn into Leave.
 table.phase=room::TablePhase::Ready;table.spectatorHold=true;table.ready[0]=table.ready[1]=true;h.Frame();
 count=h.actions.size();h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Right);
 table.phase=room::TablePhase::Waiting;table.spectatorHold=false;table.ready[0]=table.ready[1]=false;h.Frame(MenuInput::Select);
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==count,"A stale Cancel start choice left the seat");h.Frame();
 // Expanded table actions remain visible without Y, including unresolved-result recovery.
 table.phase=room::TablePhase::Paused;h.Frame();count=h.actions.size();h.Choose("table-2");
 for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.shell.Navigation().Screen()=="room-table","Expanded table actions need a shortcut");
 h.Choose("abandon-result");Check(h.shell.Navigation().Confirming()&&h.actions.size()==count,"Unresolved recovery skipped its confirmation");
 h.Press(MenuInput::Back);h.Press(MenuInput::Back);table.phase=room::TablePhase::Waiting;h.Frame();
 // Chat remains unread behind table choices and settings; attending the composer reads it.
 h.Choose("table-2");h.view.room.chat.push_back({1,2,"During table actions"});h.Frame(0,3);
 Check(h.shell.UnreadChat()==1,"Table actions marked unseen chat as read");h.Press(MenuInput::Back);
 h.Choose("settings");const auto unreadInSettings=h.shell.UnreadChat();h.view.room.chat.push_back({2,2,"During settings"});h.Frame(0,3);
 Check(h.shell.UnreadChat()==unreadInSettings+1,"Settings cleared unread chat");
 Check(!find("room-admin").enabled&&find("room-rules").enabled,"Guest permissions changed at the settings entry");
 h.Choose("room-match");Check(h.shell.Navigation().Screen()=="room-match","Match settings are unreachable");
 find("input-delay");find("check-connection");h.Press(MenuInput::Back);
 h.Choose("room-rules");find("rules");
 h.FocusOn("rules-table");const auto firstTable=find("rules-table").value;h.Press(MenuInput::Left);
 Check(find("rules-table").value!=firstTable,"Guest could not inspect another table's rules");h.Press(MenuInput::Right);
 Check(find("rules-table").value==firstTable,"Rules table selector did not return to our table");
 h.Press(MenuInput::Back);h.Press(MenuInput::Back);
 h.FocusOn("inline-chat");h.Frame(0,4);Check(h.shell.UnreadChat()==0,"Attending inline chat did not clear unread state");
 // Opposing directions do not move sideways, including after repeat begins.
 h.FocusOn("table-2");h.Press(MenuInput::Up|MenuInput::Down);h.Frame(MenuInput::Up|MenuInput::Down,45);h.Frame();
 Check(h.shell.Navigation().Focus()=="table-2","Opposing vertical inputs moved focus");
 h.Press(MenuInput::Left|MenuInput::Right);Check(h.shell.Navigation().Focus()=="table-2","Opposing horizontal inputs moved focus");
 // Back from our table has exactly the same navigation meaning as anywhere else.
 h.FocusOn("table-2");auto before=h.actions.size();h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="home"&&h.actions.size()==before,"Back changed the player's seat");
 h.Screen("room");h.FocusOn("table-0");
 const auto chooseLeave=[&]{h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);};
 chooseLeave();
 Check(h.actions.size()==before+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&
  h.actions.back().roomAction.table==2,"Table Leave acted on a previously highlighted table instead of our place");
 h.view.room.tables[2].score[0]=1;before=h.actions.size();chooseLeave();
 Check(h.shell.Navigation().Choosing()&&h.shell.Navigation().ChoiceIndex()==0,"Leaving a score did not ask with Stay selected");
 h.Press(MenuInput::Back);Check(h.actions.size()==before,"Cancelling Leave sent a command");
 h.view.room.tables[2].score[0]=0;h.view.room.tables[2].phase=room::TablePhase::Playing;h.Frame();
 Check(!find("table-2").choices[2].enabled&&!find("table-2").choices[2].detail.empty(),"Blocked Leave lacks a visible reason");
 chooseLeave();Check(h.actions.size()==before,"Blocked Leave sent a command");h.Press(MenuInput::Back);
 h.view.room.tables[2].phase=room::TablePhase::Waiting;h.view.room.members[0].seat=-1;
 h.view.room.tables[2].p1=0;h.view.room.tables[2].queue={1};h.Frame();
 Check(find("table-2").choices[1].label==loc::T("room.leave_queue"),"Queue has no named Leave action");
 Check(std::none_of(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="leave-place";}),"Leave queue is duplicated in the toolbar");
 find("selection"); // A queued player has no Fighter choice on the table.
 before=h.actions.size();h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==before+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&h.actions.back().roomAction.table==2,"Table chooser could not leave the queue");
 // The standalone fighter entry remains usable before taking a seat.
 h.view.room.tables[2].queue.clear();h.view.room.members[0].table=-1;h.Frame();
 h.Choose("selection");Check(h.shell.Navigation().Screen()=="selection","An unseated player lost fighter selection");
 h.Screen("room");h.view.room.tables[2].queue={1};h.view.room.members[0].table=2;h.Frame();
 // Host rules remain explicit, table-scoped and protected against losing an unsaved draft.
 h.view.room.host=1;h.Screen("room");h.Choose("settings");h.Choose("room-rules");
 h.FocusOn("rules-table");h.Press(MenuInput::Left);Check(find("rules-table").value=="2","Host could not select table 2");
 h.FocusOn("rounds");h.Press(MenuInput::Right);
 Check(find("apply-rules").enabled&&!find("rules-table").enabled,"A pending rules draft can change tables or cannot apply");
 h.FocusOn("rules-table");h.Press(MenuInput::Right);Check(find("rules-table").value=="2","Rules draft switched tables");
 h.Choose("apply-rules");Check(h.actions.back().roomAction.kind==room::ActionKind::SetRules&&h.actions.back().roomAction.table==1,"Rules were applied to the wrong table");
 h.Screen("room");
 // Settings and controller pages are visible; Back returns to the room.
 h.Choose("settings");h.Choose("player");Check(h.shell.Navigation().Screen()=="player","Controller settings not reachable");
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="settings","Controller Back lost its caller");
 h.Choose("interface");h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Options shortcut not available");
 h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="interface","Shortcut erased the settings return path");
 // Delay is reachable through Settings without a seat, including while watching.
 h.view.canEditPreferences=false;h.view.session.match=netplay::MatchState::None;
 h.view.room.members[0].table=-1;h.view.room.members[0].seat=-1;
 h.view.room.tables[2].p1=0;h.view.preferences.autoInputDelay=false;h.view.preferences.inputDelay=2;
 h.Screen("room");h.Choose("settings");
 Check(find("room-match").enabled&&find("room-match").label=="Input delay","Input delay is hidden behind a seat");
 h.Choose("room-match");Check(find("input-delay").enabled,"Unseated delay is disabled");
 h.FocusOn("input-delay");count=h.actions.size();h.Press(MenuInput::Right);
 Check(h.actions.size()==count+1&&h.actions.back().command.kind==Kind::SaveInputDelay&&h.actions.back().selectedDelay==3,"Unseated adjustment did not save a personal preference");
 h.view.preferences.inputDelay=3;h.Frame();Check(find("input-delay").value=="3 frames","Saved delay did not reach the editor");
 h.view.room.tables[2].queue.push_back(1);h.view.session.match=netplay::MatchState::Playing;h.Frame();
 count=h.actions.size();h.Press(MenuInput::Left);
 Check(h.actions.size()==count+1&&h.actions.back().command.kind==Kind::SaveInputDelay&&h.actions.back().selectedDelay==2,"Queued spectator cannot save future delay");
 Check(!find("connection-seat").enabled||find("connection-seat").info,"Spectator was offered an opponent connection probe");
 h.view.preferences.inputDelay=0;h.Frame();h.Press(MenuInput::Left);
 Check(h.actions.back().selectedDelay==AutoInputDelayChoice,"Unseated player cannot choose Auto");
 h.view.preferences.autoInputDelay=true;h.Frame();h.Press(MenuInput::Right);
 Check(h.actions.back().selectedDelay==0,"Unseated player cannot leave Auto");
 // A seat granted while the page is open immediately restores match locking.
 h.view.room.tables[2].queue.clear();h.view.room.members[0].table=2;h.view.room.members[0].seat=0;
 h.view.room.tables[2].p1=1;h.view.room.tables[2].phase=room::TablePhase::Playing;h.view.delayLocked=true;
 h.Frame();Check(!find("input-delay").enabled,"A playing fighter could edit delay");
 count=h.actions.size();h.FocusOn("input-delay");h.Press(MenuInput::Right);
 Check(h.actions.size()==count,"Locked fighter emitted a delay change");
 h.view.session.match=netplay::MatchState::None;h.view.room.tables[2].phase=room::TablePhase::Waiting;h.view.delayLocked=false;
 h.view.selectedDelay=2;h.view.preferences.autoInputDelay=false;h.Frame();
 h.Press(MenuInput::Right);Check(h.actions.back().command.kind==Kind::ApplyDelay,"Unready fighter stopped using the match delay path");
 // The table shortcut opens this same editor, with Back returning to table actions.
 h.Screen("room-table");h.Choose("room-match");h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="room-table","Delay shortcut lost the table return path");
 // These are the same eligibility and value rules the runtime uses after dequeue.
 Check(!netplay::CanSaveUnseatedInputDelay(h.view.session,h.view.room),"A newly seated member passed the personal preference gate");
 h.view.room.members[0].table=-1;h.view.room.members[0].seat=-1;
 Check(netplay::CanSaveUnseatedInputDelay(h.view.session,h.view.room),"Unseated member failed the preference gate");
 h.view.session.readyPending=true;
 Check(!netplay::CanSaveUnseatedInputDelay(h.view.session,h.view.room),"A pending Ready passed the preference gate");h.view.session.readyPending=false;
 h.view.room.closed=true;Check(!netplay::CanSaveUnseatedInputDelay(h.view.session,h.view.room),"Closed room passed the preference gate");h.view.room.closed=false;
 h.view.room.members.clear();Check(!netplay::CanSaveUnseatedInputDelay(h.view.session,h.view.room),"Missing local member passed the preference gate");
 netplay::PlayerPreferences chosen;chosen.displayName="Keep me";chosen.inputDelay=3;
 Check(netplay::ChooseInputDelay(chosen,0)&&!chosen.autoInputDelay&&chosen.inputDelay==0,"Manual zero was not saved");
 Check(netplay::ChooseInputDelay(chosen,AutoInputDelayChoice)&&chosen.autoInputDelay&&chosen.inputDelay==0,"Auto discarded the manual preference");
 Check(!netplay::ChooseInputDelay(chosen,-1)&&chosen.autoInputDelay&&chosen.inputDelay==0,"An unseated recommendation changed the preference");
 Check(!netplay::ChooseInputDelay(chosen,MaximumInputDelay+1)&&chosen.autoInputDelay&&chosen.inputDelay==0,"Invalid delay changed the preference");
 Check(chosen.displayName=="Keep me","Delay changed an unrelated profile field");
 SetMenuEntriesProbe({});
 std::cout<<"Room actions, directional navigation and return paths passed.\n";
 return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
