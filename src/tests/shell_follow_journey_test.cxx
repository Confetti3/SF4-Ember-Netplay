// Follow the room from the shell (session/RoomFollow.hxx): the board's action
// bar and a table's options offer it, each move goes out as the room's
// ordinary Watch, drawn or hidden, one press stops it, a seat or a lock-in
// ends it, and the member's own Stop watching wins over a move on its way.
#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include "../common/Localization.hxx"
#include "../ui/MenuProbes.hxx"
#include <string>

namespace {
using namespace sf4e;
// A Classic room the player (member 1) has joined, watching table 0, whose
// pair stood up, while table 2 plays. Members 4 and 5 are free to sit
// anywhere a journey seats them.
struct FollowRoom {
 Harness h;
 std::vector<MenuEntry> rows;
 std::array<room::Table,room::TableCount>& tables=h.view.room.tables;
 explicit FollowRoom(std::uint64_t epoch){
  h.Frame();
  JoinedRoom(h,epoch);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=2;h.view.room.revision=3;
  room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
  for(room::MemberId id=2;id<=5;++id){
   room::Member fighter;fighter.id=id;fighter.name="Fighter "+std::to_string(id);
   fighter.table=static_cast<std::int8_t>(id<4?2:3);fighter.seat=static_cast<std::int8_t>(id%2);h.view.room.members.push_back(fighter);
  }
  tables[2].p1=2;tables[2].p2=3;tables[2].phase=room::TablePhase::Playing;tables[2].matchGeneration=4;
  tables[0].spectators={1};
 }
 room::Member& Local(){return h.view.room.members[0];}
 void Capture(){SetMenuEntriesProbe([&](const std::vector<MenuEntry>& seen){rows=seen;});h.Frame();SetMenuEntriesProbe({});}
 const MenuEntry* Row(const char* id) const{
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  return found==rows.end()?nullptr:&*found;
 }
 // The tables of every room action of `kind` sent since `from`.
 std::vector<int> Sent(room::ActionKind kind,std::size_t from) const{
  std::vector<int> sent;
  for(auto i=from;i<h.actions.size();++i)
   if(h.actions[i].command.kind==Kind::RoomAction&&h.actions[i].roomAction.kind==kind)sent.push_back(h.actions[i].roomAction.table);
  return sent;
 }
 std::vector<int> Watches(std::size_t from) const{return Sent(room::ActionKind::Watch,from);}
 // Frames with Ember hidden (F10), `seconds` apart, as the overlay runs them.
 void Hidden(int count=1,float seconds=1.f/60){
  for(int i=0;i<count;++i){
   ImGui::GetIO().DeltaTime=seconds;ImGui::NewFrame();
   h.shell.Background(h.view,h.view.room,[&](ShellAction a){h.actions.push_back(a);return true;});ImGui::Render();
  }
 }
 void TableOptions(const char* card){h.Screen("room");h.FocusOn(card);h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="room-table","Y on a table did not open its options");}
};

