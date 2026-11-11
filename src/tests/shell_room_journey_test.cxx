#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include "../ui/RoomPanelBoard.hxx"
#include <algorithm>
#include <iterator>
#include <map>
namespace {
// In a room its shortcuts come before any panel's, on a panel's own screen
// too: Y on Rankings opens the table options, not the rankings refresh.
void RoomShortcutOnPanelScreen() {
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,7);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;
 room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
 h.Frame();
 h.Screen("rankings");h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Y on a panel's screen in a room did not open the table options");
 h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="room","Y again did not return to the board from the table options");
}
}
// EMBER-68: the host names a table. The name's text row shares the cards'
// "table-" prefix; committing it sends NameTable and parses no number.
void TableNameCommitNamesTable() {
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,16);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;h.view.room.revision=3;
 for(auto& table:h.view.room.tables)table.revision=7;
 room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
 h.Frame();h.Screen("room");h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Y did not open the table options");
 h.Choose("table-name");Check(h.shell.Navigation().Editing(),"The table name did not open its editor");
 ImGui::GetIO().AddInputCharactersUTF8("Finals");h.Frame();
 const auto sent=h.actions.size();
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 Check(h.actions.size()==sent+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::NameTable&&sent.roomAction.text=="Finals"&&
  sent.roomAction.table==0;}),"Committing a table name did not send NameTable");
}
// Each of the board's rows is in the area that draws it, the one answer the
// action bar, LB and RB and the narrow tabs read: the room's status while it
// opens is a button on the action bar, so a step to the floor never lands on
// it. The narrow tabs fold the bar under the floor's.
void BoardAreasAreWhereDrawn() {
 using namespace sf4e;
 Check(BoardAreaOf("table-2")==BoardArea::Floor&&BoardAreaOf("cabinet-9")==BoardArea::Floor&&BoardAreaOf("matchup-1")==BoardArea::Floor&&
  BoardAreaOf("room-hide-picture")==BoardArea::Floor,"A floor row is not in the floor's area");
 Check(BoardAreaOf("room-status")==BoardArea::Bar&&BoardAreaOf("ready")==BoardArea::Bar&&BoardAreaOf("copy-short")==BoardArea::Bar&&
  BoardAreaOf("leave")==BoardArea::Bar,"The room's status or a bar button is not on the bar");
 Check(BoardAreaOf("member-7")==BoardArea::Members&&BoardAreaOf("room-members")==BoardArea::Members&&BoardAreaOf("room-show-members")==BoardArea::Members&&
  BoardAreaOf("room-chat")==BoardArea::Chat&&BoardAreaOf("room-hide-chat")==BoardArea::Chat,"A panel's row is not in its panel's area");
 Check(BoardTabOf(BoardArea::Bar,true)==0&&BoardTabOf(BoardArea::Members,true)==1&&BoardTabOf(BoardArea::Chat,true)==2&&
  BoardTabOf(BoardArea::Chat,false)==3&&BoardTabCount(true)==3&&BoardTabCount(false)==4,"The narrow tabs do not fold the bar under the floor's");
}
// The narrow board shows one part at a time under its tabs: RB steps Board,
// Members, Chat and round again, LB back, as the caps on the strip say, each
// tab back at the row last focused there, as a wide board's areas are.
void NarrowBoardTabsStep() {
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,18);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;h.view.room.revision=3;
 room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";h.view.room.members.push_back(peer);
 ImGui::GetIO().DisplaySize=ImVec2(640,720);
 h.Frame();h.Screen("room");h.Frame();
 const auto focus=[&]{return h.shell.Navigation().Focus();};
 Check(focus().compare(0,6,"table-")==0,"The narrow board did not open on its tables");
 h.Press(MenuInput::NextTab);const std::string first=focus();Check(first.compare(0,7,"member-")==0,"RB on the narrow board did not show Members");
 h.Press(MenuInput::Down);const std::string member=focus();
 Check(member.compare(0,7,"member-")==0&&member!=first,"Down in the narrow Members tab did not reach the next member");
 h.Press(MenuInput::NextTab);Check(focus()=="room-chat","RB from Members did not show Chat");
 h.Press(MenuInput::NextTab);Check(focus().compare(0,6,"table-")==0,"RB from Chat did not wrap to the board");
 h.Press(MenuInput::PrevTab);Check(focus()=="room-chat"&&h.shell.Navigation().Screen()=="room","LB from the board did not wrap to Chat");
 h.Press(MenuInput::PrevTab);Check(focus()==member,"LB to the narrow Members tab did not return to the member last focused there");
 ImGui::GetIO().DisplaySize=ImVec2(1280,960);
}
// The board opens on what the player most likely does next, once per room:
// Ready in a seat, else the first table with a free seat, never the last
// room's row. On a wide board LB and RB step the floor, the action bar,
// Members and Chat, round at the ends, each back at the row last focused
// there; an open popout keeps them. The bar's Left and Right go along it and
// stop at its ends.
void BoardOpensOnReadyAndStepsAreas() {
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,21);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;h.view.room.revision=3;
 room::Member local;local.id=1;local.name="Local";local.table=1;local.seat=0;local.status=room::MemberStatus::Seated;
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=1;peer.seat=1;peer.status=room::MemberStatus::Seated;
 h.view.room.members={local,peer};h.view.room.tables[1].p1=1;h.view.room.tables[1].p2=2;h.view.room.tables[1].phase=room::TablePhase::Waiting;
 h.view.canReady=true;h.view.invitation="sf4://lab";
 h.Frame(0,3);
 const auto focus=[&]{return h.shell.Navigation().Focus();};
 Check(h.shell.Navigation().Screen()=="room"&&focus()=="ready","A seated player's board did not open on Ready");
 h.Press(MenuInput::NextTab);Check(focus()=="member-1","RB from the bar did not go to the first member");
 h.Press(MenuInput::NextTab);Check(focus()=="room-chat","RB from Members did not go to the message box");
 // Up from the message box is its panel's Hide, not a table card across the board.
 h.Press(MenuInput::Up);Check(focus()=="room-hide-chat","Up from the message box left the Chat panel");
 h.Press(MenuInput::Down);Check(focus()=="room-chat","Down from Chat's Hide did not return to the message box");
 h.Press(MenuInput::NextTab);Check(focus()=="table-1","RB from Chat did not go round to the player's own table");
 h.Press(MenuInput::NextTab);Check(focus()=="ready","RB from the floor did not return to the bar's Ready");
 h.Press(MenuInput::Right);const std::string kept=focus();Check(kept!="ready","Right on the bar did not move along it");
 h.Press(MenuInput::PrevTab);Check(focus()=="table-1","LB from the bar did not go to the floor");
 h.Press(MenuInput::PrevTab);Check(focus()=="room-chat","LB from the floor did not go round to Chat");
 h.Press(MenuInput::NextTab);h.Press(MenuInput::NextTab);Check(focus()==kept,"The bar did not keep the row last focused there");
 // Along the bar to its end, where Right stays and does not leave for the column.
 for(int i=0;i<12&&focus()!="leave";++i)h.Press(MenuInput::Right);
 Check(focus()=="leave","Right did not walk the bar to Leave room");
 h.Press(MenuInput::Right);Check(focus()=="leave","Right from the bar's end left the bar");
 // An open popout keeps LB and RB from moving the focus under it.
 h.Press(MenuInput::Left);Check(focus()=="room-more","Left from Leave room is not More...");
 h.Press(MenuInput::Select);Check(h.shell.Navigation().Choosing(),"More... did not open");
 h.Press(MenuInput::NextTab);Check(h.shell.Navigation().Choosing()&&focus()=="room-more","RB moved the focus from under an open popout");
 h.Press(MenuInput::Back);
 // The next room opens afresh: unseated, on the first table with a free seat.
 h.view.room.roomEpoch=22;h.view.session.generation.room=2;
 h.view.room.members[0].table=h.view.room.members[0].seat=-1;h.view.room.members[0].status=room::MemberStatus::Idle;
 h.view.room.tables[1].p1=0;h.view.room.tables[0].p1=2;h.view.room.tables[0].p2=3;
 room::Member third;third.id=3;third.name="Third";third.table=0;third.seat=1;h.view.room.members.push_back(third);
 h.view.room.members[1].table=0;h.view.room.members[1].seat=0;h.view.room.tables[1].p2=0;
 // The board is already up, so nothing pushes it: the frame the opening rule
 // moves the focus counts nothing, and A pressed then and held on counts only
 // once released, never opening the seat chooser on the row it moved to.
 const auto sent=h.actions.size();
 h.Frame(MenuInput::Select,3);
 Check(h.shell.Navigation().Screen()=="room"&&focus()=="table-1","The next room's board did not open on its first free table");
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==sent,"A pressed as the opening rule moved the focus acted on the row it moved to");
 h.Frame(0,2);
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==sent,"A held over the opening focus acted when released");
 // Watching a game, on the bar: Table options (Stop watching while the game shows).
 h.view.room.roomEpoch=23;h.view.session.generation.room=3;h.view.room.tables[0].phase=room::TablePhase::Playing;
 h.view.room.tables[0].spectators={1};h.view.room.members[0].status=room::MemberStatus::Watching;
 h.Frame(0,3);
 Check(focus()=="options","A watcher's board did not open on the action bar");
}
// The row LB or RB lands on scrolls into view as a D-pad move's does, by the
// next frame: the player's own table card below the fold, and Ready when the
// bar follows the tables in their scroll.
void AreaStepScrollsIntoView() {
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,24);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;h.view.room.revision=3;
 h.view.room.tableCount=8;
 room::Member local;local.id=1;local.name="Local";local.table=7;local.seat=0;local.status=room::MemberStatus::Seated;
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=7;peer.seat=1;peer.status=room::MemberStatus::Seated;
 h.view.room.members={local,peer};h.view.room.tables[7].p1=1;h.view.room.tables[7].p2=2;h.view.room.tables[7].phase=room::TablePhase::Waiting;
 h.view.canReady=true;h.view.invitation="sf4://lab";
 std::map<std::string,bool> inView;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){
  const auto* d=ImGui::GetWindowDrawList();const ImVec2 lo=d->GetClipRectMin(),hi=d->GetClipRectMax();
  // Up and down in its scroll (a card runs under the scroll's bar); the clip
  // is whole pixels, the box not: a pixel's slack.
  inView[id]=min.y>=lo.y-1&&max.y<=hi.y+1;});
 const auto focus=[&]{return h.shell.Navigation().Focus();};
 ImGui::GetIO().DisplaySize=ImVec2(1280,720);
 h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="room"&&focus()=="ready"&&inView["ready"]&&!inView["table-7"],
  "The fixture's own table card was not below the fold, under a bar in view");
 h.Press(MenuInput::PrevTab);
 Check(focus()=="table-7"&&inView["table-7"],"LB to the floor left the player's own card out of view");
 // Shorter, the bar follows the tables in their scroll, out of sight under
 // the cards (Up goes further from it, if need be), and RB back to Ready
 // brings it into view.
 ImGui::GetIO().DisplaySize=ImVec2(1280,480);h.Frame(0,3);
 for(int i=0;i<6&&inView["ready"];++i)h.Press(MenuInput::Up);
 Check(focus().compare(0,6,"table-")==0&&!inView["ready"],"The fixture's bar did not follow the tables out of sight");
 h.Press(MenuInput::NextTab);
 Check(focus()=="ready"&&inView["ready"],"RB to the bar left Ready out of view in the tables' scroll");
 SetMenuCardProbe({});ImGui::GetIO().DisplaySize=ImVec2(1280,960);
}
// A room locks the player's settings but those it leaves open
// (PlayerPreferences::CopyRoomSettings), the board's layout and the watched
// game's picture: Column changed with the pad in a room goes out at once in the
// room's own request, once the runtime's snapshot holds it the next change goes
// out too, and Picture goes out the same way.
void RoomLayoutSavesInRoom() {
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,19);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;
 room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
 h.Frame();
 Check(!h.view.canEditPreferences,"A joined room left the player's settings open");
 const auto roomSaves=[&](std::size_t from){
  std::vector<commands::RoomSettings> sent;
  for(auto i=from;i<h.actions.size();++i)if(const auto* save=As<commands::RoomSettings>(h.actions[i]))sent.push_back(*save);
  return sent;
 };
 const auto lockedSave=[&](std::size_t from){
  return std::any_of(h.actions.begin()+static_cast<std::ptrdiff_t>(from),h.actions.end(),[](const ShellAction& a){return Is<commands::SavePreferences>(a);});
 };
 h.Screen("tab-interface/room-layout");h.FocusOn("arrange-column");
 const auto first=h.actions.size();
 h.Press(MenuInput::Right);h.Wait(1);h.Frame();
 auto sent=roomSaves(first);
 Check(!lockedSave(first),"A layout change in a room asked for the locked settings' save");
 Check(sent.size()==1&&sent[0].preferences.roomColumn==netplay::DefaultRoomColumn+netplay::RoomColumnStep,
  "Column changed in a room was not saved by the room's own request");
 // The runtime takes it and publishes it, which acknowledges the save.
 h.view.preferences=sent[0].preferences;h.Frame();
 const auto second=h.actions.size();
 h.Press(MenuInput::Right);h.Wait(1);h.Frame();
 sent=roomSaves(second);
 Check(!lockedSave(second)&&sent.size()==1&&sent[0].preferences.roomColumn==netplay::DefaultRoomColumn+2*netplay::RoomColumnStep,
  "Once the snapshot held the first change, the next one in the room was not saved");
 // The watched game's picture is one of them too (ShellView::CanEditRoomSettings):
 // every Room layout row is open in the room, and Picture goes out the same way.
 h.view.preferences=sent[0].preferences;
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 h.Frame();
 const auto open=[&](const char* id){
  const auto e=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});
  return e!=rows.end()&&e->enabled;
 };
 for(const char* id:{"arrange-column","arrange-split","arrange-preset","arrange-members","arrange-chat","arrange-tables","arrange-pictures","chat-filter","arrange-picture","arrange-reset"})
  Check(open(id),"A Room layout row was locked in a room");
 h.FocusOn("arrange-picture");
 const auto third=h.actions.size();
 h.Press(MenuInput::Right);h.Wait(1);h.Frame();
 sent=roomSaves(third);
 Check(!lockedSave(third)&&sent.size()==1&&sent[0].preferences.pictureMode==netplay::PictureBig&&
  sent[0].preferences.roomColumn==netplay::DefaultRoomColumn+2*netplay::RoomColumnStep,"Picture changed in a room was not saved by the room's own request");
 // Pictures: Characters by default; Left turns it to Portraits, saved the same way.
 h.view.preferences=sent[0].preferences;h.Frame();
 Check(sent[0].preferences.roomCharacterPictures,"Pictures did not default to Characters");
 h.FocusOn("arrange-pictures");
 const auto fourth=h.actions.size();
 h.Press(MenuInput::Left);h.Wait(1);h.Frame();
 sent=roomSaves(fourth);
 Check(!lockedSave(fourth)&&sent.size()==1&&!sent[0].preferences.roomCharacterPictures&&sent[0].preferences.pictureMode==netplay::PictureBig,
  "Pictures changed in a room was not saved by the room's own request");
 // Matches names the same picture: its rows are open there, the rest of the panel is not.
 h.view.preferences=sent[0].preferences;h.Screen("tab-matches/training-replays");h.Frame();
 Check(open("watched-picture")&&open("picture-corner")&&open("picture-size")&&!open("replay-share"),
  "The watched picture's rows on Matches did not follow the room's open settings");
 SetMenuEntriesProbe({});
}
static void RoomBoardJourneys() {
 using namespace sf4e;
 Harness h;h.Frame();
 SetMenuGlyphs(3,0x40000,0x20000);h.Frame();
 // Outside a room Back at Home still hides Ember, and sends nothing.
 {const auto before=h.actions.size();h.Press(MenuInput::Back);Check(!h.open&&h.shell.Navigation().Screen()=="home"&&h.actions.size()==before,"Back on an idle Home did not hide Ember");h.open=true;}
 h.Screen("player");h.view.inputCapture=input::Capture::ReleaseAll;h.Frame();
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="assignment"&&Is<commands::Input>(h.actions.back(),[](const commands::Input& i){return i.inputAction==input::Action::Cancel;}),"Assignment Back did not only cancel capture");
 h.view.inputCapture=input::Capture::Idle;h.Frame();Check(h.shell.Navigation().Screen()=="player","Assignment lost return destination");
 h.Screen("home");h.Choose("online");Check(h.shell.Navigation().Screen()=="online","Online route");
 // The Online screen tells the player, for information only, which relay this PC uses
 // and how its network treats a direct connection. The rows take focus but Select does nothing.
 {
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
  h.Frame();
  Check(row("relay")!=rows.end()&&row("network")!=rows.end()&&row("relay")->info&&row("network")->info,"Online lacks the relay and network rows");
  Check(row("relay")->value==loc::T("connection.checking")&&row("network")->value==loc::T("connection.checking"),"Unreported network is not shown as checking");
  h.view.netReport.reported=true;h.view.netReport.relay="euc1";h.view.netReport.relayConnected=true;h.view.netReport.nat=NatClass::Strict;h.Frame();
  Check(row("relay")->value==loc::Tf("network.relay_connected",loc::T("network.region_euc1"))&&row("network")->value==loc::T("network.nat_strict")&&
   row("network")->detail==loc::T("network.nat_detail_strict"),"Online shows the wrong relay or network class");
  h.view.netReport.relayConnected=false;h.view.netReport.nat=NatClass::NoUdp;h.view.netReport.captivePortal=true;h.Frame();
  Check(row("relay")->value==loc::Tf("network.relay_connecting",loc::T("network.region_euc1"))&&row("network")->value==loc::T("network.nat_no_udp")&&
   row("network")->detail.find(loc::T("network.captive_portal"))!=std::string::npos,"A disconnected relay or captive portal is not shown");
  h.view.controllerReady=h.view.canOpenRoom=false;h.Frame();
  Check(!row("create")->enabled&&row("create")->quiet&&row("create")->detail==loc::T("room.controller_required")&&row("public-rooms")->detail==loc::T("room.controller_required"),"Online rows without a controller do not ask for one");
  h.view.controllerReady=true;h.view.network=netplay::NetworkAvailability::Starting;h.Frame();
  Check(!row("join")->quiet&&row("join")->detail==loc::T("online.join_detail"),"Online rows blame networking while it is still starting");
  h.view.network=netplay::NetworkAvailability::Unavailable;h.Frame();
  Check(row("join")->quiet&&row("join")->detail==loc::T("runtime.networking_unavailable"),"Online rows without networking do not say so");
  h.view.network=netplay::NetworkAvailability::Ready;h.view.canOpenRoom=true;h.Frame();
  const auto actions=h.actions.size();h.Choose("network");Check(h.actions.size()==actions&&h.shell.Navigation().Screen()=="online","An information row acted on Select");
  h.view.netReport=NetworkSummary{};SetMenuEntriesProbe({});
 }
 h.Choose("create");h.Choose("host");Check(Is<commands::HostRoom>(h.actions.back()),"Create journey");
 // Opening a room keeps the player on Create with a Cancel; the room screen
 // appears only once the committed snapshot says the room is joined.
 // The controller records that the room being opened is hosted, so Back
 // from Home returns to Create, not Join.
 h.Frame(0,5);h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Opening;h.view.session.isHost=true;h.Frame();
 Check(h.shell.Navigation().Screen()=="create","Opening room showed the placeholder room screen");
 h.Screen("home");h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="create","Back from Home while creating did not return to Create");
 // Create's opening says Creating, on its own screen and on Home's status and
 // Online row; Join's says Joining (below).
 std::string openingStatus;std::vector<MenuEntry> openingRows;
 SetMenuStatusProbe([&](const char* status,Tone){openingStatus=status;});
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){openingRows=rows;});
 const auto onlineDetail=[&]{const auto it=std::find_if(openingRows.begin(),openingRows.end(),[](const MenuEntry& e){return e.id=="online";});
  return it==openingRows.end()?std::string():it->detail;};
 h.Frame();Check(openingStatus==loc::T("room.creating_status"),"Creating a room is not reported as creating");
 h.Screen("home");Check(openingStatus==loc::T("room.creating_status")&&onlineDetail()==loc::T("room.creating_status"),
  "Home does not say the room is being created");
 h.Press(MenuInput::Back);
 h.Choose("cancel-open");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(Is<commands::LeaveRoom>(h.actions.back()),"Cancel while opening did not leave the room");
 h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;h.view.room.roomEpoch=9;h.view.room.localMember=1;h.Frame();
 Check(h.shell.Navigation().Screen()=="room","Joined room did not open the room screen");
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.view.room.roomEpoch=0;h.view.room.localMember=0;h.Frame();h.Screen("home");
 h.Screen("join");h.Choose("invite-text");ImGui::GetIO().AddInputCharactersUTF8("sf4://invitation");h.Frame();
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 h.Choose("join-now");Check(Is<commands::JoinRoom>(h.actions.back(),[](const commands::JoinRoom& join){return join.invitation=="sf4://invitation";}),"Join draft journey");
 h.Frame(0,5);h.view.session.room=netplay::RoomState::Opening;h.view.session.isHost=false;h.Frame();
 Check(h.shell.Navigation().Screen()=="join"&&openingStatus==loc::T("room.joining_status"),"Joining a room is not reported as joining");
 h.Screen("home");Check(openingStatus==loc::T("room.joining_status")&&onlineDetail()==loc::T("room.joining_status"),
  "Home does not say the room is being joined");
 // A host the runtime refused never leaves Idle. A later join that no click
 // started (a Discord invitation) is still a join, on every screen that says so.
 h.view.session.room=netplay::RoomState::Idle;h.Screen("create");h.Choose("host");
 h.view.error="refused";h.Frame(0,3);h.view.error.clear();
 h.view.session.room=netplay::RoomState::Opening;h.view.session.isHost=false;h.Frame();
 h.Screen("home");Check(openingStatus==loc::T("room.joining_status")&&onlineDetail()==loc::T("room.joining_status"),
  "A refused host made a later join read as creating");
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="join","Back from Home during a Discord join did not return to Join");
 h.view.session.room=netplay::RoomState::Idle;h.Frame();
 SetMenuStatusProbe({});SetMenuEntriesProbe({});
 JoinedRoom(h,10);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;
 h.view.room.host=1;h.view.room.name="Test room";h.view.room.revision=3;
 for(auto& table:h.view.room.tables)table.revision=7;
 room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";h.view.room.members.push_back(peer);
 // One table per line: Down goes to the next table.
 h.Frame();h.Screen("room");h.Press(MenuInput::Down);Check(h.shell.Navigation().Focus()=="table-1","Down did not reach the next table");
 // With Tables set to Grid the cards move as the grid they are drawn in:
 // Right goes to the card beside, and past the last column to the members.
 h.Press(MenuInput::Up);Check(h.shell.Navigation().Focus()=="table-0","Up did not return to the first table");
 h.view.preferences.roomTableGrid=true;h.Frame();h.Frame();
 h.Press(MenuInput::Right);Check(h.shell.Navigation().Focus()=="table-1","Right did not reach the card beside");
 h.Press(MenuInput::Right);Check(h.shell.Navigation().Focus()=="member-1","Right did not enter member pane");
 h.Press(MenuInput::Down);Check(h.shell.Navigation().Focus()=="member-2","Member focus order changed");
 h.view.room.members.pop_back();h.Frame();Check(h.shell.Navigation().Focus()=="room-members","Disappeared member did not choose nearest entry");
 h.view.room.members.push_back(peer);h.Press(MenuInput::Left);h.Screen("room");
 h.view.preferences.mainFighter=8;h.view.selectedPick.fighter=0;
 h.view.room.members[1].mainFighter=9;h.view.room.members[1].fighter=2;
 h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;
 // The Members list shows each member's saved main, not the fighter they battle with.
 h.Screen("room-members");
 std::vector<int> portraits;SetPortraitProbe([&](const FighterPicture& picture,ImVec2,ImVec2){portraits.push_back(picture.fighter);});
 h.Frame();SetPortraitProbe({});
 Check(portraits==std::vector<int>({8,9}),"Member portraits must use saved mains, not battle fighters");
 h.Screen("room");
 // Your own seat shows your pick's costume, colour and Ultra at once, before
 // Ready shares them; a Random pick names the fighter it rolled.
 h.view.selectedPick.costume=1;h.view.selectedPick.color=3;h.view.selectedPick.ultra=1;
 std::vector<FighterPicture> seats;SetPortraitProbe([&](const FighterPicture& picture,ImVec2,ImVec2){seats.push_back(picture);});
 h.Frame();SetPortraitProbe({});
 Check(std::any_of(seats.begin(),seats.end(),[](const FighterPicture& p){return p.fighter==0&&p.art==FighterArt::Character&&p.costume==1&&p.color==3;}),
  "Your own seat did not show your pick's costume and colour before Ready");
 const auto ownLook=SeatLookOf(h.view,h.view.room.members[0]);
 Check(SeatCaption(ownLook)==loc::Tf("room.seat_look",selection::FindFighter(0)->name,ShortCostumeLabel(h.view.selectedPick),UltraLabel(1)),
  "Your own seat's line did not name your pick's costume and Ultra");
 h.view.selectedRandom=true;
 Check(SeatCaption(SeatLookOf(h.view,h.view.room.members[0])).rfind(loc::Tf("selection.random_fighter",selection::FindFighter(0)->name),0)==0,
  "Your own Random seat did not name the fighter it rolled");
 h.view.selectedRandom=false;h.view.selectedPick=selection::Pick{};
 h.view.room.tables[0].p1=h.view.room.tables[0].p2=0;
 // A on an empty table opens the seat chooser: P1 left, P2 right, B cancels.
 h.Choose("table-2");Check(h.shell.Navigation().Choosing(),"A on an empty table did not open the seat chooser");
 auto before=h.actions.size();h.Press(MenuInput::Back);
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==before&&h.shell.Navigation().Screen()=="room","B did not cancel the seat chooser");
 h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Queue&&sent.roomAction.table==2&&
  sent.roomAction.seat==1;}),"Seat chooser did not take the P2 seat");
 // P1 taken under a chooser highlighting P2: the chooser closes, the A pressed
 // on that same frame does nothing, and the next A offers the new options.
 h.Choose("table-1");h.Press(MenuInput::Right);h.view.room.tables[1].p1=2;
 auto raced=h.actions.size();h.Frame(MenuInput::Select);h.Frame();
 Check(h.actions.size()==raced&&!h.shell.Navigation().Choosing(),"A changed chooser sent an option the player never saw");
 h.Press(MenuInput::Select);h.Press(MenuInput::Select);
 Check(h.actions.size()==raced+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Queue&&sent.roomAction.seat==1;}),
  "The reopened chooser did not offer the open P2 seat first");
 h.view.room.tables[1].p1=0;
 // The mouse picks from the chooser with real buttons: a notice over it
 // blocks them, and a click on another table's card opens that table's
 // chooser even though its game count differs.
 std::map<std::string,ImVec2> centres;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){centres[id]=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);});
 const auto click=[&](const std::string& id){
  h.Frame();const auto at=centres.at(id);auto& io=ImGui::GetIO();io.AddMousePosEvent(at.x,at.y);h.Frame();
  io.AddMouseButtonEvent(0,true);h.Frame();io.AddMouseButtonEvent(0,false);h.Frame();h.Frame();
 };
 h.view.room.tables[3].matchGeneration=5;h.Choose("table-2");h.Press(MenuInput::Back);
 click("table-3");Check(h.shell.Navigation().Choosing()&&h.shell.Navigation().DialogId()=="table-3","Clicking another table did not keep its chooser open");
 h.view.readyFailure="Notice over the chooser.";h.view.readyFailureSequence=7;raced=h.actions.size();
 click("table-3/1");Check(h.actions.size()==raced&&h.shell.Navigation().Choosing(),"A click reached the chooser under a notice");
 h.Press(MenuInput::Select);
 // A cursor resting on P1 does not undo P2 picked on the pad.
 {const auto at=centres.at("table-3/0");ImGui::GetIO().AddMousePosEvent(at.x,at.y);h.Frame();h.Frame();}
 h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==raced+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.seat==1;}),"A resting cursor overrode the pad's choice");
 // The right mouse button cancels a choice, sending nothing.
 click("table-3");Check(h.shell.Navigation().Choosing(),"Clicking a table did not open its chooser");
 raced=h.actions.size();
 {auto& io=ImGui::GetIO();io.AddMouseButtonEvent(1,true);h.Frame();io.AddMouseButtonEvent(1,false);h.Frame();h.Frame();}
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==raced&&h.shell.Navigation().Screen()=="room","The right mouse button did not cancel the chooser");
 click("table-3");click("table-3/1");
 Check(h.actions.size()==raced+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.table==3&&sent.roomAction.seat==1;}),"Clicking P2 in the chooser did not take it");
 ImGui::GetIO().AddMousePosEvent(-1,-1);
 SetMenuCardProbe({});h.view.room.tables[3].matchGeneration=0;
 // A full table offers the queue or watching instead.
 h.view.room.tables[3].p1=3;h.view.room.tables[3].p2=4;h.view.room.tables[3].phase=room::TablePhase::Playing;
 h.Choose("table-3");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Watch&&sent.roomAction.table==3;}),"Full-table chooser did not watch");
 h.view.room.tables[3].p1=h.view.room.tables[3].p2=0;h.view.room.tables[3].phase=room::TablePhase::Idle;
 // Y opens the table's options; the list still queues and watches.
 h.Screen("room");h.Choose("table-2");h.Press(MenuInput::Back);h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Y did not open the table options");
 h.Choose("queue");Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Queue&&sent.roomAction.table==2;}),"Table queue journey");
 h.Choose("watch");Check(RoomActionIs(h.actions.back(),room::ActionKind::Watch),"Watch journey");
 h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="room","Y again did not return to the board");
 h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room-chat","View did not open chat");
 h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room","View again did not return to the board");
 // X opens the real fighter selector, which hands its shortcuts back: X
 // there returns to the board and View goes on to chat.
 FighterSelector selector;selection::Pick pick;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="selection","X did not open fighter selection");
 // The selector names the board as its way back and shows the room's shortcuts.
 Check(EmbeddedReturnContext().exitName=="Room"&&EmbeddedReturnContext().shortcutHints.size()==3,
  "The fighter selector does not know it returns to the room or which shortcuts it forwards");
 // X opens the selector's board on the fighter, to change it; leaving it on
 // a sub-page and coming back by X opens the board again.
 Check(selector.Navigation().Screen()=="home"&&selector.Navigation().Focus().compare(0,8,"fighter-")==0,"X did not open the selector on the fighter");
 selector.Navigation().Home();selector.Navigation().Push("options");h.Frame();
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="room","X on a selector sub-page did not return to the board");
 h.Press(MenuInput::Fighter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home","X reopened the selector on the sub-page it was left on");
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="room","X in fighter selection did not return to the board");
 h.Press(MenuInput::Fighter);h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room-chat","View in fighter selection did not open chat");
 h.Press(MenuInput::Chat);h.selection=[]{};h.view.canEditSelection=false;
 // B on the board with no seat goes to Home, and B there does nothing rather
 // than dropping to the game's own menu.
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="home","B on the board did not reach Home");
 // Back at Home in a room does nothing: Ember stays up, the room is kept and
 // nothing is sent, because the game's own menu behind it is locked.
 h.Frame();
 raced=h.actions.size();h.Press(MenuInput::Back);
 Check(h.open&&h.shell.Navigation().Screen()=="home"&&h.actions.size()==raced&&h.view.session.room==netplay::RoomState::Joined,
  "B at Home in a room hid Ember, left Home, or sent a command");
 h.Screen("room");
 h.Choose("table-2");h.Press(MenuInput::Back);h.Press(MenuInput::Options);
 h.view.room.members[0].table=2;h.view.room.members[0].seat=0;h.view.room.tables[2].p1=1;h.view.room.tables[2].p2=2;
  h.view.room.tables[2].phase=room::TablePhase::Waiting;h.view.canReady=true;
 std::vector<MenuEntry> tableRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){tableRows=rows;});
 h.view.canReady=false;h.view.canEditSelection=false;h.view.room.tables[2].p2=0;h.Frame();
 const auto row=[&](const char* id)->const MenuEntry&{return *std::find_if(tableRows.begin(),tableRows.end(),[&](const MenuEntry& e){return e.id==id;});};
 Check(row("ready").detail.find("opponent")!=std::string::npos,"Ready does not explain the missing opponent");
 Check(row("selection").detail.find("Unready")==std::string::npos,"Unready instruction shown to an unready player");
 Check(!row("selection").enabled,"Locked fighter change pretends to be available");
 h.view.canEditSelection=true;h.Frame();Check(row("selection").enabled,"Waiting solo player cannot change fighter");
  h.view.room.tables[2].p2=2;h.view.room.tables[2].phase=room::TablePhase::Playing;
 h.view.room.tables[2].ready[0]=h.view.room.tables[2].ready[1]=true;
 std::string tableStatus;SetMenuStatusProbe([&](const char* status,Tone){tableStatus=status;});
 h.view.session.match=netplay::MatchState::PostMatch;h.view.canEditSelection=false;h.Frame();
 // The finished game's bookkeeping (result, receipt, drain) is the runtime's
 // job: the player sees one Ready for rematch control and presses it once.
 Check(row("ready").label=="Ready for rematch"&&row("ready").enabled,"Finished match hides Ready for rematch behind the result wait");
 Check(row("selection").detail.find("Unready")==std::string::npos,"Finished match incorrectly asks the player to Unready");
 Check(tableStatus.find("READY")==std::string::npos,"Post-match footer falsely reports READY");
 Check(tableStatus.find("Waiting for results")==std::string::npos,"Post-match footer exposes the result wait");
 const auto postMatchActions=h.actions.size();h.Choose("ready");
 Check(h.actions.size()==postMatchActions+1&&ReadyIs(h.actions.back(),true),"Pending result dropped the rematch press");
 // A Ready the runtime still holds (while the last game closes) is taken
 // back with Unready, which does not wait for the last game's receipt.
 h.view.readyPress=netplay::ReadyPress::Held;h.view.room.localTerminalPending=true;h.Frame();
 Check(row("ready").label=="Unready / unlock fighter"&&row("ready").enabled,"In-flight Ready cannot be taken back");
 Check(tableStatus.find("Readying up")!=std::string::npos,"In-flight Ready is not shown on the seat line");
 h.Choose("ready");
 Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Unready&&sent.roomAction.seat==0;}),"Unready of an in-flight Ready was refused");
 // A Ready already sent waits for the room, and offers no second press.
 h.view.readyPress=netplay::ReadyPress::Sent;h.Frame();
 Check(row("ready").label=="Readying up..."&&!row("ready").enabled,"A sent Ready still offers a second press");
 h.view.room.localTerminalPending=false;
 h.view.readyPress=netplay::ReadyPress::None;
 h.view.readyFailure="Your Ready did not go through.";h.view.readyFailureSequence=1;h.Frame();
 const auto beforeNotice=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==beforeNotice,"Dismissing the failure notice activated the focused row");
 h.Press(MenuInput::Select);
 Check(h.actions.size()==beforeNotice+1,"Ready unavailable after the failure notice was dismissed");
 h.view.room.tables[2].phase=room::TablePhase::Paused;h.Frame();
 Check(row("ready").label=="Result unresolved"&&!row("ready").enabled,"Unresolved result presented as Ready");
 SetMenuStatusProbe({});
 h.view.room.tables[2].phase=room::TablePhase::Waiting;
 h.view.room.tables[2].ready[0]=h.view.room.tables[2].ready[1]=false;
 h.view.session.match=netplay::MatchState::None;h.view.canEditSelection=true;
 SetMenuEntriesProbe({});h.view.room.tables[2].p2=2;h.view.canReady=true;
 h.Choose("ready");Check(ReadyIs(h.actions.back()),"Ready journey");
 h.view.room.tables[2].ready[0]=true;h.Choose("ready");Check(RoomActionIs(h.actions.back(),room::ActionKind::Unready),"Unready journey");
 // On the board, A on your own seat readies and unreadies, and one B leaves
 // the seat, ready or not. While the game is starting B takes Ready back
 // instead, and the legend says which.
 h.Screen("room");h.view.room.tables[2].ready[0]=false;h.Choose("table-2");
 Check(ReadyIs(h.actions.back()),"A on your seat did not ready");
 h.view.room.tables[2].ready[0]=true;h.Press(MenuInput::Select);
 Check(RoomActionIs(h.actions.back(),room::ActionKind::Unready),"A again did not unready");
 h.Press(MenuInput::Back);Check(Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Unqueue&&sent.roomAction.table==2;})&&
  h.shell.Navigation().Screen()=="room","One B did not leave a readied seat");
 // Leaving a seat that holds a set score, or that passes to the queue, asks
 // first, and the answer starts on staying.
 auto& seatTable=h.view.room.tables[2];
 seatTable.score[0]=2;seatTable.score[1]=1;raced=h.actions.size();
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Choosing()&&h.shell.Navigation().DialogId()=="table-2"&&h.shell.Navigation().ChoiceIndex()==0&&h.actions.size()==raced,
  "B on a seat with a set score did not ask before leaving");
 h.Press(MenuInput::Select);
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==raced&&h.shell.Navigation().Screen()=="room","The leave question did not start on staying");
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Choosing(),"B did not ask again");
 h.Press(MenuInput::Back);Check(!h.shell.Navigation().Choosing()&&h.actions.size()==raced,"B did not cancel the leave question");
 h.Press(MenuInput::Back);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==raced+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Unqueue&&sent.roomAction.table==2;}),
  "Confirming the leave question did not release the seat");
 seatTable.score[0]=seatTable.score[1]=0;seatTable.queue={3};raced=h.actions.size();
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Choosing()&&h.actions.size()==raced,"B on a seat with someone queued did not ask before handing it over");
 h.Press(MenuInput::Back);Check(!h.shell.Navigation().Choosing()&&h.actions.size()==raced,"B did not cancel the handover question");
 // The options screen's Leave seat row asks the same way, in its own dialog.
 h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Y did not open the seat's options");
 h.Choose("unqueue");Check(h.shell.Navigation().Confirming()&&!h.shell.Navigation().ConfirmSelected()&&h.actions.size()==raced,
  "Leave seat with someone queued did not ask on the options screen");
 h.Press(MenuInput::Back);h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="room","Y again did not return to the board");
 seatTable.queue.clear();
 // A that cannot ready, unready or rematch names no action: the card keeps its
 // state on the strip and A does nothing.
 std::vector<MenuEntry> boardRows;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){boardRows=rows;});
 const auto seatCard=[&]()->const MenuEntry&{return *std::find_if(boardRows.begin(),boardRows.end(),[](const MenuEntry& e){return e.id=="table-2";});};
 seatTable.ready[0]=false;h.Frame();
 Check(!seatCard().info&&seatCard().hint==loc::T("room.ready_up"),"A seat that can ready lost its Ready action");
 seatTable.p2=0;h.Frame();
 Check(seatCard().info&&seatCard().hint==loc::T("room.ready_up"),"A seat with no opponent offered a Ready that cannot be sent");
 seatTable.p2=2;
 for(const auto phase:{room::TablePhase::Ready,room::TablePhase::Playing,room::TablePhase::Paused}){
  seatTable.phase=phase;h.Frame();
  Check(seatCard().info&&!seatCard().hint.empty(),"A seat with nothing to send still offered an action");
  h.Frame();
  raced=h.actions.size();h.Press(MenuInput::Select);
  Check(h.actions.size()==raced,"A on a seat with nothing to send sent something");
 }
 seatTable.phase=room::TablePhase::Waiting;seatTable.ready[0]=true;
 SetMenuEntriesProbe({});SetMenuTextProbe({});
 // The start hold: A and B both take Ready back, so one press of B always
 // gets a player out of a start they did not want. B is labelled for that.
 std::string boardStatus;SetMenuStatusProbe([&](const char* status,Tone){boardStatus=status;});
 const std::string finishFirst=loc::T("room.leave_seat.finish_first");
 h.view.room.tables[2].ready[1]=true;h.view.room.tables[2].phase=room::TablePhase::Ready;h.view.room.tables[2].spectatorHold=true;
 raced=h.actions.size();h.Press(MenuInput::Back);
 Check(h.actions.size()==raced+1&&RoomActionIs(h.actions.back(),room::ActionKind::Unready)&&h.shell.Navigation().Screen()=="room",
  "B while the start is held did not unready, or left the board");
 h.Press(MenuInput::Select);Check(RoomActionIs(h.actions.back(),room::ActionKind::Unready),"A while the start is held did not unready");
 h.view.room.tables[2].spectatorHold=false;raced=h.actions.size();h.Press(MenuInput::Back);
 Check(h.actions.size()==raced&&h.shell.Navigation().Screen()=="room","B during a starting game left the seat or the board");
 // A refusal names its screen, its cause and a few seconds: it lapses on its
 // own, ends when what refused it does, and does not follow the player.
 Check(boardStatus==finishFirst,"B during a starting game gave no reason");
 h.Frame(0,240);Check(boardStatus==finishFirst,"A refusal lapsed before its time");
 h.Frame(0,90);Check(boardStatus.find(finishFirst)==std::string::npos,"A refusal outlived its time");
 h.Press(MenuInput::Back);Check(boardStatus==finishFirst,"B during a starting game gave no reason again");
 h.view.room.tables[2].phase=room::TablePhase::Waiting;h.view.room.tables[2].ready[0]=h.view.room.tables[2].ready[1]=false;h.Frame();
 Check(boardStatus.find(finishFirst)==std::string::npos,"A refusal outlived what refused it");
 // X on a readied seat says to unready, not that the runtime is waiting; in a
 // closed room it says the room is closed, to a fighter who is not ready.
 h.view.room.tables[2].ready[0]=true;h.view.canEditSelection=false;
 h.view.selectionLockReason="Waiting for the current match or selection update to finish.";
 h.Press(MenuInput::Fighter);
 Check(h.shell.Navigation().Screen()=="room"&&boardStatus==loc::T("room.change_fighter.unready"),"X on a readied seat gave the runtime's reason instead of Unready");
 h.view.room.tables[2].ready[0]=false;h.view.canEditSelection=true;h.view.selectionLockReason.clear();h.view.room.closed=true;
 h.Press(MenuInput::Fighter);
 Check(h.shell.Navigation().Screen()=="room"&&boardStatus.find("closed")!=std::string::npos&&boardStatus.find("Unready")==std::string::npos,
  "X in a closed room told a fighter who is not ready to unready");
 h.view.room.closed=false;h.Frame();
 h.view.room.tables[2].phase=room::TablePhase::Ready;h.Press(MenuInput::Back);Check(boardStatus==finishFirst,"B during a starting game gave no reason a third time");
 // B away from your own card is the ordinary Back: Home, keeping the seat.
 h.Press(MenuInput::Right);raced=h.actions.size();h.Press(MenuInput::Back);
 Check(h.actions.size()==raced&&h.shell.Navigation().Screen()=="home","B off your own card gave up the seat instead of going Home");
 Check(boardStatus.find(finishFirst)==std::string::npos,"A refusal followed the player to Home");
 h.view.room.tables[2].phase=room::TablePhase::Waiting;
 SetMenuStatusProbe({});
 h.Press(MenuInput::Back);Check(h.open&&h.shell.Navigation().Screen()=="home","B at Home in a room hid Ember");
 h.Screen("room");
 // Walking past other tables to the bar's Table options still opens your own table's options.
 h.FocusOn("table-2");h.Choose("options");
 std::vector<MenuEntry> optionRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){optionRows=rows;});h.Frame();SetMenuEntriesProbe({});
 Check(h.shell.Navigation().Screen()=="room-table"&&std::any_of(optionRows.begin(),optionRows.end(),[](const MenuEntry& e){return e.id=="ready";}),
  "Table options opened a table the player only passed");
 // A on another table's card opens its options while you keep your seat: its
 // rules, and a host's recovery for its unresolved result, by pad or mouse.
 h.Screen("room");h.view.room.tables[0].phase=room::TablePhase::Paused;raced=h.actions.size();
 h.Choose("table-0");SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){optionRows=rows;});h.Frame();SetMenuEntriesProbe({});
 const auto hasRow=[&](const char* id){return std::any_of(optionRows.begin(),optionRows.end(),[&](const MenuEntry& e){return e.id==id;});};
 // The host sees that table's rules on the same page.
 Check(h.shell.Navigation().Screen()=="room-table"&&hasRow("rounds")&&hasRow("cancel-result")&&!hasRow("ready"),
  "A on another table did not open its options");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="room"&&h.actions.size()==raced,"Looking at another table sent a room action");
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){centres[id]=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);});
 click("table-0");SetMenuCardProbe({});ImGui::GetIO().AddMousePosEvent(-1,-1);
 Check(h.shell.Navigation().Screen()=="room-table"&&h.actions.size()==raced,"Clicking another table did not open its options");
 h.view.room.tables[0].phase=room::TablePhase::Idle;
 // A watcher reaches another table's options through the chooser's last
 // option, keeping the watch.
 h.view.room.members[0].seat=-1;h.view.room.tables[2].p1=0;h.view.room.tables[2].spectators={1};
 h.view.room.tables[0].phase=room::TablePhase::Paused;h.Screen("room");raced=h.actions.size();
 h.Choose("table-0");Check(h.shell.Navigation().Choosing(),"A watcher's A on another table did not open its chooser");
 h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){optionRows=rows;});h.Frame();SetMenuEntriesProbe({});
 Check(h.shell.Navigation().Screen()=="room-table"&&hasRow("rounds")&&hasRow("cancel-result")&&h.actions.size()==raced,
  "A watcher could not look at another table's options");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="room"&&h.actions.size()==raced&&h.view.room.tables[2].spectators.size()==1,"Looking at another table changed the watch");
 h.view.room.tables[0].phase=room::TablePhase::Idle;h.view.room.tables[2].spectators.clear();
 h.view.room.members[0].seat=0;h.view.room.tables[2].p1=1;
 h.Screen("room");h.FocusOn("table-2");
 // A queued member's B leaves the queue.
 h.view.room.members[0].seat=-1;h.view.room.tables[2].p1=3;h.view.room.tables[2].queue={1};h.Press(MenuInput::Back);
 Check(RoomActionIs(h.actions.back(),room::ActionKind::Unqueue)&&h.shell.Navigation().Screen()=="room","B did not leave the queue");
 h.view.room.members[0].seat=0;h.view.room.tables[2].p1=1;h.view.room.tables[2].queue.clear();
 h.Screen("room-table");
 h.view.room.tables[2].ready[0]=false;h.view.session.match=netplay::MatchState::PostMatch;
 h.Choose("ready");Check(ReadyIs(h.actions.back(),true),"Rematch journey");
 h.view.room.tables[2].phase=room::TablePhase::Paused;h.Choose("cancel-result");
 auto count=h.actions.size();++h.view.room.tables[2].matchGeneration;h.Frame();h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==count,"Stale confirmation cancelled a different game");h.Press(MenuInput::Back);
 h.Screen("room");h.Choose("leave");count=h.actions.size();h.Press(MenuInput::Select);Check(h.actions.size()==count,"Leave default was not Cancel");
 h.Choose("leave");h.Press(MenuInput::Right);h.Press(MenuInput::Select);Check(h.actions.size()==count+1&&Is<commands::LeaveRoom>(h.actions.back()),"Confirmed leave");
  // Kick, third of a member's actions, asks first on the member's page.
  h.Screen("room");h.Choose("room-members");h.Choose("member-2");h.Press(MenuInput::Down);h.Press(MenuInput::Down);h.Press(MenuInput::Select);h.Frame(0,2);
  h.view.room.members.pop_back();h.Frame();h.Press(MenuInput::Right);h.Press(MenuInput::Select);Check(Is<commands::LeaveRoom>(h.actions.back()),"Removed member was kicked");
 h.Screen("room");h.Choose("room-chat");h.Frame(0,4);ImGui::GetIO().AddInputCharactersUTF8("Hello");h.Frame();
 count=h.actions.size();h.Press(MenuInput::Down);Check(h.actions.size()==count,"Scrolling the chat sent it");
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 Check(h.actions.size()==count+1&&Is<commands::RoomAction>(h.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Chat&&sent.roomAction.text=="Hello";}),"Enter in the chat box did not send");
  h.view.session.control=netplay::Health::Lost;h.Screen("room");count=h.actions.size();h.Choose("table-2");Check(h.actions.size()==count,"Lost connection submitted Ready");
 h.view={};h.view.controllerReady=h.view.canEditPreferences=h.view.canOpenRoom=h.view.lobbyMusicAvailable=true;h.Frame();h.Screen("tab-interface/match-hud");h.FocusOn("hud");h.Press(MenuInput::Left);
 count=h.actions.size();h.Frame(0,20);Check(h.actions.size()==count,"Autosave not coalesced");
 h.Frame(0,20);Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return !s.preferences.showMatchHud;}),"Autosave did not queue");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Screen("player");h.FocusOn("background-play");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.backgroundPlay;}),"Play in the background did not save");
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.endlessLobbyMusic;}),"Endless lobby music was not on by default");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Screen("tab-interface/sounds");h.FocusOn("lobby-music");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return !s.preferences.endlessLobbyMusic;}),"Turning off endless lobby music did not save");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Screen("player");h.FocusOn("faster-display");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return !s.preferences.fasterDisplay&&s.preferences.backgroundPlay;}),"Faster display did not save off");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Screen("player");h.FocusOn("borderless");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.borderless&&!s.preferences.fasterDisplay;}),"Borderless did not save on");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Screen("tab-matches/training-replays");h.FocusOn("replay-save-watched");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return !s.preferences.recordWatched;}),"Save matches you watch did not save");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();
 // Show my Steam profile to room members: on until the player turns it off, saved with the other preferences.
 Check(h.view.preferences.shareSteamProfile,"Steam profile sharing was not on by default");
 h.Screen("profile");h.FocusOn("share-steam");
 h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return !s.preferences.shareSteamProfile;}),"Hiding the Steam profile did not save");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.shareSteamProfile;}),"Showing the Steam profile did not save");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Screen("tab-interface/match-hud");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.FocusOn("hud-size");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudSize==1;}),"HUD size did not save"); // Small by default; Right steps to Standard.
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.FocusOn("hud-layout");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudLayout==0;}),"HUD layout did not save"); // Split by default; Left steps to the Ember strip.
 {std::vector<MenuEntry> stripRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){stripRows=rows;});h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();SetMenuEntriesProbe({});
  const auto offset=std::find_if(stripRows.begin(),stripRows.end(),[](const MenuEntry& e){return e.id=="hud-name-offset";});
  Check(offset!=stripRows.end()&&!offset->enabled,"Name height is offered for the Ember strip, which has no name plates");}
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudLayout==1;}),"HUD layout did not step back to Split");
 // Name height: Left moves the plates up two units at a time, to the limit and no further, and Right back down.
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.FocusOn("hud-name-offset");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudNameOffset==-2;}),"Name height did not save");
 for(int i=0;i<40;++i){h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);}
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudNameOffset==-netplay::MaxMatchHudNameOffset;}),"Name height went past its limit");
 for(int i=0;i<30;++i){h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);}
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudNameOffset==0;}),"Name height did not step back to Default");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.FocusOn("hud-position");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudAnchor==1;}),"HUD position did not save"); // Bottom center by default; Right steps to Bottom left.
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudAnchor==0;}),"HUD position did not step back to Bottom center");
 // Right steps through the positions to Top center (the sixth) and stops there; Left comes back.
 for(int i=0;i<netplay::MaxMatchHudAnchor+3;++i){h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);}
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudAnchor==5;}),"HUD position did not reach Top center, or went past it");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudAnchor==4;}),"HUD position did not step back from Top center");
 for(int i=0;i<netplay::MaxMatchHudAnchor+2;++i){h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);}
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudAnchor==0;}),"HUD position did not return to Bottom center");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.FocusOn("hud-spacing");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.matchHudRaised;}),"HUD spacing did not save");
 // A new profile is on Auto. Right from Auto starts at 0 frames, and Left from 0 returns to Auto, keeping the number.
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Screen("tab-matches/defaults");h.FocusOn("delay");
 Check(h.view.preferences.autoInputDelay,"A new profile did not start on Auto");
 h.Press(MenuInput::Right);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return !s.preferences.autoInputDelay&&s.preferences.inputDelay==0;}),"Leaving Auto did not start at 0 frames");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.autoInputDelay&&s.preferences.inputDelay==0;}),"Auto delay did not save");
 h.Screen("tab-access/text");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();h.FocusOn("scale");h.Press(MenuInput::Right);
 h.view.settingsError="Disk unavailable";h.Frame(0,45);
 {std::vector<MenuEntry> saveRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){saveRows=rows;});h.Frame();SetMenuEntriesProbe({});
  const auto retry=std::find_if(saveRows.begin(),saveRows.end(),[](const MenuEntry& e){return e.id=="retry-save";});
  Check(retry!=saveRows.end()&&retry->detail==loc::T("error.settings_not_saved"),"The retry row shows the store's English diagnostic, not the catalog's message");}
 h.Choose("retry-save");h.Frame();
 Check(Is<commands::SavePreferences>(h.actions.back()),"Save retry missing");
 h.view.settingsError.clear();h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();
 // An invitation arrives as a toast; its screen opens from Home's row (or the toast's Join).
 h.view.discordPending=h.view.discordConfirm=h.view.discordCanSwitch=true;h.view.discordRevision=9;h.Frame();
 h.Screen("home");h.Choose("discord-invitation");
 h.Choose("invite-switch");h.Press(MenuInput::Select);Check(!Is<commands::DiscordInvite>(h.actions.back()),"Switch default not Cancel");
 h.Choose("invite-cancel");Check(Is<commands::DiscordInvite>(h.actions.back(),[](const commands::DiscordInvite& d){return d.discordAction==discord::InviteAction::Cancel&&d.discordRevision==9;}),"Discord cancellation");
 h.view.discordPending=false;h.Frame();h.Screen("profile");h.Choose("main-character");h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,40);
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.mainFighter==sf4e::selection::RosterDisplayOrder[1];}),"Profile main was not saved");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();
 h.Screen("profile");h.Choose("main-character");count=h.actions.size();
 h.Press(MenuInput::Right);
 ControllerNavigation profilePad;
 const auto physical=[&](unsigned mapped,unsigned raw){
  ControllerSample sample{3,0,true,ControllerButtons(mapped,3,raw)};
  profilePad.Update(sample,true,true,true);h.Frame(profilePad.Buttons());
 };
 physical(0,0);physical(0,0);physical(0x40,0x40000);physical(0,0);h.Frame(0,40);
 Check(h.actions.size()>count&&Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.mainFighter==sf4e::selection::RosterDisplayOrder[2];}),"Physical A did not save profile portrait");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();
 Check(h.shell.Navigation().Screen()=="profile","Accepted portrait save did not return to Profile");
 h.Choose("main-character");count=h.actions.size();
 ImVec2 mouseTarget;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){if(std::strcmp(id,"main-3")==0)mouseTarget=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);});
 h.Frame();SetMenuCardProbe({});
 auto& mouse=ImGui::GetIO();mouse.AddMousePosEvent(mouseTarget.x,mouseTarget.y);h.Frame();
 mouse.AddMouseButtonEvent(0,true);h.Frame();mouse.AddMouseButtonEvent(0,false);h.Frame(0,40);
 Check(h.actions.size()>count&&Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.mainFighter==3;}),"Mouse click did not save profile portrait");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();Check(h.shell.Navigation().Screen()=="profile","Mouse portrait save did not confirm");
 // A rejected save must leave a labelled, actionable retry in the portrait grid.
 h.Choose("main-character");h.Press(MenuInput::Right);h.accept=false;h.Press(MenuInput::Select);h.Frame(0,40);
 Check(h.shell.Navigation().Screen()=="main-character","Failed portrait save closed the roster");
 bool retryLabel=false;
 // The card's label as the text probe sees it (v2::Fit).
 const std::string retryId=std::string("v2/")+loc::T("settings.retry_save");
 SetMenuTextProbe([&](const char* id,float,float,float width,float available){
  if(retryId==id)retryLabel=width>0&&width<=available;
 });
 h.Frame();SetMenuTextProbe({});
 Check(retryLabel,"Portrait save retry has no visible label");
 h.accept=true;h.Choose("retry-save");h.Frame();
 Check(Is<commands::SavePreferences>(h.actions.back(),[&](const commands::SavePreferences& s){return s.preferences.mainFighter==DisplayedAfter(3);}),"Portrait retry lost the selected main");
 h.view.preferences=std::get<commands::SavePreferences>(h.actions.back()).preferences;h.Frame();
 Check(h.shell.Navigation().Screen()=="profile","Retried portrait save did not return to Profile");
}
// Each journey owns a Harness and with it the ImGui context, so they run one
// after another: a Harness built while another is alive destroys the current
// context on its way out and leaves the outer one without any.
void RoomJourneys() {
 RoomBoardJourneys();
 RoomShortcutOnPanelScreen();
 TableNameCommitNamesTable();
 BoardAreasAreWhereDrawn();
 NarrowBoardTabsStep();
 BoardOpensOnReadyAndStepsAreas();
 AreaStepScrollsIntoView();
 RoomLayoutSavesInRoom();
}

