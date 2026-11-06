#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include "../netplay/RoomPreferences.hxx"
#include <algorithm>
#include <string>
#include <vector>

// Wired players only: off on Create to begin with, kept with the defaults once
// changed and carried by Create room; the host turns it on and off in Room
// settings; a player on Wi-Fi finds the seat closed and is told why, and the
// room says what it is.
void WiredOnlyJourney() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 h.Screen("online");h.Choose("create");
 Check(row("wired-only")!=rows.end()&&row("wired-only")->value==loc::T("common.off")&&row("wired-only")->label==loc::T("room.wired_only"),
  "Create did not offer Wired players only, off");
 h.Choose("host");
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::HostRoom&&!h.actions.back().wiredOnly,"Create room asked for a wired room");
 Check(!netplay::RoomPreferences(h.actions.back().preferences).contains("wiredOnly"),"An open room was written to the saved defaults as wired");
 h.Screen("online");h.Choose("create");
 h.FocusOn("wired-only");h.Press(MenuInput::Right);
 Check(row("wired-only")->value==loc::T("common.on"),"Wired players only did not turn on");
 h.Choose("host");
 Check(h.actions.back().command.kind==Kind::HostRoom&&h.actions.back().wiredOnly,"Create room did not ask for a wired room");
 Check(netplay::RoomPreferences(h.actions.back().preferences).at("wiredOnly")==true,"Wired players only did not reach the saved defaults");
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
 Check(h.actions.size()==before+1&&h.actions.back().command.kind==Kind::RoomAction&&h.actions.back().roomAction.kind==room::ActionKind::SetWiredOnly&&
  h.actions.back().roomAction.wiredOnly,"Room settings did not turn Wired players only on");
 // With it on, this PC on Wi-Fi finds the queue closed and why; the room says what it is.
 h.view.room.wiredOnly=true;h.Frame();h.Screen("room-table");
 Check(row("queue")!=rows.end()&&!row("queue")->enabled&&row("queue")->detail.find(loc::T("room.reject.wired_only"))==0,
  "A player on Wi-Fi was offered the queue of a wired room");
 Check(row("watch")!=rows.end()&&row("watch")->enabled,"A player on Wi-Fi could not watch in a wired room");
 // An Unknown link is never kept out.
 h.view.room.members[0].link=NetworkLink::Unknown;h.Frame();
 Check(row("queue")->enabled,"A player whose link is unknown was kept out of a wired room");
 SetMenuEntriesProbe({});
}
