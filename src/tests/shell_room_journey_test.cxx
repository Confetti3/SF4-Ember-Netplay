#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include <algorithm>
#include <iterator>
#include <map>
namespace {
// The retry row a failed save adds to every screen stays the shell's on the
// panels' screens too: the Ember ID panel never takes it, and the rankings and
// replays panels, which do not know it, hand it on to the shell.
void RetrySaveOnPanelScreens() {
 using namespace sf4e;
 for(const char* screen:{"identity","rankings","replays"}){
  Harness h;h.Frame();
  h.Screen("interface");h.Choose("scale");h.Press(MenuInput::Right);
  h.view.settingsError="Disk unavailable";h.Frame(0,45);
  h.Screen(screen);
  const auto mark=static_cast<std::ptrdiff_t>(h.actions.size());
  h.Choose("retry-save");h.Frame();
  const bool saved=std::any_of(h.actions.begin()+mark,h.actions.end(),[](const ShellAction& a){return a.command.kind==Kind::SavePreferences;});
  Check(saved&&h.shell.Navigation().Screen()==screen,"The retry row on a panel's screen did not retry the save");
 }
}
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
static void RoomBoardJourneys() {
 using namespace sf4e;
 Harness h;h.Frame();
 // Back names where it goes: out of Ember from an idle Home, and while a
 // controller is being captured, the cancel it is.
 std::set<std::string> legend;
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});
 SetMenuGlyphs(3,0x40000,0x20000);legend.clear();h.Frame();Check(legend.count("Return to SF4"),"Idle Home's Back does not say it returns to SF4");
 // Outside a room Back at Home still hides Ember, and sends nothing.
 {const auto before=h.actions.size();h.Press(MenuInput::Back);Check(!h.open&&h.shell.Navigation().Screen()=="home"&&h.actions.size()==before,"Back on an idle Home did not hide Ember");h.open=true;}
 h.Screen("player");h.view.inputCapture=input::Capture::ReleaseAll;legend.clear();h.Frame();
 Check(legend.count("Cancel")&&!legend.count("Back"),"Controller assignment's Back does not say it cancels");
 SetMenuTextProbe({});h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="assignment"&&h.actions.back().inputAction==input::Action::Cancel,"Assignment Back did not only cancel capture");
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
 h.Choose("create");h.Choose("host");Check(h.actions.back().command.kind==Kind::HostRoom,"Create journey");
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
 Check(h.actions.back().command.kind==Kind::LeaveRoom,"Cancel while opening did not leave the room");
 h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;h.view.room.roomEpoch=9;h.view.room.localMember=1;h.Frame();
 Check(h.shell.Navigation().Screen()=="room","Joined room did not open the room screen");
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.view.room.roomEpoch=0;h.view.room.localMember=0;h.Frame();h.Screen("home");
 h.Screen("join");h.Choose("invite-text");ImGui::GetIO().AddInputCharactersUTF8("sf4://invitation");h.Frame();
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 h.Choose("join-now");Check(h.actions.back().command.kind==Kind::JoinInvite&&h.actions.back().command.invitation=="sf4://invitation","Join draft journey");
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
 h.Frame();h.Screen("room");h.Press(MenuInput::Right);Check(h.shell.Navigation().Focus()=="member-1","Right did not enter member pane");
 h.Press(MenuInput::Down);Check(h.shell.Navigation().Focus()=="member-2","Member focus order changed");
 h.view.room.members.pop_back();h.Frame();Check(h.shell.Navigation().Focus()=="room-members","Disappeared member did not choose nearest entry");
 h.view.room.members.push_back(peer);h.Press(MenuInput::Left);h.Screen("room");
 h.view.preferences.mainFighter=8;h.view.selectedFighter=0;
 h.view.room.members[1].mainFighter=9;h.view.room.members[1].fighter=2;
 h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;
 std::vector<int> portraits;SetPortraitProbe([&](int fighter,ImVec2,ImVec2){portraits.push_back(fighter);});
 h.Frame();SetPortraitProbe({});
 Check(portraits==std::vector<int>({0,2,8,9}),"Member portraits must use saved mains, not battle fighters");
 h.view.room.tables[0].p1=h.view.room.tables[0].p2=0;
 // A on an empty table opens the seat chooser: P1 left, P2 right, B cancels.
 h.Choose("table-2");Check(h.shell.Navigation().Choosing(),"A on an empty table did not open the seat chooser");
 auto before=h.actions.size();h.Press(MenuInput::Back);
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==before&&h.shell.Navigation().Screen()=="room","B did not cancel the seat chooser");
 h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.back().roomAction.kind==room::ActionKind::Queue&&h.actions.back().roomAction.table==2&&
  h.actions.back().roomAction.seat==1,"Seat chooser did not take the P2 seat");
 // P1 taken under a chooser highlighting P2: the chooser closes, the A pressed
 // on that same frame does nothing, and the next A offers the new options.
 h.Choose("table-1");h.Press(MenuInput::Right);h.view.room.tables[1].p1=2;
 auto raced=h.actions.size();h.Frame(MenuInput::Select);h.Frame();
 Check(h.actions.size()==raced&&!h.shell.Navigation().Choosing(),"A changed chooser sent an option the player never saw");
 h.Press(MenuInput::Select);h.Press(MenuInput::Select);
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.kind==room::ActionKind::Queue&&h.actions.back().roomAction.seat==1,
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
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.seat==1,"A resting cursor overrode the pad's choice");
 // The header button cancels a choice for a mouse, sending nothing.
 click("table-3");Check(h.shell.Navigation().Choosing(),"Clicking a table did not open its chooser");
 raced=h.actions.size();click("menu-back");
 Check(!h.shell.Navigation().Choosing()&&h.actions.size()==raced&&h.shell.Navigation().Screen()=="room","The header did not cancel the chooser");
 click("table-3");click("table-3/1");
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.table==3&&h.actions.back().roomAction.seat==1,"Clicking P2 in the chooser did not take it");
 ImGui::GetIO().AddMousePosEvent(-1,-1);
 SetMenuCardProbe({});h.view.room.tables[3].matchGeneration=0;
 // A full table offers the queue or watching instead.
 h.view.room.tables[3].p1=3;h.view.room.tables[3].p2=4;h.view.room.tables[3].phase=room::TablePhase::Playing;
 h.Choose("table-3");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.back().roomAction.kind==room::ActionKind::Watch&&h.actions.back().roomAction.table==3,"Full-table chooser did not watch");
 h.view.room.tables[3].p1=h.view.room.tables[3].p2=0;h.view.room.tables[3].phase=room::TablePhase::Idle;
 // Y opens the table's options; the list still queues and watches.
 h.Screen("room");h.Choose("table-2");h.Press(MenuInput::Back);h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Y did not open the table options");
 h.Choose("queue");Check(h.actions.back().roomAction.kind==room::ActionKind::Queue&&h.actions.back().roomAction.table==2,"Table queue journey");
 h.Choose("watch");Check(h.actions.back().roomAction.kind==room::ActionKind::Watch,"Watch journey");
 h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="room","Y again did not return to the board");
 h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room-chat","View did not open chat");
 h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room","View again did not return to the board");
 // The members hide from their heading and come back, and the choice is
 // saved on its own, as a room locks the other settings.
 h.view.canEditPreferences=false;h.view.atMainMenu=true;
 h.Choose("members-part");h.Frame(0,45);
 Check(h.actions.back().roomSettings&&h.actions.back().preferences.roomMembersHidden,"Hiding the members did not save in the room");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("members-part");h.Frame(0,45);
 Check(h.actions.back().roomSettings&&!h.actions.back().preferences.roomMembersHidden,"Showing the members did not save in the room");
 h.view.preferences=h.actions.back().preferences;h.view.canEditPreferences=true;h.Frame();
 // X opens the real fighter selector, which hands its shortcuts back: X
 // there returns to the board and View goes on to chat.
 FighterSelector selector;selection::Pick pick;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="selection","X did not open fighter selection");
 // The selector names the board as its way back and shows the room's shortcuts.
 Check(EmbeddedReturnContext().exitName=="Room"&&EmbeddedReturnContext().shortcutHints.size()==3,
  "The fighter selector does not know it returns to the room or which shortcuts it forwards");
 // X opens the selector on the roster, to change the fighter; leaving it on
 // another sub-page and coming back by X opens the roster again.
 Check(selector.Navigation().Screen()=="roster","X did not open the selector on the roster");
 selector.Navigation().Home();selector.Navigation().Push("appearance");selector.Navigation().Push("costumes");h.Frame();
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="room","X on a selector sub-page did not return to the board");
 h.Press(MenuInput::Fighter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="roster","X reopened the selector on the sub-page it was left on");
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="room","X in fighter selection did not return to the board");
 h.Press(MenuInput::Fighter);h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room-chat","View in fighter selection did not open chat");
 h.Press(MenuInput::Chat);h.selection=[]{};h.view.canEditSelection=false;
 // B on the board with no seat goes to Home, and B there does nothing rather
 // than dropping to the game's own menu.
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="home","B on the board did not reach Home");
 // Home in a room does not offer a way back to the game, and Back there does
 // nothing: Ember stays up, the room is kept and nothing is sent, because the
 // game's own menu behind it is locked.
 legend.clear();SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});h.Frame();SetMenuTextProbe({});
 Check(!legend.count("Return to SF4")&&!legend.count("Back")&&legend.count("Select"),"Home in a room offers Back, or lost its Select");
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
 Check(h.actions.size()==postMatchActions+1&&h.actions.back().command.kind==Kind::Rematch,"Pending result dropped the rematch press");
 // A Ready the runtime still holds (while the last game closes) is taken
 // back with Unready, which does not wait for the last game's receipt.
 h.view.readyPress=netplay::ReadyPress::Held;h.view.room.localTerminalPending=true;h.Frame();
 Check(row("ready").label=="Unready / unlock fighter"&&row("ready").enabled,"In-flight Ready cannot be taken back");
 Check(tableStatus.find("Readying up")!=std::string::npos,"In-flight Ready is not shown on the seat line");
 h.Choose("ready");
 Check(h.actions.back().roomAction.kind==room::ActionKind::Unready&&h.actions.back().roomAction.seat==0,"Unready of an in-flight Ready was refused");
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
 h.Choose("ready");Check(h.actions.back().command.kind==Kind::Ready,"Ready journey");
 h.view.room.tables[2].ready[0]=true;h.Choose("ready");Check(h.actions.back().roomAction.kind==room::ActionKind::Unready,"Unready journey");
 // On the board, A on your own seat readies and unreadies, and one B leaves
 // the seat, ready or not. While the game is starting B takes Ready back
 // instead, and the legend says which.
 h.Screen("room");h.view.room.tables[2].ready[0]=false;h.Choose("table-2");
 Check(h.actions.back().command.kind==Kind::Ready,"A on your seat did not ready");
 h.view.room.tables[2].ready[0]=true;h.Press(MenuInput::Select);
 Check(h.actions.back().roomAction.kind==room::ActionKind::Unready,"A again did not unready");
 h.Press(MenuInput::Back);Check(h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&h.actions.back().roomAction.table==2&&
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
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&h.actions.back().roomAction.table==2,
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
 // state on the strip, A does nothing, and the legend has no A.
 std::vector<MenuEntry> boardRows;std::set<std::string> boardLegend;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){boardRows=rows;});
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))boardLegend.insert(id+7);});
 const auto seatCard=[&]()->const MenuEntry&{return *std::find_if(boardRows.begin(),boardRows.end(),[](const MenuEntry& e){return e.id=="table-2";});};
 seatTable.ready[0]=false;h.Frame();
 Check(!seatCard().info&&seatCard().hint==loc::T("room.ready_up"),"A seat that can ready lost its Ready action");
 seatTable.p2=0;h.Frame();
 Check(seatCard().info&&seatCard().hint==loc::T("room.ready_up"),"A seat with no opponent offered a Ready that cannot be sent");
 seatTable.p2=2;
 for(const auto phase:{room::TablePhase::Ready,room::TablePhase::Playing,room::TablePhase::Paused}){
  seatTable.phase=phase;h.Frame();
  Check(seatCard().info&&!seatCard().hint.empty(),"A seat with nothing to send still offered an action");
  boardLegend.clear();h.Frame();
  Check(!boardLegend.count(seatCard().hint),"The legend named a state as A's action");
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
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.kind==room::ActionKind::Unready&&h.shell.Navigation().Screen()=="room",
  "B while the start is held did not unready, or left the board");
 h.Press(MenuInput::Select);Check(h.actions.back().roomAction.kind==room::ActionKind::Unready,"A while the start is held did not unready");
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
 // Walking down past other tables to the toolbar still opens your own table.
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
 Check(h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&h.shell.Navigation().Screen()=="room","B did not leave the queue");
 h.view.room.members[0].seat=0;h.view.room.tables[2].p1=1;h.view.room.tables[2].queue.clear();
 h.Screen("room-table");
 h.view.room.tables[2].ready[0]=false;h.view.session.match=netplay::MatchState::PostMatch;
 h.Choose("ready");Check(h.actions.back().command.kind==Kind::Rematch,"Rematch journey");
 h.view.room.tables[2].phase=room::TablePhase::Paused;h.Choose("cancel-result");
 auto count=h.actions.size();++h.view.room.tables[2].matchGeneration;h.Frame();h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==count,"Stale confirmation cancelled a different game");h.Press(MenuInput::Back);
 h.Screen("room");h.Choose("leave");count=h.actions.size();h.Press(MenuInput::Select);Check(h.actions.size()==count,"Leave default was not Cancel");
 h.Choose("leave");h.Press(MenuInput::Right);h.Press(MenuInput::Select);Check(h.actions.size()==count+1&&h.actions.back().command.kind==Kind::LeaveRoom,"Confirmed leave");
  h.Screen("room");h.Choose("room-members");h.Choose("member-2");h.Choose("kick");
  h.view.room.members.pop_back();h.Frame();h.Press(MenuInput::Right);h.Press(MenuInput::Select);Check(h.actions.back().command.kind==Kind::LeaveRoom,"Removed member was kicked");
 h.Screen("room");h.Choose("room-chat");h.Frame(0,4);ImGui::GetIO().AddInputCharactersUTF8("Hello");h.Frame();
 count=h.actions.size();h.Press(MenuInput::Down);Check(h.actions.size()==count,"Scrolling the chat sent it");
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 Check(h.actions.size()==count+1&&h.actions.back().roomAction.kind==room::ActionKind::Chat&&h.actions.back().roomAction.text=="Hello","Enter in the chat box did not send");
  h.view.session.control=netplay::Health::Lost;h.Screen("room");count=h.actions.size();h.Choose("table-2");Check(h.actions.size()==count,"Lost connection submitted Ready");
 h.view={};h.view.controllerReady=h.view.canEditPreferences=h.view.canOpenRoom=h.view.lobbyMusicAvailable=true;h.Frame();h.Screen("interface");h.Choose("hud");h.Press(MenuInput::Left);
 count=h.actions.size();h.Frame(0,20);Check(h.actions.size()==count,"Autosave not coalesced");
 h.Frame(0,20);Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.showMatchHud,"Autosave did not queue");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Screen("player");h.Choose("background-play");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.backgroundPlay,"Play in the background did not save");
 Check(h.actions.back().preferences.endlessLobbyMusic,"Endless lobby music was not on by default");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Screen("interface");h.Choose("lobby-music");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.endlessLobbyMusic,"Turning off endless lobby music did not save");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Screen("player");h.Choose("faster-display");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.fasterDisplay&&h.actions.back().preferences.backgroundPlay,"Faster display did not save off");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Screen("player");h.Choose("borderless");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.borderless&&!h.actions.back().preferences.fasterDisplay,"Borderless did not save on");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Screen("settings");h.Choose("training-replays");h.Choose("replay-save-watched");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.recordWatched,"Save matches you watch did not save");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 // Show my Steam profile to room members: on until the player turns it off, saved with the other preferences.
 Check(h.view.preferences.shareSteamProfile,"Steam profile sharing was not on by default");
 h.Screen("profile");h.FocusOn("share-steam");
 h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.shareSteamProfile,"Hiding the Steam profile did not save");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.shareSteamProfile,"Showing the Steam profile did not save");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Screen("interface");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-size");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudSize==1,"HUD size did not save"); // Small by default; Right steps to Standard.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-layout");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudLayout==0,"HUD layout did not save"); // Split by default; Left steps to the Ember strip.
 {std::vector<MenuEntry> stripRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){stripRows=rows;});h.view.preferences=h.actions.back().preferences;h.Frame();SetMenuEntriesProbe({});
  const auto offset=std::find_if(stripRows.begin(),stripRows.end(),[](const MenuEntry& e){return e.id=="hud-name-offset";});
  Check(offset!=stripRows.end()&&!offset->enabled,"Name height is offered for the Ember strip, which has no name plates");}
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudLayout==1,"HUD layout did not step back to Split");
 // Name height: Left moves the plates up two units at a time, to the limit and no further, and Right back down.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-name-offset");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudNameOffset==-2,"Name height did not save");
 for(int i=0;i<40;++i){h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);}
 Check(h.actions.back().preferences.matchHudNameOffset==-netplay::MaxMatchHudNameOffset,"Name height went past its limit");
 for(int i=0;i<30;++i){h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);}
 Check(h.actions.back().preferences.matchHudNameOffset==0,"Name height did not step back to Default");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-position");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudAnchor==1,"HUD position did not save"); // Bottom center by default; Right steps to Bottom left.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudAnchor==0,"HUD position did not step back to Bottom center");
 // Right steps through the positions to Top center (the sixth) and stops there; Left comes back.
 for(int i=0;i<netplay::MaxMatchHudAnchor+3;++i){h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);}
 Check(h.actions.back().preferences.matchHudAnchor==5,"HUD position did not reach Top center, or went past it");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudAnchor==4,"HUD position did not step back from Top center");
 for(int i=0;i<netplay::MaxMatchHudAnchor+2;++i){h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);}
 Check(h.actions.back().preferences.matchHudAnchor==0,"HUD position did not return to Bottom center");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-spacing");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudRaised,"HUD spacing did not save");
 // A new profile is on Auto. Right from Auto starts at 0 frames, and Left from 0 returns to Auto, keeping the number.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Screen("defaults");h.Choose("delay");
 Check(h.view.preferences.autoInputDelay,"A new profile did not start on Auto");
 h.Press(MenuInput::Right);h.Frame(0,45);
 Check(!h.actions.back().preferences.autoInputDelay&&h.actions.back().preferences.inputDelay==0,"Leaving Auto did not start at 0 frames");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().preferences.autoInputDelay&&h.actions.back().preferences.inputDelay==0,"Auto delay did not save");
 h.Screen("interface");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("scale");h.Press(MenuInput::Right);
 h.view.settingsError="Disk unavailable";h.Frame(0,45);
 {std::vector<MenuEntry> saveRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){saveRows=rows;});h.Frame();SetMenuEntriesProbe({});
  const auto retry=std::find_if(saveRows.begin(),saveRows.end(),[](const MenuEntry& e){return e.id=="retry-save";});
  Check(retry!=saveRows.end()&&retry->detail==loc::T("error.settings_not_saved"),"The retry row shows the store's English diagnostic, not the catalog's message");}
 h.Choose("retry-save");h.Frame();
 Check(h.actions.back().command.kind==Kind::SavePreferences,"Save retry missing");
 h.view.settingsError.clear();h.view.preferences=h.actions.back().preferences;h.Frame();
 h.view.discordPending=h.view.discordConfirm=h.view.discordCanSwitch=true;h.view.discordRevision=9;h.Frame();
 h.Choose("invite-switch");h.Press(MenuInput::Select);Check(h.actions.back().discordAction==discord::InviteAction::None,"Switch default not Cancel");
 h.Choose("invite-cancel");Check(h.actions.back().discordAction==discord::InviteAction::Cancel&&h.actions.back().discordRevision==9,"Discord cancellation");
 h.view.discordPending=false;h.Frame();h.Screen("home");h.Choose("profile");h.Choose("main-character");h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,40);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.mainFighter==sf4e::selection::RosterDisplayOrder[1],"Profile main was not saved");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 h.Screen("profile");h.Choose("main-character");count=h.actions.size();
 h.Press(MenuInput::Right);
 ControllerNavigation profilePad;
 const auto physical=[&](unsigned mapped,unsigned raw){
  ControllerSample sample{3,0,true,ControllerButtons(mapped,3,raw)};
  profilePad.Update(sample,true,true,true);h.Frame(profilePad.Buttons());
 };
 physical(0,0);physical(0,0);physical(0x40,0x40000);physical(0,0);h.Frame(0,40);
 Check(h.actions.size()>count&&h.actions.back().preferences.mainFighter==sf4e::selection::RosterDisplayOrder[2],"Physical A did not save profile portrait");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 Check(h.shell.Navigation().Screen()=="profile","Accepted portrait save did not return to Profile");
 h.Choose("main-character");count=h.actions.size();
 ImVec2 mouseTarget;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){if(std::strcmp(id,"main-3")==0)mouseTarget=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);});
 h.Frame();SetMenuCardProbe({});
 auto& mouse=ImGui::GetIO();mouse.AddMousePosEvent(mouseTarget.x,mouseTarget.y);h.Frame();
 mouse.AddMouseButtonEvent(0,true);h.Frame();mouse.AddMouseButtonEvent(0,false);h.Frame(0,40);
 Check(h.actions.size()>count&&h.actions.back().preferences.mainFighter==3,"Mouse click did not save profile portrait");
 h.view.preferences=h.actions.back().preferences;h.Frame();Check(h.shell.Navigation().Screen()=="profile","Mouse portrait save did not confirm");
 // A rejected save must leave a labelled, actionable retry in the portrait grid.
 h.Choose("main-character");h.Press(MenuInput::Right);h.accept=false;h.Press(MenuInput::Select);h.Frame(0,40);
 Check(h.shell.Navigation().Screen()=="main-character","Failed portrait save closed the roster");
 bool retryLabel=false;
 SetMenuTextProbe([&](const char* id,float,float,float width,float available){
  if(std::strcmp(id,"retry-save")==0)retryLabel=width>0&&width<=available;
 });
 h.Frame();SetMenuTextProbe({});
 Check(retryLabel,"Portrait save retry has no visible label");
 h.accept=true;h.Choose("retry-save");h.Frame();
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.mainFighter==DisplayedAfter(3),"Portrait retry lost the selected main");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 Check(h.shell.Navigation().Screen()=="profile","Retried portrait save did not return to Profile");
}
// Each journey owns a Harness and with it the ImGui context, so they run one
// after another: a Harness built while another is alive destroys the current
// context on its way out and leaves the outer one without any.
void RoomJourneys() {
 RoomBoardJourneys();
 RetrySaveOnPanelScreens();
 RoomShortcutOnPanelScreen();
}

