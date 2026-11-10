// Launcher --host-public-room, for a PC that streams a public room all day
// with nobody at it.
#include "ApplicationShell.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
// Whenever it is free it opens Public rooms and there the last public room
// created here (CreatePublicRoom keeps its type and template), so the room is
// back after it closes or the game restarts. Like the links it never moves
// the menu over a dialog, a notice or an Ember ID screen. A refused or lost
// request waits 30 s before the next.
void ApplicationShell::UpdateHostedPublicRoom(const ShellView& v,double now) {
 using namespace netplay;
 if(!v.hostPublicRoom||now<hostRoomAt_||v.session.room!=RoomState::Idle||v.session.match!=MatchState::None||!v.canOpenRoom||MenuBusy())return;
 // Public rooms asks for the Ember ID's services, and shows the wait and its answer.
 if(menu_.navigation.Screen()!="public-rooms"){ShowRoomLink();return;}
 if(publicRooms_.Busy()||identity_.UsableBridge(v).empty())return;
 hostRoomAt_=now+30;
 // As Host on Create would, after the template it was made from.
 const auto& templates=preferences_.roomTemplates;
 const auto made=std::find_if(templates.begin(),templates.end(),[&](const RoomTemplate& t){return t.name==preferences_.publicRoomTemplate;});
 const int index=made==templates.end()?-1:static_cast<int>(made-templates.begin());
 if(index>=0&&createTemplate_!=index)ApplyRoomTemplate(index);
 CreatePublicRoom(preferences_.publicRoomType);
}
} }