void FollowFromBoardAndOptions() {
 FollowRoom f(23);auto& h=f.h;auto& tables=f.tables;
 h.Screen("room");f.Capture();
 Check(f.Row("follow-room")&&f.Row("follow-room")->label==loc::T("room.follow")&&f.Row("follow-room")->enabled,"A watcher's board did not offer Follow the room");
 // A lock-in holds one table, so it is released first.
 f.Local().spectatorLocked=true;f.Capture();
 Check(f.Row("follow-room")&&!f.Row("follow-room")->enabled&&f.Row("follow-room")->detail==loc::T("room.follow.locked_in"),"Follow the room was open to a locked-in watcher");
 f.Local().spectatorLocked=false;
 // Following moves the watcher from the idle table to the game: one Watch, no Unwatch.
 auto from=h.actions.size();
 h.Choose("follow-room");h.Frame(0,3);
 Check(f.Watches(from)==std::vector<int>{2},"Following did not move the watcher to the table playing");
 Check(std::none_of(h.actions.begin()+static_cast<std::ptrdiff_t>(from),h.actions.end(),[](const ShellAction& a){
  return a.roomAction.kind==room::ActionKind::Unwatch||a.roomAction.kind==room::ActionKind::LockSpectating;}),"Following sent more than a Watch");
 tables[0].spectators.clear();tables[2].watchingNext={1};
 from=h.actions.size();h.Frame(0,10);f.Capture();
 Check(f.Watches(from).empty(),"Following moved again while the game it waits for plays");
 Check(f.Row("follow-room")->label==loc::T("room.follow.stop")&&f.Row("follow-room")->detail==loc::Tf("room.follow.stop_detail",3),
  "The board did not say which table is followed");
 // One press stops it: the table stopping no longer moves the watcher.
 h.Choose("follow-room");f.Capture();
 Check(f.Row("follow-room")->label==loc::T("room.follow"),"Stop following did not stop");
 tables[2].phase=room::TablePhase::Waiting;tables[2].p1=tables[2].p2=0;tables[2].spectators={1};tables[2].watchingNext.clear();
 tables[3].p1=4;tables[3].p2=5;tables[3].phase=room::TablePhase::Playing;tables[3].matchGeneration=2;
 from=h.actions.size();h.Wait(4);h.Frame(0,3);
 Check(f.Watches(from).empty(),"A stopped follower still moved");
 // From a table's options it watches that table first, then follows on.
 tables[2].spectators.clear();
 f.TableOptions("table-1");
 from=h.actions.size();h.Choose("follow-room");
 Check(f.Watches(from)==std::vector<int>{1},"Follow the room from a table's options did not watch it");
 tables[1].spectators={1};h.Frame(0,3);
 Check(f.Watches(from)==std::vector<int>{1,3},"Following from the options did not go on to the game");
 // A seat ends it.
 tables[1].spectators.clear();tables[3].watchingNext={1};
 tables[1].p1=1;f.Local().table=1;f.Local().seat=0;tables[1].phase=room::TablePhase::Waiting;
 tables[3].phase=room::TablePhase::Waiting;tables[3].watchingNext.clear();tables[3].spectators={};
 tables[2].p1=2;tables[2].p2=3;tables[2].phase=room::TablePhase::Playing;
 from=h.actions.size();h.Screen("room");h.Wait(4);h.Frame(0,3);f.Capture();
 Check(f.Watches(from).empty()&&!f.Row("follow-room"),"A seat did not end following");
 // An Arcade board's action bar offers it too, and moves the same way.
 tables[1].p1=0;f.Local().table=-1;f.Local().seat=-1;tables[1].spectators={1};
 h.view.room.arcade=room::ArcadeRoom;h.Screen("room");f.Capture();
 Check(f.Row("follow-room")&&f.Row("follow-room")->enabled,"An Arcade watcher's board did not offer Follow the room");
 from=h.actions.size();h.Choose("follow-room");h.Frame(0,3);
 Check(f.Watches(from)==std::vector<int>{2},"Following in an Arcade room did not move to the pair playing");
}

// Streamers hide Ember (F10) to show the games: following goes on in the
// hidden frames, through a game it is shown there, and its moves go out.
void FollowWhileHidden() {
 FollowRoom f(31);auto& h=f.h;auto& tables=f.tables;
 h.Screen("room");h.Choose("follow-room");h.Frame(0,3);
 tables[0].spectators.clear();tables[2].watchingNext={1};
 // Hidden: table 2's game ends with its pair seated, and their next one is
 // shown on this PC from its start.
 f.Hidden(3);
 tables[2].phase=room::TablePhase::Waiting;tables[2].spectators={1};tables[2].watchingNext.clear();f.Hidden(3);
 tables[2].phase=room::TablePhase::Playing;tables[2].matchGeneration=5;h.view.watchedGame={2,5};f.Hidden(3);
 // Table 3 plays while that game still shows here: nothing moves.
 tables[3].p1=4;tables[3].p2=5;tables[3].phase=room::TablePhase::Playing;tables[3].matchGeneration=7;
 auto from=h.actions.size();f.Hidden(5,1.f);
 Check(f.Watches(from).empty(),"Following moved while the watched game still showed");
 // The game ends in the room and on this PC. It was one the follower was
 // shown, so the pair still seated keeps it for the rematch grace only.
 tables[2].phase=room::TablePhase::Waiting;h.view.watchedGame={};f.Hidden();
 f.Hidden(1,2.f);
 Check(f.Watches(from).empty(),"Following left before the rematch grace ran out");
 f.Hidden(1,1.5f);f.Hidden();
 Check(f.Watches(from)==std::vector<int>{3},"Following did not move while Ember was hidden");
}

