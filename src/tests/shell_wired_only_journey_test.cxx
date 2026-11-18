#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../ui/RoomControls.hxx"
#include <algorithm>
#include <string>
#include <vector>

// Wired players only: off on Create to begin with, kept with the defaults once
// changed and carried by Create room; the host turns it on and off in Room
// settings; a player on Wi-Fi finds the seat closed and is told why, and the
// room says what it is.
void WiredOnlyJourney() {
 using namespace sf4e;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 h.Screen("online");h.Choose("create");
 Check(row("wired-only")!=rows.end()&&row("wired-only")->value==loc::T("common.off")&&row("wired-only")->label==loc::T("room.wired_only"),
  "Create did not offer Wired players only, off");
 h.Choose("host");
 const auto* hosted=h.actions.empty()?nullptr:As<commands::HostRoom>(h.actions.back());
 Check(hosted&&!hosted->charter.wiredOnly,"Create room asked for a wired room");
 Check(!netplay::RoomPreferences(hosted->preferences).contains("wiredOnly"),"An open room was written to the saved defaults as wired");
 h.Screen("online");h.Choose("create");
 h.FocusOn("wired-only");h.Press(MenuInput::Right);
 Check(row("wired-only")->value==loc::T("common.on"),"Wired players only did not turn on");
 h.Choose("host");
 hosted=As<commands::HostRoom>(h.actions.back());
 Check(hosted&&hosted->charter.wiredOnly,"Create room did not ask for a wired room");
 Check(netplay::RoomPreferences(hosted->preferences).at("wiredOnly")==true,"Wired players only did not reach the saved defaults");
 h.Screen("online");h.Choose("create");
 Check(row("wired-only")->value==loc::T("common.on"),"Wired players only was forgotten when Create opened again");
 // A public room offers it too, and the gameplay defaults keep it.
 h.FocusOn("visibility");h.Press(MenuInput::Right);
 Check(row("wired-only")!=rows.end(),"Public Create did not offer Wired players only");
 h.Press(MenuInput::Left);
 h.Screen("tab-matches/defaults");
 Check(row("wired-only")!=rows.end(),"Gameplay defaults did not offer Wired players only");
 // In the room: the host turns it on in Room settings.
 room::Member me,other;me.id=1;me.name="Me";me.host=true;me.link=NetworkLink::Wireless;other.id=2;other.name="Other";
 JoinedRoom(h,14);h.view.session.control=netplay::Health::Healthy;
 h.view.room.members={me,other};h.view.room.host=1;
 h.Frame();h.Screen("room-admin");
 Check(row("room-wired-only")!=rows.end()&&row("room-wired-only")->value==loc::T("common.off")&&row("room-wired-only")->enabled,
  "Room settings did not offer the host Wired players only");
 const auto before=h.actions.size();
 h.FocusOn("room-wired-only");h.Press(MenuInput::Right);
 Check(h.actions.size()==before+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){
  return sent.roomAction.kind==room::ActionKind::SetWiredOnly&&sent.roomAction.wiredOnly;}),"Room settings did not turn Wired players only on");
 // With it on, this PC on Wi-Fi finds the queue closed and why; the room says what it is.
 h.view.room.wiredOnly=true;h.Frame();h.Screen("room-table");
 Check(row("queue")!=rows.end()&&!row("queue")->enabled&&row("queue")->detail.find(loc::T("room.reject.wired_only"))==0,
  "A player on Wi-Fi was offered the queue of a wired room");
 Check(row("watch")!=rows.end()&&row("watch")->enabled,"A player on Wi-Fi could not watch in a wired room");
 // An Unknown link is never kept out.
 h.view.room.members[0].link=NetworkLink::Unknown;h.Frame();
 Check(row("queue")->enabled,"A player whose link is unknown was kept out of a wired room");
 // On the board a table's seats say the same, and A on one sends nothing.
 h.view.room.members[0].link=NetworkLink::Wireless;h.Screen("room");h.FocusOn("table-0");
 {const auto offers=room_controls::SeatChoices(h.view,h.view.room.tables[0]);
  Check(offers.size()==2&&!offers[0].enabled&&offers[0].detail.find(loc::T("room.reject.wired_only"))==0,
   "A seat of a wired room was offered to a player on Wi-Fi");}
 std::string told;SetMenuStatusProbe([&](const char* text,Tone){told=text;});
 const auto refused=h.actions.size();h.Press(MenuInput::Select);h.Press(MenuInput::Select);
 Check(h.actions.size()==refused&&told.find(loc::T("room.reject.wired_only"))==0,"A on a wired room's seat sent it for a player on Wi-Fi, or did not say why");
 SetMenuStatusProbe({});
 SetMenuEntriesProbe({});
}

