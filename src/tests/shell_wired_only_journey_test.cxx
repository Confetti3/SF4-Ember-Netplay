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