// A lock-in pressed on an older snapshot than Follow the room's press lands
// after following started: following ends there, never holding the start,
// and Release lock-in is in reach.
void LockInLandsAfterFollow() {
 FollowRoom f(37);auto& h=f.h;auto& tables=f.tables;
 tables[2].phase=room::TablePhase::Waiting;
 tables[0].p1=4;tables[0].p2=5;tables[0].phase=room::TablePhase::Waiting;
 f.TableOptions("table-0");
 auto from=h.actions.size();h.Choose("lock-spectating");
 Check(f.Sent(room::ActionKind::LockSpectating,from).size()==1,"Lock in was not sent");
 h.Choose("follow-room");f.Capture();
 Check(f.Row("follow-room")->label==loc::T("room.follow.stop"),"Follow the room did not start");
 f.Local().spectatorLocked=true;
 tables[0].phase=room::TablePhase::Ready;tables[0].ready[0]=tables[0].ready[1]=true;tables[0].spectatorHold=true;h.view.room.localTerminalPending=true;
 // The frame that takes the snapshot ends following after its rows; the next shows it.
 h.Frame();f.Capture();
 Check(f.Row("follow-room")->label==loc::T("room.follow")&&!f.Row("follow-room")->enabled,"A lock-in that landed left the watcher following");
 Check(f.Row("lock-spectating")&&f.Row("lock-spectating")->label==loc::T("room.unlock_spectating"),"Release lock-in was out of reach");
 tables[0].phase=room::TablePhase::Waiting;tables[0].spectatorHold=false;h.view.room.localTerminalPending=false;
 tables[2].phase=room::TablePhase::Playing;
 from=h.actions.size();h.Wait(25);h.Frame(0,3);
 Check(f.Watches(from).empty(),"A locked-in watcher still moved");
}

// The member's own Stop watching wins over a move still on its way, and holds
// until the room shows it. Following sent Watch 2; before any answer, the
// member presses Stop watching at table 0 twice. The room takes the Watch
// first, so both Unwatches find nothing at 0. Following, off at once, leaves
// table 2 when the room shows the member there; that Unwatch does not go the
// first time, and goes again after the wait. Then nothing more is sent.
void StopWatchingBeatsMove() {
 FollowRoom f(41);auto& h=f.h;auto& tables=f.tables;
 f.TableOptions("table-0");
 auto from=h.actions.size();h.Choose("follow-room");
 Check(f.Watches(from)==std::vector<int>{2},"Following did not move");
 h.Choose("unwatch");f.Capture();
 Check(f.Sent(room::ActionKind::Unwatch,from)==std::vector<int>{0}&&f.Row("follow-room")->label==loc::T("room.follow"),
  "Stop watching did not end following at once");
 h.Press(MenuInput::Select);
 Check(f.Sent(room::ActionKind::Unwatch,from)==std::vector<int>{0,0},"The second Stop watching was not sent");
 h.accept=false;
 tables[0].spectators.clear();tables[2].watchingNext={1};h.Frame(0,3);
 Check(f.Sent(room::ActionKind::Unwatch,from)==std::vector<int>{0,0,2},"The move that landed after Stop watching was not left");
 h.accept=true;h.Frame(0,3);
 Check(f.Sent(room::ActionKind::Unwatch,from).size()==3,"An Unwatch that did not go was sent again at once");
 h.Wait(room::FollowMoveTimeoutMs/1000.f+.5f);h.Frame(0,3);
 Check(f.Sent(room::ActionKind::Unwatch,from)==std::vector<int>{0,0,2,2},"An Unwatch that did not go was forgotten");
 tables[2].watchingNext.clear();tables[3].p1=4;tables[3].p2=5;tables[3].phase=room::TablePhase::Playing;
 h.Wait(30);h.Frame(0,3);
 Check(f.Watches(from)==std::vector<int>{2}&&f.Sent(room::ActionKind::Unwatch,from).size()==4,"Something was sent after the room showed Stop watching");
}

// A Stop watching pressed on the very frame a move falls due comes first.
void StopWatchingOnTheDueFrame() {
 FollowRoom f(43);auto& h=f.h;auto& tables=f.tables;
 tables[0].p1=4;tables[0].p2=5;tables[0].phase=room::TablePhase::Playing;tables[0].matchGeneration=3;
 f.TableOptions("table-0");
 h.Choose("follow-room");h.FocusOn("unwatch");
 // The game it was shown ends; the rematch grace runs out on the frame of the press.
 tables[0].phase=room::TablePhase::Waiting;h.Frame();
 const auto from=h.actions.size();
 h.Frame(MenuInput::Select,1,room::FollowRematchGraceMs/1000.f+.5f);h.Frame(0,3);
 Check(f.Sent(room::ActionKind::Unwatch,from)==std::vector<int>{0}&&f.Watches(from).empty(),"A move went out on the frame Stop watching was pressed");
}
}

void FollowJourneys() {
 FollowFromBoardAndOptions();
 FollowWhileHidden();
 LockInLandsAfterFollow();
 StopWatchingBeatsMove();
 StopWatchingOnTheDueFrame();
}