void ArcadeJourneys() {
 using namespace sf4e;
 {
  // An Arcade room's Queue names a cabinet (table + seat * TableCount), not the
  // table its pair plays at: a member paired at table 0 still moves to cabinet 1.
  Harness a;JoinedRoom(a,14);a.view.session.generation.room=1;a.view.session.control=netplay::Health::Healthy;
  a.view.room.host=1;a.view.room.revision=3;a.view.room.arcade=room::ArcadeRoom;
  room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;a.view.room.members.push_back(local);
  room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;a.view.room.members.push_back(peer);
  a.view.room.tables[0].p1=1;a.view.room.tables[0].p2=2;a.view.room.cabinets[1]=1;a.view.room.cabinets[2]=2;
  a.Screen("room");const auto sent=a.actions.size();a.Choose("cabinet-0");
  Check(a.actions.size()==sent+1&&a.actions.back().roomAction.kind==room::ActionKind::Queue&&a.actions.back().roomAction.table==0&&
   a.actions.back().roomAction.seat==0,"A paired member could not move to cabinet 1");
 }
 {
  // King of the hill after a set: nothing to choose, the set's winner stays on
  // and its loser waits, and no answer keeps the seat.
  Harness k;JoinedRoom(k,15);k.view.session.generation.room=1;k.view.session.control=netplay::Health::Healthy;
  k.view.room.host=1;k.view.room.revision=3;k.view.room.arcade=room::KingRoom;
  room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;k.view.room.members.push_back(local);
  room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;k.view.room.members.push_back(peer);
  auto& t=k.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Playing;
  k.view.room.cabinets[0]=1;k.view.room.cabinets[8]=2;
  k.Screen("room");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
  const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
  t.lastSet.generation=5;t.lastSet.p1=1;t.lastSet.p2=2;t.lastSet.winnerSeat=0;t.score[0]=t.score[1]=0;
  k.view.session.match=netplay::MatchState::Playing;k.view.atMainMenu=false;k.view.matchResult=true;k.Frame();
  Check(k.shell.Navigation().Screen()=="post-match"&&row("again")==rows.end()&&row("back")!=rows.end(),"King of the hill offered a Rematch after a set");
  Check(row("pm-you")->value==loc::T("postmatch.king.stays")&&row("pm-opponent")->value==loc::T("postmatch.king.waits"),"The set's winner and loser were not shown");
  Check(row("back")->detail==loc::T("postmatch.back.keep_seat"),"Back after a King of the hill set said it gives up the seat");
  const auto sent=k.actions.size();k.Wait(11);
  k.view.matchResult=false;k.view.session.match=netplay::MatchState::PostMatch;k.view.atMainMenu=true;t.phase=room::TablePhase::Waiting;k.open=true;k.Frame(0,5);
  Check(std::none_of(k.actions.begin()+sent,k.actions.end(),[](const ShellAction& a){return a.roomAction.kind==room::ActionKind::Unqueue;}),"No answer after a King of the hill set left the cabinet");
  SetMenuEntriesProbe({});
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
 const auto win=[&]{h.view.session.match=netplay::MatchState::Playing;h.view.atMainMenu=false;h.view.matchResult=true;h.Frame();};
 const auto gameOver=[&]{h.view.matchResult=false;h.view.session.match=netplay::MatchState::PostMatch;h.view.atMainMenu=true;
  t.phase=room::TablePhase::Waiting;h.open=true;h.Frame();};
 win();
 Check(h.shell.Navigation().Screen()=="post-match"&&row("again").label==loc::T("postmatch.rematch")&&row("again").enabled,"WIN did not open the post-match prompt");
 h.Choose("again");Check(h.actions.back().command.kind==Kind::Rematch,"Rematch did not send the room's Ready");
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
 Check(row("again").enabled&&row("pm-you").value!=loc::T("postmatch.state.ready"),"The Ready that started the game was taken for a Rematch");
 h.Choose("again");h.view.rematchChoices=3;h.Frame();
 h.view.rematchChoices=0;h.view.matchResult=false;h.view.gamesInPlace=1;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","A rematch in place did not end the prompt");
 win();Check(h.shell.Navigation().Screen()=="post-match"&&row("again").enabled,"The next WIN in the same battle opened no prompt");
 h.view.matchResult=false;h.view.gamesInPlace=0;t.ready[0]=t.ready[1]=false;h.Frame();
 // No answer with no set length: Ember steps aside over WIN, then leaves the seat in the room.
 h.view.matchResult=false;h.Frame();win();const auto before=h.actions.size();h.Wait(11);
 Check(!h.open&&h.shell.Navigation().Screen()!="post-match"&&h.actions.size()==before,"No answer did not close the prompt over WIN");
 gameOver();h.Frame();
 Check(h.actions.size()==before+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue,"No answer after the game did not leave the seat");
 // Back to room over the WIN: the prompt shows that choice until the game is
 // over, then leaves the seat. The game over, the result does not open a new prompt.
 h.view.matchResult=false;h.Frame();win();const auto backed=h.actions.size();h.Choose("back");h.Wait(2);
 Check(h.open&&h.shell.Navigation().Screen()=="post-match"&&row("pm-you").value==loc::T("postmatch.back")&&!row("back").enabled&&h.actions.size()==backed,
  "Back to room over the WIN did not keep the choice shown");
 gameOver();h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match"&&h.actions.size()==backed+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue,
  "Back to room did not leave the seat once the game was over");
 h.view.matchResult=true;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","A result after the game was over opened the prompt over the room");
 h.view.matchResult=false;h.Frame();
 // Inside a set: Next game, and Back keeps the seat.
 t.rules.format=room::SetFormat::Ft2;t.matchGeneration=7;t.phase=room::TablePhase::Playing;h.view.room.tables[0].queue={3};
 win();Check(row("again").label==loc::T("postmatch.next_game")&&row("again").enabled,"A set's game did not offer Next game despite the queue");
 const auto midSet=h.actions.size();h.Press(MenuInput::Back);gameOver();h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match"&&h.actions.size()==midSet,"Back inside a set left the seat");
 // The set over, someone queued: no rematch.
 t.matchGeneration=8;t.phase=room::TablePhase::Playing;t.lastSet.generation=8;t.lastSet.p1=1;t.lastSet.p2=2;h.view.matchResult=false;h.Frame();win();
 Check(row("again").label==loc::T("postmatch.rematch")&&!row("again").enabled&&row("again").detail==loc::T("postmatch.queue_waiting"),
  "Rematch was offered while someone waits in the queue");
 // A WIN cut short: the game is over before anyone answered and the opponent
 // has left. Then the room board takes a seat by mouse.
 gameOver();t.p2=0;h.view.room.members[1].table=h.view.room.members[1].seat=-1;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","The prompt stayed over the room after the game was over");
 t.p1=0;t.queue.clear();h.view.room.members[0].table=h.view.room.members[0].seat=-1;++h.view.room.revision;h.Frame();h.Frame();
 std::map<std::string,ImVec2> cards;
 SetMenuCardProbe([&](const char* id,ImVec2 a,ImVec2 b){cards[id]=ImVec2((a.x+b.x)/2,(a.y+b.y)/2);});
 const auto click=[&](const char* id){
  h.Frame();const auto at=cards.at(id);auto& io=ImGui::GetIO();
  io.AddMousePosEvent(at.x,at.y);h.Frame();io.AddMouseButtonEvent(0,true);h.Frame();io.AddMouseButtonEvent(0,false);h.Frame();
 };
 click("table-0");Check(h.shell.Navigation().Choosing(),"After the prompt a click on the table did not open the seat chooser");
 const auto sat=h.actions.size();click("table-0/0");
 Check(h.actions.size()==sat+1&&h.actions.back().roomAction.kind==room::ActionKind::Queue,"After the prompt a click on Play as P1 did nothing");
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
 h.view.matchResult=true;h.Frame(MenuInput::Down);
 Check(h.shell.Navigation().Screen()=="post-match"&&h.shell.Navigation().Focus()=="again","The WIN with a direction held opened no prompt on Rematch");
 const auto held=h.actions.size();h.Frame(MenuInput::Down|MenuInput::Select);
 Check(h.actions.size()==held+1&&h.actions.back().command.kind==Kind::Rematch,"Rematch pressed beside a held direction was swallowed");
 // Back to room chosen over the WIN keeps the prompt up; the room going away
 // closes it with nothing left to do, so no seat is left once the game is over.
 h.view.session.room=netplay::RoomState::Closing;h.Frame();h.view.session.room=netplay::RoomState::Joined;
 t.matchGeneration=11;t.phase=room::TablePhase::Playing;t.ready[0]=t.ready[1]=false;h.view.matchResult=false;h.Frame();
 win();h.Choose("back");Check(h.shell.Navigation().Screen()=="post-match"&&row("pm-you").value==loc::T("postmatch.back"),"Back over the WIN closed the prompt early");
 const auto gone=h.actions.size();h.view.session.room=netplay::RoomState::Closing;h.Frame();
 Check(h.shell.Navigation().Screen()!="post-match","Back chosen kept the prompt over a room that was closing");
 h.view.session.room=netplay::RoomState::Joined;gameOver();h.Frame();
 Check(h.actions.size()==gone,"A prompt closed with its room still left the seat afterwards");
 SetMenuCardProbe({});
 SetMenuEntriesProbe({});
}
