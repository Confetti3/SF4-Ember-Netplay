// Follow the room from the shell (session/RoomFollow.hxx): the board's action
// bar and a table's options offer it, each move goes out as the room's
// ordinary Watch, one press stops it, and a seat ends it.
#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include "../common/Localization.hxx"
#include "../ui/MenuProbes.hxx"
#include <string>

void FollowJourneys() {
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,23);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=2;h.view.room.revision=3;
 room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
 for(room::MemberId id=2;id<=5;++id){
  room::Member fighter;fighter.id=id;fighter.name="Fighter "+std::to_string(id);
  fighter.table=static_cast<std::int8_t>(id<4?2:3);fighter.seat=static_cast<std::int8_t>(id%2);h.view.room.members.push_back(fighter);
 }
 // Table 0's pair stood up while the player watched it; table 2 plays.
 auto& tables=h.view.room.tables;
 tables[2].p1=2;tables[2].p2=3;tables[2].phase=room::TablePhase::Playing;tables[2].matchGeneration=4;
 tables[0].spectators={1};
 std::vector<MenuEntry> rows;
 const auto capture=[&]{SetMenuEntriesProbe([&](const std::vector<MenuEntry>& seen){rows=seen;});h.Frame();SetMenuEntriesProbe({});};
 const auto row=[&](const char* id)->const MenuEntry*{
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  return found==rows.end()?nullptr:&*found;
 };
 const auto watches=[&](std::size_t from){
  std::vector<int> tablesWatched;
  for(auto i=from;i<h.actions.size();++i)
   if(h.actions[i].command.kind==Kind::RoomAction&&h.actions[i].roomAction.kind==room::ActionKind::Watch)tablesWatched.push_back(h.actions[i].roomAction.table);
  return tablesWatched;
 };
 h.Screen("room");capture();
 Check(row("follow-room")&&row("follow-room")->label==loc::T("room.follow")&&row("follow-room")->enabled,"A watcher's board did not offer Follow the room");
 // A lock-in holds one table, so it is released first.
 h.view.room.members[0].spectatorLocked=true;capture();
 Check(row("follow-room")&&!row("follow-room")->enabled&&row("follow-room")->detail==loc::T("room.follow.locked_in"),"Follow the room was open to a locked-in watcher");
 h.view.room.members[0].spectatorLocked=false;
 // Following moves the watcher from the idle table to the game: one Watch, no Unwatch.
 auto from=h.actions.size();
 h.Choose("follow-room");h.Frame(0,3);
 Check(watches(from)==std::vector<int>{2},"Following did not move the watcher to the table playing");
 Check(std::none_of(h.actions.begin()+static_cast<std::ptrdiff_t>(from),h.actions.end(),[](const ShellAction& a){
  return a.roomAction.kind==room::ActionKind::Unwatch||a.roomAction.kind==room::ActionKind::LockSpectating;}),"Following sent more than a Watch");
 tables[0].spectators.clear();tables[2].watchingNext={1};
 from=h.actions.size();h.Frame(0,10);capture();
 Check(watches(from).empty(),"Following moved again while the game it waits for plays");
 Check(row("follow-room")->label==loc::T("room.follow.stop")&&row("follow-room")->detail==loc::Tf("room.follow.stop_detail",3),
  "The board did not say which table is followed");
 // One press stops it: the table stopping no longer moves the watcher.
 h.Choose("follow-room");capture();
 Check(row("follow-room")->label==loc::T("room.follow"),"Stop following did not stop");
 tables[2].phase=room::TablePhase::Waiting;tables[2].p1=tables[2].p2=0;tables[2].spectators={1};tables[2].watchingNext.clear();
 tables[3].p1=4;tables[3].p2=5;tables[3].phase=room::TablePhase::Playing;tables[3].matchGeneration=2;
 from=h.actions.size();h.Wait(4);h.Frame(0,3);
 Check(watches(from).empty(),"A stopped follower still moved");
 // From a table's options it watches that table first, then follows on.
 tables[2].spectators.clear();
 h.Screen("room");h.FocusOn("table-1");h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Y on a table did not open its options");
 from=h.actions.size();h.Choose("follow-room");
 Check(watches(from)==std::vector<int>{1},"Follow the room from a table's options did not watch it");
 tables[1].spectators={1};h.Frame(0,3);
 Check(watches(from)==std::vector<int>{1,3},"Following from the options did not go on to the game");
 // A seat ends it.
 tables[1].spectators.clear();tables[3].watchingNext={1};
 tables[1].p1=1;h.view.room.members[0].table=1;h.view.room.members[0].seat=0;tables[1].phase=room::TablePhase::Waiting;
 tables[3].phase=room::TablePhase::Waiting;tables[3].watchingNext.clear();tables[3].spectators={};
 tables[2].p1=2;tables[2].p2=3;tables[2].phase=room::TablePhase::Playing;
 from=h.actions.size();h.Screen("room");h.Wait(4);h.Frame(0,3);capture();
 Check(watches(from).empty()&&!row("follow-room"),"A seat did not end following");
 // An Arcade board's action bar offers it too, and moves the same way.
 tables[1].p1=0;h.view.room.members[0].table=-1;h.view.room.members[0].seat=-1;tables[1].spectators={1};
 h.view.room.arcade=room::ArcadeRoom;h.Screen("room");capture();
 Check(row("follow-room")&&row("follow-room")->enabled,"An Arcade watcher's board did not offer Follow the room");
 from=h.actions.size();h.Choose("follow-room");h.Frame(0,3);
 Check(watches(from)==std::vector<int>{2},"Following in an Arcade room did not move to the pair playing");
}