// Max ping: off on Create to begin with, stepped through its limits, kept with
// the defaults and carried by Create room; the host changes it in Room
// settings; a player whose measured ping to someone at the table is over it
// finds the seat closed and is told the figure, one still being measured is
// told so, and a seat taken carries the figures the room weighs.
void MaxPingJourney() {
 using namespace sf4e;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 h.Screen("online");h.Choose("create");
 Check(row("max-ping")!=rows.end()&&row("max-ping")->value==loc::T("common.off")&&row("max-ping")->label==loc::T("room.max_ping")&&
  row("max-ping")==row("wired-only")+1,"Create did not offer Max ping, off, after Wired players only");
 h.FocusOn("max-ping");h.Press(MenuInput::Left);
 Check(row("max-ping")->value==loc::T("common.off"),"Max ping went below Off");
 h.Press(MenuInput::Right);h.Press(MenuInput::Right);
 Check(row("max-ping")->value==loc::Tf("room.latency.value",100),"Max ping did not step to 100 ms");
 h.Choose("host");
 const auto* hosted=h.actions.empty()?nullptr:As<commands::HostRoom>(h.actions.back());
 Check(hosted&&hosted->charter.maxPingMs==100,"Create room did not ask for a Max ping of 100 ms");
 Check(hosted&&netplay::RoomPreferences(hosted->preferences).at("maxPingMs")==100,"Max ping did not reach the saved defaults");
 h.Screen("tab-matches/defaults");
 Check(row("max-ping")!=rows.end()&&row("max-ping")->value==loc::Tf("room.latency.value",100),"Gameplay defaults did not offer Max ping, as saved");
 // In the room: the host changes it in Room settings.
 room::Member me,other;me.id=1;me.name="Me";me.host=true;other.id=2;other.name="Other";
 JoinedRoom(h,15);h.view.session.control=netplay::Health::Healthy;
 h.view.room.members={me,other};h.view.room.host=1;
 h.Frame();h.Screen("room-admin");
 Check(row("room-max-ping")!=rows.end()&&row("room-max-ping")->value==loc::T("common.off")&&row("room-max-ping")->enabled,
  "Room settings did not offer the host Max ping");
 const auto before=h.actions.size();
 h.FocusOn("room-max-ping");h.Press(MenuInput::Right);
 Check(h.actions.size()==before+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){
  return sent.roomAction.kind==room::ActionKind::SetMaxPing&&sent.roomAction.maxPingMs==80;}),"Room settings did not set Max ping to 80 ms");
 // With 100 ms on and Other seated: over it the queue is closed, with the figure.
 h.view.room.maxPingMs=100;
 h.view.room.tables[0].p1=2;h.view.room.members[1].table=0;h.view.room.members[1].seat=0;h.view.room.members[1].status=room::MemberStatus::Seated;
 session::MemberLatency ping;ping.incarnation=h.view.room.members[1].incarnation;ping.state=session::MemberLatency::State::Value;ping.rttMs=140;ping.relay=true;
 h.view.memberLatency[2]=ping;
 h.Frame();h.Screen("room-table");
 Check(row("queue")!=rows.end()&&!row("queue")->enabled&&row("queue")->detail.find(loc::Tf("room.max_ping.over","Other",140,100))==0,
  "A player over Max ping was offered the queue, or not told the figure");
 Check(row("watch")!=rows.end()&&row("watch")->enabled,"A player over Max ping could not watch");
 Check(room_controls::RoomRulesText(h.view,h.view.room.tables[0]).find(loc::Tf("room.max_ping.rule",100))!=std::string::npos,
  "The table's rules line did not say Max ping");
 // Still being measured: closed for now, and told so.
 h.view.memberLatency[2].state=session::MemberLatency::State::Measuring;h.Frame();
 Check(!row("queue")->enabled&&row("queue")->detail.find(loc::T("room.max_ping.measuring"))==0,"A ping still being measured did not wait");
 // Not measurable, as in a game: the seat is open, and it carries no figure.
 h.view.memberLatency[2].state=session::MemberLatency::State::Unavailable;h.view.memberLatency[2].reason="in_match";h.Frame();
 Check(row("queue")->enabled,"A ping that cannot be measured kept the player out");
 h.FocusOn("queue");h.Press(MenuInput::Select);
 Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){
  return sent.roomAction.kind==room::ActionKind::Queue&&sent.roomAction.pings.empty();}),"A seat carried a figure that was not measured");
 // Within it: the seat is open and carries the figure.
 h.view.memberLatency[2]=ping;h.view.memberLatency[2].rttMs=60;h.Frame();
 Check(row("queue")->enabled,"A player within Max ping was kept out");
 h.FocusOn("queue");h.Press(MenuInput::Select);
 Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){
  return sent.roomAction.kind==room::ActionKind::Queue&&sent.roomAction.pings.size()==1&&
   sent.roomAction.pings[0].member==2&&sent.roomAction.pings[0].ms==60;}),"A seat did not carry the measured ping");
 SetMenuEntriesProbe({});
}