void ArcadeJourneys() {
 using namespace sf4e;
 {
  // An Arcade room's Queue names a cabinet (table + seat * TableCount), not the
  // table its pair plays at: a member paired at table 0 still moves to cabinet 1.
  Harness a;JoinedRoom(a,14);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
  a.view.room.host=1;a.view.room.revision=3;a.view.room.type=room::RoomType::Arcade;
  room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;a.view.room.members.push_back(local);
  room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;a.view.room.members.push_back(peer);
  a.view.room.tables[0].p1=1;a.view.room.tables[0].p2=2;a.view.room.cabinets[1]=1;a.view.room.cabinets[2]=2;
  a.Screen("room");const auto sent=a.actions.size();a.Choose("cabinet-0");
  Check(a.actions.size()==sent+1&&Is<commands::RoomAction>(a.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Queue&&sent.roomAction.table==0&&
   sent.roomAction.seat==0;}),"A paired member could not move to cabinet 1");
  // The status line names the room's type: Arcade, or King of the hill, locked or not.
  std::string status;SetMenuStatusProbe([&](const char* text,Tone){status=text;});a.Frame();
  Check(status==loc::T("room.arcade_status"),"An Arcade room's status line did not say so");
  a.view.room.type=room::RoomType::King;a.Frame();
  Check(status==loc::T("room.king_status"),"A King of the hill room's status line called it an Arcade room");
  a.view.room.locked=true;a.Frame();
  Check(status==loc::T("room.king_locked_status"),"A locked King of the hill room's status line did not say so");
  SetMenuStatusProbe({});
 }
 {
  // King of the hill after a set with another pair in the room: the winner
  // waits for the next one free and the loser leaves the table, so there is
  // nothing to choose; the set's winner stays on, its loser waits, and no
  // answer keeps the seat.
  Harness k;JoinedRoom(k,15);k.view.session.generation.room=1;k.view.session.control=netplay::Health::Healthy;
  k.view.room.host=1;k.view.room.revision=3;k.view.room.type=room::RoomType::King;
  room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;k.view.room.members.push_back(local);
  room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;k.view.room.members.push_back(peer);
  room::Member third;third.id=3;third.name="Third";third.table=1;third.seat=0;k.view.room.members.push_back(third);
  room::Member fourth;fourth.id=4;fourth.name="Fourth";fourth.table=1;fourth.seat=1;k.view.room.members.push_back(fourth);
  auto& t=k.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;
  k.view.room.tables[1].p1=3;k.view.room.tables[1].p2=4;
  k.view.room.cabinets[0]=1;k.view.room.cabinets[8]=2;k.view.room.cabinets[1]=3;k.view.room.cabinets[9]=4;
  k.Screen("room");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  std::map<std::string,std::string> words;SetMenuCardTextProbe([&](const std::string& id,const std::string& text){words[id]=text;});
  const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
  t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=0;t.score[0]=t.score[1]=0;
  t.p2=0;k.view.room.members[1].table=-1;k.view.room.members[1].seat=-1;
  k.view.session.match=netplay::MatchState::Playing;k.view.atMainMenu=false;k.view.foughtGame={0,t.matchGeneration,room::MatchResult::Abort};k.view.matchResult=true;k.Frame();
  Check(k.shell.Navigation().Screen()=="post-match"&&row("again")==rows.end()&&row("back")!=rows.end(),"King of the hill offered a Rematch after a set");
  // The buttons are the rows: Character select and Back to room, each its own action.
  Check(rows.size()==2&&rows[0].id=="fighter"&&rows[1].id=="back","King of the hill's buttons were not Character select and Back to room");
  Check(words["pm-you"]==loc::T("postmatch.king.stays")&&words["pm-opponent"]==loc::T("postmatch.king.waits"),"The set's winner and loser were not shown");
  // Nothing is left to choose, so the buttons say what the room did.
  Check(row("fighter")->detail.rfind(loc::T("postmatch.rotate.winner"),0)==0&&row("back")->detail.rfind(loc::T("postmatch.rotate.winner"),0)==0,
   "The winner was not told they stay on for the next challenger");
  // The winner who keeps the fighter is told so once the room has the set's result.
  Check(row("fighter")->detail.find(loc::T("selection.winner_keeps_fighter"))==std::string::npos,"A winner who is not the keeper was told they keep the fighter");
  t.keeper=1;t.phase=room::TablePhase::Waiting;k.Frame();
  Check(row("fighter")->detail.find(loc::T("selection.winner_keeps_fighter"))!=std::string::npos,"The set's winner was not told they keep the fighter");
  t.phase=room::TablePhase::Playing;
  // Before the result, a keeper from the last set is not this game's: a game a
  // win from the goal may have ended the set the other way.
  t.lastSet.generation=4;t.rules.format=room::SetFormat::Ft2;t.score[0]=1;k.Frame();
  Check(row("fighter")->detail.find(loc::T("selection.winner_keeps_fighter"))==std::string::npos,"The last set's keeper was told they keep the fighter before the result");
  // A game that cannot end the set (0-0 in FT2) leaves the keeper known before the result.
  t.score[0]=0;k.Frame();
  Check(row("fighter")->detail.find(loc::T("selection.winner_keeps_fighter"))!=std::string::npos,"The keeper was not told before the result of a game that cannot end the set");
  t.score[0]=1;
  // Once the result of a game inside the set is in, the keeper still keeps it.
  t.phase=room::TablePhase::Waiting;k.Frame();
  Check(row("fighter")->detail.find(loc::T("selection.winner_keeps_fighter"))!=std::string::npos,"The keeper lost the sentence once a game inside the set was in");
  t.phase=room::TablePhase::Playing;
  t.lastSet.generation=5;t.score[0]=0;t.rules.format=room::SetFormat::Unlimited;t.keeper=0;k.Frame();
  const std::string keep=loc::T("postmatch.back.keep_seat");const auto& back=row("back")->detail;
  Check(back.size()>=keep.size()&&back.compare(back.size()-keep.size(),keep.size(),keep)==0,"Back after a King of the hill set said it gives up the seat");
  for(const char* id:{"fighter","back"}){k.FocusOn(id);k.Press(MenuInput::Down);
   Check(k.shell.Navigation().Focus()==id,"Down left the post-match buttons");}
  const auto sent=k.actions.size();k.Wait(11);
  k.view.matchResult=false;k.view.session.match=netplay::MatchState::PostMatch;k.view.atMainMenu=true;t.phase=room::TablePhase::Waiting;k.open=true;k.Frame(0,5);
  Check(std::none_of(k.actions.begin()+sent,k.actions.end(),[](const ShellAction& a){return RoomActionIs(a,room::ActionKind::Unqueue);}),"No answer after a King of the hill set left the cabinet");
  SetMenuEntriesProbe({});SetMenuCardTextProbe({});
 }
 {
  // An Arcade set over with nobody waiting alone: the pair decides, and the
  // prompt asks the room's own question. Rematch and Y's New opponent are the
  // table's choices, not a Ready the room refuses while it decides; no answer
  // and Back to room keep the cabinet (ARCADE_CABINET_MODE.md, item 4).
  Harness a;JoinedRoom(a,16);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
  a.view.room.host=1;a.view.room.revision=3;a.view.room.type=room::RoomType::Arcade;
  room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;a.view.room.members.push_back(local);
  room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;a.view.room.members.push_back(peer);
  auto& t=a.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;
  // A second pair at cabinets 2 and 10: with others seated a finished pair decides.
  room::Member third;third.id=3;third.name="Third";third.table=1;third.seat=0;a.view.room.members.push_back(third);
  room::Member fourth;fourth.id=4;fourth.name="Fourth";fourth.table=1;fourth.seat=1;a.view.room.members.push_back(fourth);
  a.view.room.tables[1].p1=3;a.view.room.tables[1].p2=4;
  a.view.room.cabinets[0]=1;a.view.room.cabinets[8]=2;a.view.room.cabinets[1]=3;a.view.room.cabinets[9]=4;
  a.Screen("room");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
  const auto setOver=[&](std::uint64_t game){
   t.matchGeneration=game;t.phase=room::TablePhase::Playing;t.lastSet.generation=game;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=0;
   t.deciding=true;t.rematch[0]=t.rematch[1]=false;a.view.matchResult=false;a.Frame();
   a.view.session.match=netplay::MatchState::Playing;a.view.atMainMenu=false;a.view.foughtGame={0,t.matchGeneration,room::MatchResult::Abort};a.view.matchResult=true;a.Frame();
  };
  const auto gameOver=[&]{a.view.matchResult=false;a.view.session.match=netplay::MatchState::PostMatch;a.view.atMainMenu=true;
   t.phase=room::TablePhase::Waiting;a.open=true;a.Frame(0,5);};
  const auto leftCabinet=[&](std::size_t from){return std::any_of(a.actions.begin()+static_cast<std::ptrdiff_t>(from),a.actions.end(),
   [](const ShellAction& x){return RoomActionIs(x,room::ActionKind::Unqueue);});};
  setOver(5);
  Check(a.shell.Navigation().Screen()=="post-match"&&row("rematch")!=rows.end()&&row("new-opponent")!=rows.end()&&row("again")==rows.end(),
   "A deciding Arcade pair was not asked Rematch or New opponent");
  Check(row("back")->detail==loc::T("postmatch.back.keep_seat"),"Back after an Arcade set said it gives up the cabinet");
  const auto quiet=a.actions.size();a.Wait(11);gameOver();
  Check(!leftCabinet(quiet),"No answer after an Arcade set left the cabinet");
  // With the prompt missed, the room's Ready is the pair's Rematch.
  const auto readied=a.actions.size();a.Screen("room");a.Choose("ready");
  Check(a.actions.size()==readied+1&&RoomActionIs(a.actions.back(),room::ActionKind::Rematch),
   "Ready after a missed Arcade prompt was not the table's Rematch");
  // Another pair's cabinet focused last (the board's selected table follows
  // it): Ready still answers this player's own table.
  a.FocusOn("cabinet-1");a.shell.Navigation().Focus("ready",rows);
  const auto elsewhere=a.actions.size();a.Press(MenuInput::Select);
  Check(a.actions.size()==elsewhere+1&&Is<commands::RoomAction>(a.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Rematch&&sent.roomAction.table==0;}),
   "Ready with another pair's cabinet focused answered that pair's table");
  setOver(6);
  const auto chose=a.actions.size();a.Choose("rematch");
  Check(a.actions.size()==chose+1&&Is<commands::RoomAction>(a.actions.back(),[](const commands::RoomAction& sent){return sent.roomAction.kind==room::ActionKind::Rematch&&
   sent.roomAction.table==0;}),"The prompt's Rematch was not the table's Rematch while it decides");
  t.rematch[0]=true;a.Frame();
  Check(!row("rematch")->enabled,"The prompt offered Rematch again after the room showed it chosen");
  const auto opponent=a.actions.size();a.Press(MenuInput::Options);
  Check(a.actions.size()==opponent+1&&RoomActionIs(a.actions.back(),room::ActionKind::NewOpponent),"Y on a deciding prompt did not choose New opponent");
  // A Back mashed through the WIN keeps the cabinet.
  const auto backed=a.actions.size();a.Press(MenuInput::Back);a.Press(MenuInput::Back);gameOver();
  Check(!leftCabinet(backed),"Back to room after an Arcade set left the cabinet");
  // Both choose Rematch: the prompt waits past its timer while the other
  // decides, and once the room asks no more it readies the player for the
  // next set itself, rather than sending them back to the room unready.
  setOver(7);a.Choose("rematch");t.rematch[0]=true;a.Wait(11);
  Check(a.shell.Navigation().Screen()=="post-match","The prompt gave up on a pair whose Rematch was chosen while the other decided");
  const auto paired=a.actions.size();t.deciding=false;t.rematch[0]=false;t.phase=room::TablePhase::Waiting;a.Frame();
  Check(a.actions.size()==paired+1&&ReadyIs(a.actions.back(),true),"Both choosing Rematch did not ready the player for the next set");
  a.Frame(0,3);
  Check(a.actions.size()==paired+1&&a.shell.Navigation().Screen()=="post-match","The pair's Rematch was sent twice or the prompt left");
  gameOver();
  // Rematch, then Back before the other answered: once both answers end the
  // room's question, the prompt readies nobody who last chose Back.
  setOver(8);a.Choose("rematch");t.rematch[0]=true;a.Frame();a.Press(MenuInput::Back);
  const auto withdrawn=a.actions.size();t.deciding=false;t.rematch[0]=false;t.phase=room::TablePhase::Waiting;a.Frame(0,3);
  Check(std::none_of(a.actions.begin()+static_cast<std::ptrdiff_t>(withdrawn),a.actions.end(),
   [](const ShellAction& x){return ReadyIs(x,true);}),"A Rematch taken back with Back still readied the player");
  SetMenuEntriesProbe({});
 }
 for(const auto type:{room::RoomType::Arcade,room::RoomType::King}){
  // The set's loser, whom the room moved on to wait at another table before
  // the WIN was over, still gets the prompt of the game they fought: the
  // winner stays on, the loser waits, and there is nothing to choose.
  Harness a;JoinedRoom(a,19);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
  a.view.room.host=1;a.view.room.revision=3;a.view.room.type=type;
  room::Member local;local.id=1;local.name="Local";local.table=1;local.seat=0;a.view.room.members.push_back(local);
  room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;a.view.room.members.push_back(peer);
  room::Member third;third.id=3;third.name="Third";third.table=0;third.seat=0;a.view.room.members.push_back(third);
  a.view.room.cabinets[0]=1;a.view.room.cabinets[8]=2;a.view.room.cabinets[1]=3;
  // Third waited alone at table 1: they now play the winner at table 0, and the loser waits at table 1.
  auto& t=a.view.room.tables[0];t.p1=3;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Waiting;t.rules.format=room::SetFormat::Ft2;
  t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=1;t.lastSet.score[1]=2;
  a.view.room.tables[1].p1=1;
  a.Screen("room");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  std::map<std::string,std::string> words;SetMenuCardTextProbe([&](const std::string& id,const std::string& text){words[id]=text;});
  const auto has=[&](const char* id){return std::any_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
  a.view.session.match=netplay::MatchState::Playing;a.view.atMainMenu=false;
  a.view.foughtGame={0,5,room::MatchResult::P2Win};a.view.matchResult=true;a.Frame();
  const std::string mode=type==room::RoomType::Arcade?"Arcade":"King of the hill";
  Check(a.shell.Navigation().Screen()=="post-match"&&has("fighter")&&has("back")&&!has("again"),
   ("The loser of an "+mode+" set moved to another table got no prompt").c_str());
  Check(words["pm-you"]==loc::T("postmatch.king.waits")&&words["pm-opponent"]==loc::T("postmatch.king.stays"),
   ("The loser of an "+mode+" set was not shown waiting while the winner stays on").c_str());
  SetMenuEntriesProbe({});SetMenuCardTextProbe({});
 }
 {
  // PostMatchFocusedButtonInView: the Arcade decision's four buttons, on a
  // narrow, short screen at 150% text where their bar takes two lines and
  // reaches below the prompt's panel, which grows to its content only as far
  // as the screen's height (480 lines here) lets it. Each one the pad focuses
  // is scrolled into view, inside the clip it is drawn under.
  Harness a;JoinedRoom(a,18);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
  a.view.room.host=1;a.view.room.revision=3;a.view.room.type=room::RoomType::Arcade;
  for(int i=0;i<4;++i){room::Member m;m.id=static_cast<room::MemberId>(i+1);m.name="Player "+std::to_string(i+1);m.table=static_cast<std::int8_t>(i/2);m.seat=static_cast<std::int8_t>(i%2);a.view.room.members.push_back(m);}
  auto& t=a.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;
  a.view.room.tables[1].p1=3;a.view.room.tables[1].p2=4;
  a.view.room.cabinets[0]=1;a.view.room.cabinets[8]=2;a.view.room.cabinets[1]=3;a.view.room.cabinets[9]=4;
  a.imgui.io.DisplaySize=ImVec2(640,480);ApplyTheme(1.f,1.5f);
  a.Screen("room");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  std::map<std::string,bool> inView;std::map<std::string,float> top,content;
  SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){
   const auto* d=ImGui::GetWindowDrawList();const ImVec2 lo=d->GetClipRectMin(),hi=d->GetClipRectMax();
   // The clip is whole pixels, the button not: a pixel's slack.
   inView[id]=min.x>=lo.x-1&&min.y>=lo.y-1&&max.x<=hi.x+1&&max.y<=hi.y+1;top[id]=min.y;content[id]=min.y-ImGui::GetWindowPos().y+ImGui::GetScrollY();});
  t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=0;t.deciding=true;
  a.view.session.match=netplay::MatchState::Playing;a.view.atMainMenu=false;a.view.foughtGame={0,t.matchGeneration,room::MatchResult::Abort};a.view.matchResult=true;a.Frame();
  const char* const buttons[]={"rematch","new-opponent","fighter","back"};
  Check(a.shell.Navigation().Screen()=="post-match"&&rows.size()==4,"The Arcade decision prompt did not open with its four buttons");
  a.Frame(0,2);
  float lowest=0,highest=1e9f;for(const char* id:buttons){lowest=(std::max)(lowest,top[id]);highest=(std::min)(highest,top[id]);}
  Check(lowest>highest+1,"The post-match fixture's buttons did not take two lines");
  for(const char* id:buttons){
   a.FocusOn(id);a.Frame(0,3);
   Check(a.shell.Navigation().Screen()=="post-match"&&a.shell.Navigation().Focus()==id&&inView[id],
    (std::string("A focused post-match button was out of view: ")+id).c_str());
  }
  // The prompt's own scroll, in the window the buttons are drawn in.
  ImGuiWindow* panel=nullptr;
  for(auto* w:GImGui->Windows)if(w->Active&&std::strstr(w->Name,"/Body_"))panel=w;
  Check(panel&&panel->ScrollMax.y>0,"The post-match fixture's panel did not scroll");
  // A scroll of the player's own stays put while nothing else changes.
  ImGui::SetScrollY(panel,0);a.Frame(0,3);
  Check(panel->Scroll.y<1&&a.shell.Navigation().Focus()=="back"&&!inView["back"],"The post-match panel snapped back from the player's own scroll");
  ImGui::SetScrollY(panel,panel->ScrollMax.y);a.Frame(0,3);
  Check(inView["back"],"The post-match fixture could not scroll Back into view");
  // Narrower, the words over the bar take another line and Back moves down,
  // its bar still on two lines and the focus unchanged: it is shown again.
  const float before=content["back"];
  a.imgui.io.DisplaySize=ImVec2(560,480);a.Frame(0,3);
  float low=0,high=1e9f;for(const char* id:buttons){low=(std::max)(low,top[id]);high=(std::min)(high,top[id]);}
  Check(low>high+1&&content["back"]>before+1,"The narrower post-match fixture did not move Back down on a two-line bar");
  Check(a.shell.Navigation().Focus()=="back"&&inView["back"],"Back moved down in the post-match panel and stayed out of view");
  SetMenuEntriesProbe({});SetMenuCardProbe({});ApplyTheme(1.f);
 }
 {
  // PostMatchPanelFitsItsContent (#185): an Arcade pair deciding after a set,
  // its four buttons and the Ultra row under the players. At 720p and 1080p
  // (and 1080p at 150%) the panel is as tall as all of it from the prompt's
  // first frame, and stays so: nothing scrolls, no scroll bar covers P1's row,
  // every button is in sight, and the panel never changes height a frame late.
  // Content that changes while it is open (the Ultra row going and coming
  // back) is sized on the frame it changes.
  const std::pair<ImVec2,float> screens[]={{ImVec2(1280,720),1.f},{ImVec2(1920,1080),1.f},{ImVec2(1920,1080),1.5f}};
  for(const auto& screen:screens){
   // Named, not bound, so the checks below can capture them.
   const ImVec2 size=screen.first;const float scale=screen.second;
   Harness a;JoinedRoom(a,18);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
   a.view.room.host=1;a.view.room.revision=3;a.view.room.type=room::RoomType::Arcade;
   for(int i=0;i<4;++i){room::Member m;m.id=static_cast<room::MemberId>(i+1);m.name="Player "+std::to_string(i+1);m.table=static_cast<std::int8_t>(i/2);m.seat=static_cast<std::int8_t>(i%2);a.view.room.members.push_back(m);}
   auto& t=a.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;t.rules.format=room::SetFormat::Ft2;
   a.view.room.tables[1].p1=3;a.view.room.tables[1].p2=4;
   a.view.room.cabinets[0]=1;a.view.room.cabinets[8]=2;a.view.room.cabinets[1]=3;a.view.room.cabinets[9]=4;
   a.view.ultraSteps=true;a.view.ultraName=ui::UltraName(0,0);a.view.canChangeUltra=true;
   a.imgui.io.DisplaySize=size;ApplyTheme(1.f,scale);
   a.Screen("room");
   std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
   std::map<std::string,bool> inView;
   SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){
    const auto* d=ImGui::GetWindowDrawList();const ImVec2 lo=d->GetClipRectMin(),hi=d->GetClipRectMax();
    inView[id]=min.x>=lo.x-1&&min.y>=lo.y-1&&max.x<=hi.x+1&&max.y<=hi.y+1;});
   const std::string at=std::to_string(static_cast<int>(size.x))+"x"+std::to_string(static_cast<int>(size.y))+" at "+std::to_string(static_cast<int>(scale*100))+"%";
   const char* const buttons[]={"rematch","new-opponent","fighter","back"};
   // The frame just drawn: the prompt's body scrolls not at all, all it drew
   // lies inside it, every button is in sight, and the panel is on the screen.
   const auto fits=[&](const char* when){
    ImGuiWindow* panel=nullptr;ImGuiWindow* outer=nullptr;
    for(auto* w:GImGui->Windows){if(w->Active&&std::strstr(w->Name,"/Body_"))panel=w;if(w->Active&&std::strstr(w->Name,"###EmberShell")&&!std::strstr(w->Name,"/"))outer=w;}
    const std::string where=at+" "+when;
    Check(a.shell.Navigation().Screen()=="post-match"&&panel&&outer,("The post-match prompt was not drawn at "+where).c_str());
    Check(panel->ScrollMax.y<1&&panel->Scroll.y<1&&!panel->ScrollbarY,("The post-match panel scrolled at "+where).c_str());
    Check(panel->DC.CursorMaxPos.y-panel->DC.CursorStartPos.y<=panel->InnerRect.GetHeight()+.5f,("The post-match content ran past its panel at "+where).c_str());
    Check(outer->Pos.y>=0&&outer->Pos.y+outer->Size.y<=size.y,("The post-match panel left the screen at "+where).c_str());
    for(const char* id:buttons)Check(inView[id],("A post-match button was out of sight at "+where+": "+id).c_str());
    return outer->Size.y;
   };
   t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=0;t.lastSet.score[0]=2;t.lastSet.score[1]=1;t.deciding=true;
   a.view.session.match=netplay::MatchState::Playing;a.view.atMainMenu=false;a.view.foughtGame={0,t.matchGeneration,room::MatchResult::P1Win};a.view.matchResult=true;
   inView.clear();a.Frame();
   Check(rows.size()==5&&rows.back().id=="ultra"&&rows.back().enabled,("The Arcade decision prompt did not open with its four buttons and the Ultra row at "+at).c_str());
   const float first=fits("on its first frame");
   inView.clear();a.Frame(0,3);
   Check(fits("after three frames")==first,("The post-match panel changed height after its first frame at "+at).c_str());
   // The Ultra row goes (it cannot change any more) and comes back: the panel
   // takes the new height on the very frame the content changes.
   a.view.ultraSteps=false;inView.clear();a.Frame();
   Check(rows.size()==4,("The post-match Ultra row did not go at "+at).c_str());
   Check(fits("on the frame its Ultra row went")<first,("The post-match panel kept the Ultra row's height at "+at).c_str());
   a.view.ultraSteps=true;inView.clear();a.Frame();
   Check(rows.size()==5&&fits("on the frame its Ultra row came back")==first,("The post-match panel did not take the Ultra row back at once at "+at).c_str());
   SetMenuEntriesProbe({});SetMenuCardProbe({});ApplyTheme(1.f);
  }
 }
 {
  // PostMatchNewOpponentIsNoRotation: a pair that stayed to decide after the
  // set, then parted by the loser's New opponent (a swap with a fighter who
  // asked at another table), is not the set's rotation: the winner did not
  // "stay on" and the loser does not "wait". Each chip names who they play now,
  // and no button explains a rotation.
  Harness a;JoinedRoom(a,18);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
  a.view.room.host=1;a.view.room.revision=3;a.view.room.type=room::RoomType::Arcade;
  for(int i=0;i<4;++i){room::Member m;m.id=static_cast<room::MemberId>(i+1);m.name="Player "+std::to_string(i+1);m.table=static_cast<std::int8_t>(i/2);m.seat=static_cast<std::int8_t>(i%2);a.view.room.members.push_back(m);}
  auto& t=a.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;t.rules.format=room::SetFormat::Ft2;
  auto& other=a.view.room.tables[1];other.p1=3;other.p2=4;other.phase=room::TablePhase::Waiting;other.deciding=true;other.wantsNew[0]=true;
  a.view.room.cabinets[0]=1;a.view.room.cabinets[8]=2;a.view.room.cabinets[1]=3;a.view.room.cabinets[9]=4;
  a.Screen("room");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  std::map<std::string,std::string> words;SetMenuCardTextProbe([&](const std::string& id,const std::string& text){words[id]=text;});
  const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
  // Player 2 won the set; the pair stays to decide.
  t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=1;t.lastSet.score[1]=2;t.deciding=true;
  a.view.session.match=netplay::MatchState::Playing;a.view.atMainMenu=false;a.view.foughtGame={0,t.matchGeneration,room::MatchResult::P2Win};a.view.matchResult=true;a.Frame();
  Check(a.shell.Navigation().Screen()=="post-match"&&row("new-opponent")!=rows.end(),"The deciding prompt did not open with New opponent");
  // The loser's New opponent swaps them with Player 3, who asked at table 2:
  // Player 3 now plays the winner here, and this PC plays Player 4 there.
  t.deciding=false;t.p1=3;t.phase=room::TablePhase::Waiting;
  other.deciding=false;other.wantsNew[0]=false;other.p1=1;
  for(auto& m:a.view.room.members){if(m.id==1){m.table=1;m.seat=0;}if(m.id==3){m.table=0;m.seat=0;}}
  ++a.view.room.revision;a.Frame(0,2);
  Check(a.shell.Navigation().Screen()=="post-match","The prompt closed when New opponent parted the pair");
  Check(words["pm-you"]==loc::Tf("room.arcade_vs","Player 4")&&words["pm-opponent"]==loc::Tf("room.arcade_vs","Player 3"),
   "After a New opponent swap the prompt's chips did not name who each fighter plays now");
  const auto fighter=row("fighter");
  Check(fighter!=rows.end()&&fighter->detail.find(loc::T("postmatch.rotate.winner"))==std::string::npos&&
   fighter->detail.find(loc::Tf("postmatch.rotate.loser","Player 2"))==std::string::npos,"A New opponent swap was explained as the set's rotation");
  SetMenuEntriesProbe({});SetMenuCardTextProbe({});
 }
 for(const auto type:{room::RoomType::Arcade,room::RoomType::King}){
  // A pair alone in an Arcade or King room stays together after a set and
  // readies again: the prompt offers the ordinary Rematch and names no winner
  // who stays or loser who waits (ARCADE_CABINET_MODE.md, items 4 to 6). It
  // does so before the room has the result too, which an Unlimited game that
  // may go on in place only sends once it has. A Rematch the runtime refuses
  // is offered again, and an opponent who leaves the cabinets has left.
  Harness a;JoinedRoom(a,17);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
  a.view.room.host=1;a.view.room.revision=3;a.view.room.type=type;
  room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;a.view.room.members.push_back(local);
  room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;a.view.room.members.push_back(peer);
  auto& t=a.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;
  t.rules.format=room::SetFormat::Unlimited;
  a.view.room.cabinets[0]=1;a.view.room.cabinets[8]=2;
  a.Screen("room");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  std::map<std::string,std::string> words;SetMenuCardTextProbe([&](const std::string& id,const std::string& text){words[id]=text;});
  const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
  const std::string mode=type==room::RoomType::Arcade?"Arcade":"King of the hill";
  const auto offered=[&]{return a.shell.Navigation().Screen()=="post-match"&&row("again")!=rows.end()&&row("again")->enabled&&
   row("rematch")==rows.end()&&row("new-opponent")==rows.end();};
  a.view.session.match=netplay::MatchState::Playing;a.view.atMainMenu=false;a.view.foughtGame={0,t.matchGeneration,room::MatchResult::Abort};a.view.matchResult=true;a.Frame();
  Check(offered(),("A lone pair in "+mode+" was not offered the ordinary Rematch before the room had the result").c_str());
  t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=1;t.score[0]=t.score[1]=0;a.Frame();
  Check(offered(),("A lone pair in "+mode+" was not offered the ordinary Rematch").c_str());
  Check(words["pm-you"]!=loc::T("postmatch.king.waits")&&words["pm-opponent"]!=loc::T("postmatch.king.stays"),
   ("A lone pair in "+mode+" was shown a winner who stays and a loser who waits").c_str());
  const auto sent=a.actions.size();a.Choose("again");
  Check(a.actions.size()==sent+1&&ReadyIs(a.actions.back(),true),("A lone pair's Rematch in "+mode+" was not sent").c_str());
  Check(row("again")!=rows.end()&&!row("again")->enabled,("A lone pair in "+mode+" was offered Rematch again while it was on its way").c_str());
  a.view.readyFailure="Assign a controller first.";a.view.readyFailureSequence=1;a.Frame();
  Check(row("again")!=rows.end()&&row("again")->enabled,("A refused Rematch in "+mode+" was not offered again").c_str());
  // With the opponent's Rematch already in, the start the prompt promised is
  // taken back with a refusal too.
  a.Press(MenuInput::Select);  // the refusal's notice
  a.view.rematchChoices=2;const auto again=a.actions.size();a.Choose("again");a.Frame();
  Check(a.actions.size()==again+1&&ReadyIs(a.actions.back(),true),("A lone pair's second Rematch in "+mode+" was not sent").c_str());
  Check(!row("again")->enabled,("A lone pair in "+mode+" was offered Rematch again while starting").c_str());
  a.view.readyFailureSequence=2;a.Frame();
  Check(row("again")->enabled,("A refused Rematch in "+mode+" stayed starting with the opponent's Rematch in").c_str());
  a.view.rematchChoices=0;
  // The opponent gone from the cabinets has left; that is no rotation. Nor
  // when the room then pairs this player with one who waited alone.
  t.p2=0;a.view.room.members[1].table=-1;a.view.room.members[1].seat=-1;a.view.room.cabinets[8]=0;a.Frame();
  Check(words["pm-opponent"]==loc::T("postmatch.state.left"),("An opponent who left a lone pair in "+mode+" was shown as rotated").c_str());
  room::Member third;third.id=3;third.name="Third";third.table=1;third.seat=0;a.view.room.members.push_back(third);
  a.view.room.cabinets[1]=3;a.view.room.tables[1].p1=3;a.view.room.tables[1].p2=1;t.p1=0;
  a.view.room.members[0].table=1;a.view.room.members[0].seat=1;a.Frame();
  Check(words["pm-opponent"]==loc::T("postmatch.state.left"),("An opponent who left in "+mode+" was shown as rotated once this player moved on").c_str());
  SetMenuEntriesProbe({});SetMenuCardTextProbe({});
 }
}
// The post-match prompt over the WIN screen: Rematch sends the room's Ready
// and the prompt stays until the next game starts; no answer after a game
// with no set length leaves the seat once the game is over; inside a set it
// keeps the seat; a queue greys Rematch out.
void PostMatchJourneys() {
 using namespace sf4e;
 Harness h;JoinedRoom(h,12);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;
 h.view.room.host=1;h.view.room.revision=3;
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;h.view.room.members.push_back(peer);
 auto& t=h.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;
 h.Screen("room");
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 const auto row=[&](const char* id)->const MenuEntry&{
  const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  if(it==rows.end())throw std::runtime_error(std::string("Post-match row missing: ")+id+" on "+h.shell.Navigation().Screen()+" after "+std::to_string(h.actions.size())+" actions");return *it;};
 std::map<std::string,std::string> words;SetMenuCardTextProbe([&](const std::string& id,const std::string& text){words[id]=text;});
 const auto win=[&]{h.view.session.match=netplay::MatchState::Playing;h.view.atMainMenu=false;h.view.foughtGame={0,t.matchGeneration,room::MatchResult::Abort};h.view.matchResult=true;h.Frame();};
 const auto gameOver=[&]{h.view.matchResult=false;h.view.session.match=netplay::MatchState::PostMatch;h.view.atMainMenu=true;
  t.phase=room::TablePhase::Waiting;h.open=true;h.Frame();};
 win();
 Check(h.shell.Navigation().Screen()=="post-match"&&row("again").label==loc::T("postmatch.rematch")&&row("again").enabled,"WIN did not open the post-match prompt");
 // Down stays on the buttons: the two fighters' answers are drawn, not rows.
 h.FocusOn("again");h.Press(MenuInput::Down);Check(h.shell.Navigation().Focus()=="again","Down left the post-match buttons");
 h.Choose("again");Check(ReadyIs(h.actions.back(),true),"Rematch did not send the room's Ready");
 // That Ready plays the next game whatever else is pressed over the WIN, so
 // Character select (X) and Back to room are not offered there and leave the
 // answer shown, which once read Character select over a rematch that went on.
 {const auto sent=h.actions.size();h.Press(MenuInput::Fighter);h.Press(MenuInput::Back);
  Check(h.shell.Navigation().Screen()=="post-match"&&words["pm-you"]==loc::T("postmatch.state.ready")&&!row("fighter").enabled&&!row("back").enabled&&
   h.actions.size()==sent,"Character select or Back over the WIN replaced a chosen Rematch");}
 // Both chose: the room starts the next game at once; the prompt covers the
 // room until that game loads, past its own timer.
 // The other Ready and the new game can arrive in one snapshot.
 // The press is held while the WIN plays out: past the timer, before the room shows it.
 {const auto sent=h.actions.size();h.Wait(11);
  Check(h.shell.Navigation().Screen()=="post-match"&&h.actions.size()==sent,"A Rematch the room did not show yet was taken for no answer");}
 t.ready[0]=true;h.Wait(11);
 Check(h.shell.Navigation().Screen()=="post-match","Having chosen Rematch, the prompt did not wait for the opponent");
 gameOver();
 t.ready[1]=true;t.matchGeneration=6;t.phase=room::TablePhase::Playing;h.Wait(11);
 Check(h.shell.Navigation().Screen()=="post-match"&&!row("again").enabled,"The room showed before the chosen rematch loaded");
 // It covers the game's main menu while the next game is prepared, and ends once it plays.
 t.ready[0]=t.ready[1]=false;h.view.session.match=netplay::MatchState::Preparing;h.Frame();
 Check(h.shell.Navigation().Screen()=="post-match","The prompt left the main menu showing while the next game was prepared");
 h.view.session.match=netplay::MatchState::Playing;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","The next game playing did not end the prompt");
 // In place: both Rematch choices travel with the inputs over the WIN and the
 // battle goes on to the next game, with no new room game. That ends the
 // prompt, and the next game's WIN opens its own.
 // The Readies that started the game stand until its match ends: no choice yet.
 t.ready[0]=t.ready[1]=true;h.view.matchResult=false;h.Frame();win();
 Check(row("again").enabled&&words["pm-you"]!=loc::T("postmatch.state.ready"),"The Ready that started the game was taken for a Rematch");
 h.Choose("again");h.view.rematchChoices=3;h.Frame();
 h.view.rematchChoices=0;h.view.matchResult=false;h.view.gamesInPlace=1;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","A rematch in place did not end the prompt");
 win();Check(h.shell.Navigation().Screen()=="post-match"&&row("again").enabled,"The next WIN in the same battle opened no prompt");
 // That game ended the match: both chose Rematch and the battle closes for the
 // next one to load. The count of games in place starts again from none then,
 // which ends no prompt: it covers the main menu until the next game plays.
 h.Choose("again");
 h.view.gamesInPlace=0;h.view.session.match=netplay::MatchState::Preparing;h.view.atMainMenu=true;
 t.phase=room::TablePhase::Waiting;t.ready[0]=t.ready[1]=true;h.Frame();
 Check(h.shell.Navigation().Screen()=="post-match","The battle closing after a game in place showed the room before the next game loaded");
 h.view.matchResult=false;h.view.session.match=netplay::MatchState::Playing;h.view.atMainMenu=false;t.matchGeneration=7;t.phase=room::TablePhase::Playing;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","The next game playing did not end the prompt kept over a closed battle");
 h.view.matchResult=false;h.view.gamesInPlace=0;t.ready[0]=t.ready[1]=false;h.Frame();
 // No answer with no set length: Ember steps aside over WIN, then leaves the seat in the room.
 h.view.matchResult=false;h.Frame();win();const auto before=h.actions.size();h.Wait(11);
 Check(!h.open&&h.shell.Navigation().Screen()!="post-match"&&h.actions.size()==before,"No answer did not close the prompt over WIN");
 gameOver();h.Frame();
 Check(h.actions.size()==before+1&&RoomActionIs(h.actions.back(),room::ActionKind::Unqueue),"No answer after the game did not leave the seat");
 // A Rematch the runtime refuses as the game ends, the opponent's already in,
 // is still an answer: the seat stays.
 h.view.matchResult=false;h.Frame();win();h.view.rematchChoices=2;const auto refused=h.actions.size();h.Choose("again");h.Frame();
 h.view.readyFailure="Assign a controller first.";++h.view.readyFailureSequence;gameOver();h.Frame();
 Check(h.actions.size()>refused&&std::none_of(h.actions.begin()+static_cast<std::ptrdiff_t>(refused),h.actions.end(),
  [](const ShellAction& x){return RoomActionIs(x,room::ActionKind::Unqueue);}),"A refused Rematch gave up the seat once the game was over");
 h.view.rematchChoices=0;h.Press(MenuInput::Select);  // the refusal's notice
 // Back to room over the WIN: the prompt shows that choice until the game is
 // over, and keeps the seat, so a Back mashed through the WIN costs nothing.
 // The game over, the result does not open a new prompt.
 h.view.matchResult=false;h.Frame();win();const auto backed=h.actions.size();h.Choose("back");h.Wait(2);
 Check(h.open&&h.shell.Navigation().Screen()=="post-match"&&words["pm-you"]==loc::T("postmatch.back")&&!row("back").enabled&&h.actions.size()==backed,
  "Back to room over the WIN did not keep the choice shown");
 gameOver();h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match"&&h.actions.size()==backed,"Back to room gave up the seat once the game was over");
 h.view.foughtGame={0,t.matchGeneration,room::MatchResult::Abort};h.view.matchResult=true;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","A result after the game was over opened the prompt over the room");
 h.view.matchResult=false;h.Frame();
 // Inside a set: Next game, and Back keeps the seat.
 t.rules.format=room::SetFormat::Ft2;t.matchGeneration=7;t.phase=room::TablePhase::Playing;h.view.room.tables[0].queue={3};
 win();Check(row("again").label==loc::T("postmatch.next_game")&&row("again").enabled,"A set's game did not offer Next game despite the queue");
 const auto midSet=h.actions.size();h.Press(MenuInput::Back);gameOver();h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match"&&h.actions.size()==midSet,"Back inside a set left the seat");
 // The game that wins the set, its result known before the room ends the set:
 // Rematch, not Next game, so nobody takes the set for going on.
 t.matchGeneration=20;t.phase=room::TablePhase::Playing;t.score[0]=1;h.view.matchResult=false;h.Frame();
 h.view.session.match=netplay::MatchState::Playing;h.view.atMainMenu=false;h.view.foughtGame={0,20,room::MatchResult::P1Win};h.view.matchResult=true;h.Frame();
 Check(row("again").label==loc::T("postmatch.rematch"),"The game that won the set offered Next game before the room ended the set");
 h.Press(MenuInput::Back);gameOver();h.Frame();t.score[0]=0;
 // The set over, someone queued: no rematch.
 t.matchGeneration=8;t.phase=room::TablePhase::Playing;t.lastSet.generation=8;t.lastSet.p1=1;t.lastSet.p2=2;h.view.matchResult=false;h.Frame();win();
 Check(row("again").label==loc::T("postmatch.rematch")&&!row("again").enabled&&row("again").detail==loc::T("postmatch.queue_waiting"),
  "Rematch was offered while someone waits in the queue");
 // A WIN cut short: the game is over before anyone answered and the opponent
 // has left. The set's winner, whom the rotation keeps, keeps the seat for the
 // next challenger without answering. Then the room board takes a seat by mouse.
 const auto setOver=h.actions.size();
 gameOver();t.p2=0;h.view.room.members[1].table=h.view.room.members[1].seat=-1;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","The prompt stayed over the room after the game was over");
 h.Wait(11);
 Check(std::none_of(h.actions.begin()+static_cast<std::ptrdiff_t>(setOver),h.actions.end(),
  [](const ShellAction& x){return RoomActionIs(x,room::ActionKind::Unqueue);}),"The set's winner gave up the seat for not answering");
 // Where the loser stays, the winner the rotation moves on gives it up.
 t.rules.rotation=room::RotationMode::LoserStays;h.view.matchResult=false;h.Frame();win();
 const auto moved=h.actions.size();gameOver();h.Wait(11);
 Check(std::any_of(h.actions.begin()+static_cast<std::ptrdiff_t>(moved),h.actions.end(),
  [](const ShellAction& x){return RoomActionIs(x,room::ActionKind::Unqueue);}),"The winner the rotation moves on kept the seat without answering");
 t.rules.rotation=room::RotationMode::WinnerStays;
 t.p1=0;t.queue.clear();h.view.room.members[0].table=h.view.room.members[0].seat=-1;++h.view.room.revision;h.Frame();h.Frame();
 std::map<std::string,ImVec2> cards;
 SetMenuCardProbe([&](const char* id,ImVec2 a,ImVec2 b){cards[id]=ImVec2((a.x+b.x)/2,(a.y+b.y)/2);});
 const auto click=[&](const char* id){
  h.Frame();const auto at=cards.at(id);auto& io=ImGui::GetIO();
  io.AddMousePosEvent(at.x,at.y);h.Frame();io.AddMouseButtonEvent(0,true);h.Frame();io.AddMouseButtonEvent(0,false);h.Frame();
 };
 click("table-0");Check(h.shell.Navigation().Choosing(),"After the prompt a click on the table did not open the seat chooser");
 const auto sat=h.actions.size();click("table-0/0");
 Check(h.actions.size()==sat+1&&RoomActionIs(h.actions.back(),room::ActionKind::Queue),"After the prompt a click on Play as P1 did nothing");
 // Rematch chosen, then the room goes away before the room shows the Ready
 // (the host closed it, a kick, a Discord invite): the prompt closes without
 // reading the table it no longer has.
 t.p1=1;t.p2=2;h.view.room.members[0].table=h.view.room.members[1].table=0;h.view.room.members[0].seat=0;h.view.room.members[1].seat=1;
 t.matchGeneration=9;t.phase=room::TablePhase::Playing;t.ready[0]=t.ready[1]=false;++h.view.room.revision;h.Screen("room");h.view.matchResult=false;h.Frame();
 win();Check(h.shell.Navigation().Screen()=="post-match"&&row("again").enabled,"The WIN after a new seat opened no prompt");
 h.Choose("again");h.view.session.room=netplay::RoomState::Closing;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","A chosen Rematch kept the prompt over a room that was closing");
 h.view.session.room=netplay::RoomState::Joined;
 // A direction still held from the fight when the WIN opens the prompt must
 // not keep the confirm pressed beside it from choosing Rematch.
 t.matchGeneration=10;t.phase=room::TablePhase::Playing;h.view.matchResult=false;h.Frame();
 h.view.foughtGame={0,t.matchGeneration,room::MatchResult::Abort};h.view.matchResult=true;h.Frame(MenuInput::Down);
 Check(h.shell.Navigation().Screen()=="post-match"&&h.shell.Navigation().Focus()=="again","The WIN with a direction held opened no prompt on Rematch");
 const auto held=h.actions.size();h.Frame(MenuInput::Down|MenuInput::Select);
 Check(h.actions.size()==held+1&&ReadyIs(h.actions.back(),true),"Rematch pressed beside a held direction was swallowed");
 // Back to room chosen over the WIN keeps the prompt up; the room going away
 // closes it with nothing left to do, so no seat is left once the game is over.
 h.view.session.room=netplay::RoomState::Closing;h.Frame();h.view.session.room=netplay::RoomState::Joined;
 t.matchGeneration=11;t.phase=room::TablePhase::Playing;t.ready[0]=t.ready[1]=false;h.view.matchResult=false;h.Frame();
 win();h.Choose("back");Check(h.shell.Navigation().Screen()=="post-match"&&words["pm-you"]==loc::T("postmatch.back"),"Back over the WIN closed the prompt early");
 const auto gone=h.actions.size();h.view.session.room=netplay::RoomState::Closing;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","Back chosen kept the prompt over a room that was closing");
 h.view.session.room=netplay::RoomState::Joined;gameOver();h.Frame();
 Check(h.actions.size()==gone,"A prompt closed with its room still left the seat afterwards");
 // No answer hides Ember over the WIN, and it stays hidden through the next
 // game: that game's WIN still opens its own prompt.
 const auto hidden=[&]{ImGui::NewFrame();h.shell.Background(h.view,h.view.room,[&](ShellAction a){h.actions.push_back(a);return true;});ImGui::Render();};
 t.matchGeneration=12;t.phase=room::TablePhase::Playing;h.view.matchResult=false;h.Frame();
 win();h.Wait(11);
 Check(!h.open&&h.shell.Navigation().Screen()!="post-match","No answer did not hide Ember over the WIN");
 h.view.matchResult=false;hidden();t.matchGeneration=13;hidden();
 h.open=true;win();
 Check(h.shell.Navigation().Screen()=="post-match","A WIN after one whose prompt closed with Ember hidden opened no prompt");
 SetMenuCardProbe({});
 SetMenuEntriesProbe({});SetMenuCardTextProbe({});
}