// Max ping while watching: this PC's helper measures nobody while it watches a
// game, so the queue at another table is weighed with the last ping measured
// before then. Over the limit it stays closed with that figure; within it the
// Queue carries the figure; with none for a player out of a game it waits
// until this PC stops watching. Players in the watched game cannot be measured
// by anyone and hold nothing.
void MaxPingWatchingJourney() {
 using namespace sf4e;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 room::Member me,distant,p1,p2;me.id=1;me.name="Me";me.host=true;distant.id=2;distant.name="Far";p1.id=3;p1.name="P1";p2.id=4;p2.name="P2";
 JoinedRoom(h,16);h.view.session.control=netplay::Health::Healthy;
 auto& s=h.view.room;
 s.members={me,distant,p1,p2};s.host=1;s.maxPingMs=100;
 h.Frame();
 // Far waits at table 0; table 1 plays P1 against P2, and this PC watches it.
 s.tables[0].phase=room::TablePhase::Waiting;s.tables[0].p1=2;
 s.members[1].table=0;s.members[1].seat=0;s.members[1].status=room::MemberStatus::Seated;
 s.tables[1].phase=room::TablePhase::Playing;s.tables[1].matchGeneration=7;s.tables[1].p1=3;s.tables[1].p2=4;s.tables[1].spectators={1};
 for(const int fighter:{2,3}){s.members[fighter].table=1;s.members[fighter].seat=static_cast<std::int8_t>(fighter-2);s.members[fighter].status=room::MemberStatus::Playing;}
 s.members[0].table=1;s.members[0].status=room::MemberStatus::Watching;
 h.view.watchedGame={1,7};
 // The helper names everyone room_unavailable while this PC is in a game; the
 // 140 ms it measured to Far before the game began is kept.
 for(const auto& member:s.members){
  if(member.id==s.localMember)continue;
  session::MemberLatency none;none.incarnation=member.incarnation;none.state=session::MemberLatency::State::Unavailable;none.reason="room_unavailable";
  h.view.memberLatency[member.id]=none;
 }
 h.view.knownPings[2]={s.members[1].incarnation,140};
 h.Frame();h.Screen("room-table");
 Check(row("queue")!=rows.end()&&!row("queue")->enabled&&row("queue")->detail.find(loc::Tf("room.max_ping.over","Far",140,100))==0,
  "A watcher over Max ping was offered the queue, or not told the figure kept from before the game");
 {const auto offers=room_controls::SeatChoices(h.view,s.tables[0]);
  Check(!offers.empty()&&!offers[0].enabled&&offers[0].detail.find(loc::Tf("room.max_ping.over","Far",140,100))==0,
   "The board offered a watcher over Max ping Far's table");}
 // Within it: the queue opens, and the Queue carries the kept figure.
 h.view.knownPings[2].ms=60;h.Frame();
 Check(row("queue")->enabled,"A watcher within Max ping was kept out");
 const auto before=h.actions.size();
 h.FocusOn("queue");h.Press(MenuInput::Select);
 Check(h.actions.size()==before+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){
  return sent.roomAction.kind==room::ActionKind::Queue&&sent.roomAction.pings.size()==1&&
   sent.roomAction.pings[0].member==2&&sent.roomAction.pings[0].ms==60;}),"A watcher's Queue did not carry the ping kept from before the game");
 // No figure for Far, who is out of a game: the queue waits for this PC to stop watching.
 h.view.knownPings.clear();h.Frame();
 Check(!row("queue")->enabled&&row("queue")->detail.find(loc::T("room.max_ping.after_watching"))==0,
  "A watcher with no ping to a player out of a game was offered the queue, or not told why");
 // The watched game's own queue: its fighters are in a game, so nothing waits.
 {const auto offers=room_controls::SeatChoices(h.view,s.tables[1]);
  Check(!offers.empty()&&offers[0].id=="queue"&&offers[0].enabled,"A ping nobody can measure kept a watcher out of the watched table's queue");}
 // Stopped watching: the helper measures Far again, and the queue waits for that.
 h.view.watchedGame={};s.tables[1].spectators.clear();s.members[0].table=-1;s.members[0].status=room::MemberStatus::Idle;
 h.view.memberLatency[2].state=session::MemberLatency::State::Measuring;h.view.memberLatency[2].reason.clear();h.Frame();
 Check(!row("queue")->enabled&&row("queue")->detail.find(loc::T("room.max_ping.measuring"))==0,"After watching, a ping being measured did not wait");
 SetMenuEntriesProbe({});
}

// Privacy: a private room's Friends or Hidden, on Create right after Visibility and not for a public room, kept with the
// defaults once changed and carried by Create room; the host changes it in Room settings, which a public room leaves out.
void PrivacyJourney() {
 using namespace sf4e;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 h.Screen("online");h.Choose("create");
 Check(row("privacy")!=rows.end()&&row("privacy")->value==loc::T("room.privacy.friends")&&row("privacy")->label==loc::T("room.privacy"),
  "Create did not offer Privacy, on Friends");
 Check(row("visibility")!=rows.end()&&row("privacy")==row("visibility")+1,"Privacy is not right after Visibility on Create");
 h.Choose("host");
 const auto* hosted=h.actions.empty()?nullptr:As<commands::HostRoom>(h.actions.back());
 Check(hosted&&!hosted->charter.friendsHidden,"Create room asked for a hidden room");
 Check(!netplay::RoomPreferences(hosted->preferences).contains("friendsHidden"),"A room open to friends was written to the saved defaults as hidden");
 h.Screen("online");h.Choose("create");
 h.FocusOn("privacy");h.Press(MenuInput::Right);
 Check(row("privacy")->value==loc::T("room.privacy.hidden"),"Privacy did not turn to Hidden");
 h.Choose("host");
 hosted=As<commands::HostRoom>(h.actions.back());
 Check(hosted&&hosted->charter.friendsHidden,"Create room did not ask for a hidden room");
 Check(netplay::RoomPreferences(hosted->preferences).at("friendsHidden")==true,"Hidden did not reach the saved defaults");
 h.Screen("online");h.Choose("create");
 Check(row("privacy")->value==loc::T("room.privacy.hidden"),"Privacy was forgotten when Create opened again");
 // A public room has no Privacy; the gameplay defaults keep it.
 h.FocusOn("visibility");h.Press(MenuInput::Right);
 Check(row("privacy")==rows.end(),"Public Create offered Privacy");
 h.Press(MenuInput::Left);
 Check(row("privacy")!=rows.end(),"Private Create lost Privacy");
 h.Screen("tab-matches/defaults");
 Check(row("privacy")!=rows.end()&&row("privacy")->value==loc::T("room.privacy.hidden"),"Gameplay defaults did not offer Privacy, as saved");
 // In the room: the host changes it in Room settings.
 room::Member me,other;me.id=1;me.name="Me";me.host=true;other.id=2;other.name="Other";
 JoinedRoom(h,14);h.view.session.control=netplay::Health::Healthy;
 h.view.room.members={me,other};h.view.room.host=1;
 h.Frame();h.Screen("room-admin");
 Check(row("room-privacy")!=rows.end()&&row("room-privacy")->value==loc::T("room.privacy.friends")&&row("room-privacy")->enabled,
  "Room settings did not offer the host Privacy, on Friends");
 const auto before=h.actions.size();
 h.FocusOn("room-privacy");h.Press(MenuInput::Right);
 Check(h.actions.size()==before+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){
  return sent.roomAction.kind==room::ActionKind::SetFriendsHidden&&sent.roomAction.friendsHidden;}),"Room settings did not hide the room");
 // Hidden already: Left sends Friends. A public room and a tournament's leave the row out; a member who is not the host cannot change it.
 h.view.room.friendsHidden=true;h.Frame();
 h.FocusOn("room-privacy");h.Press(MenuInput::Left);
 Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){
  return sent.roomAction.kind==room::ActionKind::SetFriendsHidden&&!sent.roomAction.friendsHidden;}),"Room settings did not show the room to friends again");
 h.view.room.serverOwned=true;h.Frame();
 Check(row("room-privacy")==rows.end(),"A public room offered Privacy in its settings");
 h.view.room.serverOwned=false;h.view.room.host=2;h.Frame();
 Check(row("room-privacy")!=rows.end()&&!row("room-privacy")->enabled,"A member who is not the host could change Privacy");
 SetMenuEntriesProbe({});
}
