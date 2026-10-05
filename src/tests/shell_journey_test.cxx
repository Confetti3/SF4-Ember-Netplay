#include "shell_journey_support.hxx"
#include "shell_chat_journey.hxx"
#include <algorithm>
#include <iterator>
namespace {
// The fighter drawn right of `fighter` in USFIV's select order, which is where Right goes.
int DisplayedAfter(int fighter){const int* at=std::find(std::begin(sf4e::selection::RosterDisplayOrder),std::end(sf4e::selection::RosterDisplayOrder),fighter);return at[1];}
void Journeys() {
 using namespace sf4e;
 Harness h;h.Frame();
 // Back names where it goes: out of Ember from an idle Home, and while a
 // controller is being captured, the cancel it is.
 std::set<std::string> legend;
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});
 SetMenuGlyphs(3,0x40000,0x20000);legend.clear();h.Frame();Check(legend.count("Return to SF4"),"Idle Home's Back does not say it returns to SF4");
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
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Test room";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
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
 // B on the board with no seat goes to Home, and B there returns to the room
 // rather than dropping to the game's own menu.
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="home","B on the board did not reach Home");
 // Home in a room says Back returns to the game, and it hides Ember without
 // leaving the room or sending anything.
 legend.clear();SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});h.Frame();SetMenuTextProbe({});
 Check(legend.count("Return to SF4"),"Home in a room does not say Back returns to SF4");
 raced=h.actions.size();h.Press(MenuInput::Back);
 Check(!h.open&&h.shell.Navigation().Screen()=="home"&&h.actions.size()==raced&&h.view.session.room==netplay::RoomState::Joined,
  "B at Home in a room did not hide Ember, or sent a command");
 h.open=true;h.Screen("room");
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
 h.view.readyRequested=true;h.Frame();
 Check(row("ready").label=="Readying up..."&&!row("ready").enabled,"In-flight Ready still offers a second press");
 Check(tableStatus.find("Readying up")!=std::string::npos,"In-flight Ready is not shown on the seat line");
 h.view.readyRequested=false;
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
 h.Press(MenuInput::Back);Check(!h.open&&h.shell.Navigation().Screen()=="home","B at Home did not hide Ember");
 h.open=true;h.Screen("room");
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
 h.view={};h.view.controllerReady=h.view.canEditPreferences=h.view.canOpenRoom=true;h.Frame();h.Screen("interface");h.Choose("hud");h.Press(MenuInput::Left);
 count=h.actions.size();h.Frame(0,20);Check(h.actions.size()==count,"Autosave not coalesced");
 h.Frame(0,20);Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.showMatchHud,"Autosave did not queue");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-size");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudSize==1,"HUD size did not save"); // Small by default; Right steps to Standard.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-layout");h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudLayout==0,"HUD layout did not save"); // Split by default; Left steps to the Ember strip.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudLayout==1,"HUD layout did not step back to Split");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-position");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudAnchor==1,"HUD position did not save"); // Bottom center by default; Right steps to Bottom left.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudAnchor==0,"HUD position did not step back to Bottom center");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-spacing");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudRaised,"HUD spacing did not save");
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
// A keyboard player reaches everything a pad does: F, T and C are X, Y and
// View, Escape always goes back, and Delete leaves the seat that B leaves.
void KeyboardJourneys(){
 using namespace sf4e;
 Harness h;h.Frame();
 auto& io=ImGui::GetIO();
 const auto key=[&](ImGuiKey k){h.Frame();io.AddKeyEvent(k,true);h.Frame();io.AddKeyEvent(k,false);h.Frame();};
 SetMenuGlyphs(input::PadKeyboard,0,0);
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Keys";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 h.view.room.tables[0].p1=1;
 h.Screen("room");h.FocusOn("table-0");
 std::set<std::string> legend;
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});
 h.Frame();SetMenuTextProbe({});
 Check(KeyboardPrompts(),"A keyboard player does not see keyboard prompts");
 Check(legend.count(loc::T("room.leave_seat"))&&legend.count(loc::T("room.legend_fighter"))&&legend.count(loc::T("common.back")),
  "The keyboard legend lacks Delete's Leave seat, F's Fighter or Escape's Back");
 // Escape on your own card goes back to Home and keeps the seat.
 auto before=h.actions.size();key(ImGuiKey_Escape);
 Check(h.shell.Navigation().Screen()=="home"&&h.actions.size()==before,"Escape on your own card left the seat");
 h.Screen("room");h.FocusOn("table-0");
 // Delete there leaves it, like a pad's B.
 key(ImGuiKey_Delete);
 Check(h.actions.size()==before+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue,"Delete on your own card did not leave the seat");
 // Delete anywhere else does nothing.
 before=h.actions.size();h.FocusOn("copy");key(ImGuiKey_Delete);
 Check(h.actions.size()==before&&h.shell.Navigation().Screen()=="room","Delete away from your card did something");
 // T and C open table options and chat, and again return to the board.
 key(ImGuiKey_T);Check(h.shell.Navigation().Screen()=="room-table","T did not open the table options");
 key(ImGuiKey_T);Check(h.shell.Navigation().Screen()=="room","T again did not return to the board");
 key(ImGuiKey_C);Check(h.shell.Navigation().Screen()=="room-chat","C did not open chat");
 // The message box has the keyboard from the start, so F, T, C and Backspace
 // are text and neither open anything nor cancel the draft; Escape goes back.
 h.Frame(0,4);io.AddInputCharactersUTF8("fct");h.Frame();key(ImGuiKey_F);key(ImGuiKey_T);key(ImGuiKey_Backspace);key(ImGuiKey_Space);
 Check(h.shell.Navigation().Screen()=="room-chat","Typing in chat pressed a menu key");
 key(ImGuiKey_Escape);Check(h.shell.Navigation().Screen()=="room","Escape did not return to the board");
 // F opens fighter select on the roster, at the current fighter; picking
 // one goes on to its Ultra, and the Ultra returns to the room.
 FighterSelector selector;selection::Pick pick;pick.fighter=4;pick.edition=14;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 key(ImGuiKey_F);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="roster"&&selector.Navigation().Focus()=="fighter-4",
  "F did not open the roster at the current fighter");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(pick.fighter==DisplayedAfter(4)&&selector.Navigation().Screen()=="ultra","Picking a fighter did not go on to its Ultra");
 key(ImGuiKey_RightArrow);key(ImGuiKey_KeypadEnter);
 Check(pick.ultra==1&&h.shell.Navigation().Screen()=="room","Picking the Ultra did not return to the room");
 // Back from the roster returns to the room as well.
 key(ImGuiKey_F);key(ImGuiKey_Backspace);
 Check(h.shell.Navigation().Screen()=="room","Back from the roster did not return to the room");
 // Select on the table page's Ultra opens the Ultra cards, and picking one
 // returns to the table page.
 h.view.ultraSteps=true;h.view.ultraName="Ultra II";key(ImGuiKey_T);h.FocusOn("ultra");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="ultra"&&selector.Navigation().Focus()=="ultra-1",
  "Select on the table's Ultra did not open the Ultra cards at the saved one");
 key(ImGuiKey_LeftArrow);key(ImGuiKey_Enter);
 Check(pick.ultra==0&&h.shell.Navigation().Screen()=="room-table","Picking an Ultra did not return to the table page");
 // Select on the table page's Appearance opens the costume cards at the saved
 // costume; a costume goes on to its colors, and a color returns to the table page.
 selection::Availability available;available.ready=true;available.costumes=3;available.colors[0]=available.colors[1]=5;available.personalActions=1;
 h.selection=[&]{selector.Draw(pick,false,nullptr,[&](int){return available;},nullptr,true);};
 h.view.appearanceName="Original / Color 1";h.FocusOn("appearance");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="costumes"&&selector.Navigation().Focus()=="costume-0",
  "Select on the table's Appearance did not open the costume cards at the saved one");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(pick.costume==1&&selector.Navigation().Screen()=="colors"&&selector.Navigation().Focus()=="color-"+std::to_string(pick.color),
  "Picking a costume did not go on to its colors at the kept color");
 const auto colors=selection::AllowedColors(pick.fighter,1,available);
 Check(colors.size()>1&&pick.color==colors[0],"The test costume has too few colors");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(pick.color==colors[1]&&h.shell.Navigation().Screen()=="room-table","Picking a color did not return to the table page");
 // Back from the colors goes to the costumes, and Back from there to the table page.
 h.FocusOn("appearance");key(ImGuiKey_Enter);key(ImGuiKey_Enter);
 Check(selector.Navigation().Screen()=="colors","Picking the saved costume again did not open its colors");
 key(ImGuiKey_Backspace);Check(selector.Navigation().Screen()=="costumes","Back from the colors did not return to the costumes");
 key(ImGuiKey_Backspace);Check(h.shell.Navigation().Screen()=="room-table","Back from the costumes did not return to the table page");
 Check(pick.costume==1&&pick.color==colors[1],"Backing out of the galleries changed the pick");
 // The rest of the pick is on the table page too: Additional options opens its
 // page (personal action, win quote and the others), and Back returns.
 h.FocusOn("fighter-options");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="options",
  "Select on the table's Additional options did not open the options page");
 key(ImGuiKey_Backspace);Check(h.shell.Navigation().Screen()=="room-table","Back from the options page did not return to the table page");
 // P1 opens the stage cards at the saved stage, and a pick returns to the table page.
 int stage=0;h.view.localSlot=0;h.view.stageName="Random";
 h.selection=[&]{selector.Draw(pick,false,nullptr,[&](int){return available;},&stage,true);};
 // Stage shows its value but steps nothing in place: Left and Right stay here.
 h.FocusOn("stage");key(ImGuiKey_RightArrow);key(ImGuiKey_LeftArrow);
 Check(h.shell.Navigation().Screen()=="room-table"&&stage==0,"Left or Right on the table's Stage left the table page");
 key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="stage",
  "Select on the table's Stage did not open the stage cards");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(stage!=0&&h.shell.Navigation().Screen()=="room-table","Picking a stage did not return to the table page");
 key(ImGuiKey_Escape);
 // A pad press puts the pad's prompts back; a key brings the keys again.
 SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B);
 h.Press(MenuInput::Down);Check(!KeyboardPrompts(),"A pad press left keyboard prompts up");
 key(ImGuiKey_UpArrow);Check(KeyboardPrompts(),"A key press kept the pad's prompts");
 // Typing in a text field counts too, though its keys press no menu bit.
 h.Press(MenuInput::Down);Check(!KeyboardPrompts(),"A pad press left keyboard prompts up");
 h.Screen("room-chat");h.Frame(0,4);
 io.AddInputCharactersUTF8("x");h.Frame();h.Frame();
 Check(KeyboardPrompts(),"Typing with a pad assigned kept the pad's prompts");
 key(ImGuiKey_Escape);
 h.selection=[]{};
}
void PresentationJourneys(){
 Check(MenuScreenLabel("room-members")=="Members"&&MenuScreenLabel("player")=="Player & controller","Internal screen keys leaked into Back labels");
 auto text=TextRow("name","Name","Player",31);auto value=Value("delay","Delay","2","Frames");
 auto confirm=ConfirmRow("leave","Leave room","Disconnect");
 Check(std::strcmp(MenuPrimaryHint(&text),"Edit")==0&&MenuPrimaryHint(&value)==nullptr&&std::strcmp(MenuPrimaryHint(&confirm),"Review")==0,"Contextual legend does not match action");
 text.enabled=false;Check(MenuPrimaryHint(&text)==nullptr,"Disabled field advertises submission");
 Harness h;std::string status;SetMenuStatusProbe([&](const char* text,Tone){status=text;});
 h.view.preferences.showMatchHud=true;h.Frame(); // The HUD defaults to off; the click below must change a saved value.
 h.Screen("interface");ImVec2 leftArrow,otherRow;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){
  if(std::strcmp(id,"hud")==0)leftArrow=ImVec2(min.x+(max.x-min.x)*.75f,(min.y+max.y)*.5f);
  if(std::strcmp(id,"scale")==0)otherRow=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);
 });h.Frame();SetMenuCardProbe({});
 auto& io=ImGui::GetIO();io.AddMousePosEvent(otherRow.x,otherRow.y);h.Frame();
 Check(h.shell.Navigation().Focus()=="hud","Mouse hover stole controller focus");
 io.AddMousePosEvent(leftArrow.x,leftArrow.y);h.Frame();io.AddMouseButtonEvent(0,true);h.Frame();
 io.AddMouseButtonEvent(0,false);h.Frame(0,40);
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.showMatchHud,"Visible value arrows did not adjust on click");
 Check(status=="Saving...","Queued settings falsely reported Saved before acknowledgement");
 h.view.preferences=h.actions.back().preferences;h.Frame();Check(status=="Saved","Acknowledged settings did not report Saved");h.Press(MenuInput::Down);
 Check(h.shell.Navigation().Focus()=="hud-layout","Controller did not move to the row following the mouse-selected row");
 // 0 frames of input delay is withdrawn: Left from 1 saves nothing lower.
 h.view.preferences.inputDelay=1;h.Screen("defaults");h.FocusOn("delay");const auto delaySaves=h.actions.size();
 h.Press(MenuInput::Left);h.Frame(0,40);
 Check(h.actions.size()==delaySaves||h.actions.back().preferences.inputDelay==1,"Gameplay defaults offered 0 frames of input delay");
 h.Screen("profile");h.Choose("main-character");h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,40);
 h.view.preferences=h.actions.back().preferences;h.Frame(0,3);Check(status.find("Profile portrait saved:")==0,"Profile success notice missing");
 h.Frame(0,200);Check(status=="Saved","Success notice did not expire");SetMenuStatusProbe({});
 GameMenu recovery;recovery.navigation=RecoveryNavigation(true);sf4e::platform::ServiceSnapshot state;
 state.update.ok=state.update.updateAvailable=true;state.update.expectedSha256=std::string(64,'a');state.update.latestVersion="v9.9.9";
 std::vector<MenuEntry> updateRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){updateRows=r;});
 const auto frame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();const auto choice=DrawRecoveryMenu(recovery,state,"",true);ImGui::Render();return choice;};
 frame();frame();
 // A found update is the first row, named by its version, so Select installs it.
 Check(!updateRows.empty()&&updateRows[0].id=="install"&&updateRows[0].value=="v9.9.9"&&recovery.navigation.Focus()=="install",
  "The updater did not offer the found update first");
 frame(MenuInput::Select);frame();
 Check(recovery.navigation.Confirming()&&!recovery.navigation.ConfirmSelected(),"Recovery install not defaulting to Cancel");
 Check(frame(MenuInput::Select)==RecoveryChoice::None,"Recovery default confirmation installed an update");frame();
 state.pending=true;state.downloadedBytes=100;state.totalBytes=200;frame();
 for(int i=0;i<10&&recovery.navigation.Focus()!="cancel";++i){frame(MenuInput::Down);frame();}
 Check(frame(MenuInput::Select)==RecoveryChoice::Cancel,"Recovery cancellation not reachable");
 // An update found while the window is open takes the highlight once; after
 // that the player's own movement stands, until a newer version is found.
 GameMenu found;found.navigation=RecoveryNavigation(true);sf4e::platform::ServiceSnapshot checking;checking.pending=true;std::string offered;
 const auto show=[&](unsigned held=0){OfferFoundUpdate(found,checking,offered);SetMenuInput({held,0});ImGui::NewFrame();DrawRecoveryMenu(found,checking,"",true);ImGui::Render();};
 show();show();Check(found.navigation.Focus()=="check","The updater did not start on its check");
 checking.pending=false;checking.update.ok=checking.update.updateAvailable=true;checking.update.expectedSha256=std::string(64,'a');checking.update.latestVersion="v9.9.9";
 show();show();Check(found.navigation.Focus()=="install","A newly found update did not take the highlight");
 show(MenuInput::Down);show();Check(found.navigation.Focus()=="check","Moving off the found update did not work");
 show();show();Check(found.navigation.Focus()=="check","The found update took the highlight back after the player moved");
 checking.update.latestVersion="v9.9.10";show();show();Check(found.navigation.Focus()=="install","A newer update did not take the highlight");
 SetMenuEntriesProbe({});
}
void AppearanceGalleries(){
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 selection::Availability available;available.ready=true;available.costumes=3;available.colors[0]=available.colors[1]=5;available.personalActions=1;
 auto frame=[&](unsigned buttons=0,bool editable=true){SetMenuInput({buttons,0});ImGui::NewFrame();
  ImGui::Begin("Gallery test");const bool changed=selector.Draw(pick,false,nullptr,[&](int){return available;},nullptr,editable);ImGui::End();ImGui::Render();return changed;};
 auto press=[&](unsigned buttons,bool editable=true){frame(0,editable);const bool changed=frame(buttons,editable);frame(0,editable);return changed;};
 selector.Navigation().Push("costumes");frame();frame();
 press(MenuInput::Right);Check(pick.costume==0,"Gallery focus committed costume");
 Check(press(MenuInput::Select)&&pick.costume==1,"Costume card did not save");
 Check(selector.Navigation().Screen()=="colors","Saving a costume did not go on to its colors");
 selector.Navigation().Return();selector.Navigation().Push("colors");frame();frame();
 press(MenuInput::Right);Check(pick.color==0,"Gallery focus committed color");
 Check(press(MenuInput::Select)&&pick.color==2,"Color gallery ignored native availability gaps");
 Check(selector.Navigation().Screen()=="home","Saving a color did not return to the selector's home");
 selector.Navigation().Push("colors");frame(0,false);frame(0,false);
 press(MenuInput::Left,false);press(MenuInput::Select,false);Check(pick.color==2,"Locked gallery saved a choice");
 available.colors[1]=0;selector.Navigation().Return();selector.Navigation().Push("costumes");frame(0,false);
 // Missing palette data must not dereference an empty preview list.
 frame(0,false);
 available.colors[1]=5;selector.Navigation().Return();selector.Navigation().Push("ultra");frame();frame();
 std::vector<MenuEntry> ultras;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){ultras=rows;});
 frame();
 // The Ultras are photo cards; Select saves one and returns to the selector's home.
 selector.Navigation().Focus("ultra-1",ultras);frame();Check(pick.ultra==0,"Ultra focus committed a choice");
 Check(press(MenuInput::Select)&&pick.ultra==1&&selector.Navigation().Screen()=="home","Ultra II did not save and return");
 selector.Navigation().Push("ultra");frame();frame();
 Check(pick.ultra==1&&ultras.size()>=2&&ultras[1].value=="SAVED"&&ultras[0].value.empty(),"Saved Ultra has no persistent selection marker separate from focus");
 Dimps::GameEvents::VsMode::ConfirmedCharaConditions native{};
 selection::ToNative(pick,native);pick=selection::FromNative(native);frame();
 Check(pick.ultra==1&&ultras[1].value=="SAVED","Native selection round trip lost Ultra II");
 selector.Navigation().Focus("ultra-0",ultras);press(MenuInput::Select,false);Check(pick.ultra==1,"Locked Ultra selection changed");
 selector.Navigation().Focus("ultra-2",ultras);press(MenuInput::Select);
 Check(pick.ultra==2&&selector.Navigation().Screen()=="home","Ultra Double did not save");
 selector.Navigation().Push("ultra");frame();frame();
 Check(pick.ultra==2&&ultras[2].value=="SAVED","Reopening Ultra lost saved selection");
 // On the home page Left and Right change the Ultra in place, and Select
 // still opens the cards.
 selector.Navigation().Home();frame();selector.Navigation().Focus("ultra",ultras);frame();
 Check(press(MenuInput::Left)&&pick.ultra==1&&selector.Navigation().Screen()=="home","Left on Ultra Combo did not step back to Ultra II");
 Check(!press(MenuInput::Left,false)&&pick.ultra==1,"Locked Ultra Combo row still steps");
 press(MenuInput::Select);Check(selector.Navigation().Screen()=="ultra","Select on Ultra Combo did not open the cards");
 // Picking a fighter goes on to its Ultra, focused on the one it has; Back
 // there keeps the new fighter and returns to the roster.
 selector.Navigation().Home();selector.Navigation().Push("roster");frame();frame();
 selector.Navigation().Focus("fighter-5",ultras);press(MenuInput::Select);
 Check(pick.fighter==5&&selector.Navigation().Screen()=="ultra"&&selector.Navigation().Focus()=="ultra-1","Picking a fighter did not go on to its Ultra");
 press(MenuInput::Back);Check(pick.fighter==5&&pick.ultra==1&&selector.Navigation().Screen()=="roster","Back from the Ultra step lost the fighter");
 // A fighter with a single Ultra in its edition skips the step.
 auto single=[&](unsigned buttons){SetMenuInput({buttons,0});ImGui::NewFrame();ImGui::Begin("Gallery test");
  selector.Draw(pick,true,nullptr,[&](int){return available;},nullptr,true);ImGui::End();ImGui::Render();};
 pick.fighter=0;pick.edition=13;pick.ultra=0;single(0);single(0);
 Check(selection::AllowedUltras(0,13).size()==1,"Ryu's SFIV edition should have one Ultra");
 selector.Navigation().Focus("fighter-0",ultras);single(0);single(MenuInput::Select);single(0);
 Check(pick.edition==13&&selector.Navigation().Screen()=="home","A single-Ultra fighter still asked for its Ultra");
 // The step follows the new fighter's own saved pick, which the caller
 // restores after the pick: from Ryu on SFIV to Zangief saved on Ultra, the
 // step shows; from there to a fighter saved on SFIV, it does not.
 Check(selection::AllowedUltras(5,13).size()==1,"Zangief's SFIV edition should have one Ultra");
 selector.Navigation().Home();selector.Navigation().Push("roster");single(0);single(0);
 selector.Navigation().Focus("fighter-5",ultras);single(0);single(MenuInput::Select);
 pick.edition=14;single(0);single(0);
 Check(pick.fighter==5&&selector.Navigation().Screen()=="ultra","A fighter saved on the Ultra edition skipped its Ultra");
 selector.Navigation().Home();selector.Navigation().Push("roster");single(0);single(0);
 selector.Navigation().Focus("fighter-0",ultras);single(0);single(MenuInput::Select);
 pick.edition=13;single(0);single(0);
 Check(pick.fighter==0&&selector.Navigation().Screen()=="home","A fighter saved on a single-Ultra edition still asked for its Ultra");
 SetMenuEntriesProbe({});
}
// A notice raised while an editor or a confirmation is open must be seen, and
// must give the dialog back with its draft and its Cancel default.
void NoticeOverDialogs() {
 using namespace sf4e;Harness h;h.Frame();
 const auto shown=[&](const char* name){const auto* w=ImGui::FindWindowByName(name);return w&&w->Active&&!w->Hidden&&w->HiddenFramesCannotSkipItems==0;};
 h.Screen("player");h.Choose("name");
 Check(h.shell.Navigation().Editing(),"The name editor did not open");
 h.Frame(0,2);Check(shown("###EditText"),"The editor was not drawn");
 h.shell.Navigation().Draft("Half typed");
 h.view.readyFailure="Your Ready did not go through.";h.view.readyFailureSequence=1;h.Frame(0,3);
 Check(shown("###Notice"),"A notice raised over the editor was never drawn");
 Check(h.shell.Navigation().Editing()&&h.shell.Navigation().Draft()=="Half typed","A notice cost the editor its draft");
 h.Press(MenuInput::Select);h.Frame(0,3);
 Check(!shown("###Notice")&&shown("###EditText")&&h.shell.Navigation().Editing()&&h.shell.Navigation().Draft()=="Half typed",
  "Answering the notice did not give the editor back");
 h.Press(MenuInput::Back);Check(!h.shell.Navigation().Editing(),"Back did not cancel the returned editor");
 // The same over a confirmation, and Use keyboard is one: its answer starts on Cancel.
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 h.Screen("player");h.Frame();SetMenuEntriesProbe({});
 const auto keyboard=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="keyboard";});
 Check(keyboard!=rows.end()&&keyboard->confirm&&keyboard->detail==loc::T("player.use_keyboard_detail"),"Use keyboard is not a confirmation that says what it costs");
 h.Choose("keyboard");
 Check(h.shell.Navigation().Confirming()&&!h.shell.Navigation().ConfirmSelected(),"Use keyboard did not open a question defaulting to Cancel");
 h.Frame(0,2);Check(shown("###ConfirmAction"),"The confirmation was not drawn");
 h.view.readyFailure="Your Ready did not go through again.";h.view.readyFailureSequence=2;h.Frame(0,3);
 Check(shown("###Notice")&&h.shell.Navigation().Confirming(),"A notice raised over the confirmation was never drawn");
 h.Press(MenuInput::Select);h.Frame(0,3);
 Check(!shown("###Notice")&&shown("###ConfirmAction")&&h.shell.Navigation().Confirming()&&!h.shell.Navigation().ConfirmSelected(),
  "Answering the notice did not give the confirmation back on Cancel");
 auto sent=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==sent&&!h.shell.Navigation().Confirming(),"Cancel on Use keyboard still switched the device");
 h.Choose("keyboard");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==sent+1&&h.actions.back().inputAction==input::Action::UseKeyboard,"Confirming Use keyboard did not switch the device");
}
// The failed language save speaks on the Interface screen only, and everything
// more urgent outranks it.
void LanguageSaveFailure() {
 using namespace sf4e;Harness h;
 h.shell.SetLanguageSaver([](const std::string&,std::string& diagnostic){diagnostic="Read-only folder";return false;});
 std::string status;Tone tone=Tone::Neutral;
 SetMenuStatusProbe([&](const char* text,Tone t){status=text;tone=t;});
 h.Screen("interface");h.FocusOn("language");h.Press(MenuInput::Right);h.Frame(0,45);
 const std::string failure=loc::T("settings.language_save_failed");
 Check(status==failure&&tone==Tone::Error,"A failed language save was not reported where the language is set");
 h.Screen("join");h.Frame(0,2);Check(status!=failure,"The language save failure followed the player to another screen");
 h.view.session.error="Could not join the room.";h.Frame();
 Check(status==h.view.session.error,"A session error lost to a stale language save failure");
 h.view.session.error.clear();h.view.controllerUnavailable=true;h.Frame();
 Check(status==loc::T("controller.disconnected"),"A controller disconnect lost to a stale language save failure");
 h.view.controllerUnavailable=false;h.Screen("interface");h.Frame(0,2);
 Check(status==failure,"The failure did not return with the Interface screen");
 SetMenuStatusProbe({});loc::SetActive(loc::Locale::En);
}
// What the session reports is worded in the active language, and a replacement
// offer outranks the condition that led to it.
void SessionReports() {
 using namespace sf4e;Harness h;
 std::string status;Tone tone=Tone::Neutral;
 SetMenuStatusProbe([&](const char* text,Tone t){status=text;tone=t;});
 h.Screen("home");
 h.view.session.fault=netplay::Fault::ControlRecovering;h.Frame();
 Check(status==loc::T("room.control_recovering")&&tone==Tone::Error,"A lost room control was not worded from the catalog");
 h.view.session.fault=netplay::Fault::CatchingUp;h.Frame();
 Check(status==loc::T("room.catching_up"),"A room that is catching up was not worded from the catalog");
 loc::SetActive(loc::Locale::Fr);h.Frame();
 Check(status==loc::T("room.catching_up")&&status.find("catching up")==std::string::npos,"The catching-up report stayed English in French");
 loc::SetActive(loc::Locale::En);
 h.view.session.room=netplay::RoomState::Joined;h.view.room.roomEpoch=3;h.view.room.localMember=1;
 h.view.session.control=netplay::Health::Lost;h.view.session.recovery=netplay::Recovery::Recovering;
 h.view.session.fault=netplay::Fault::ControlRecovering;h.Screen("room");h.Frame();
 Check(status==loc::T("room.control_recovering"),"The room's recovery status is not the catalog's");
 h.view.session.recovery=netplay::Recovery::ReplacementOffered;h.Frame();
 Check(status==loc::T("room.control_unavailable"),"The offer to replace the room is hidden behind the recovery sentence");
 SetMenuStatusProbe({});
}
// The recovery window says all of a long launcher message, and shows the
// newer of the launcher's message and a service's.
void RecoveryWindow() {
 using namespace sf4e;HeadlessImGui imgui;auto& io=imgui.io;
 const std::string paths="C:\\Program Files (x86)\\Steam\\steamapps\\common\\Super Street Fighter IV - Arcade Edition\\GGPO.dll\n"
  "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Super Street Fighter IV - Arcade Edition\\spdlog.dll";
 const auto message=loc::Tf("launcher.runtime_shadowed",paths);
 float text=0,room=0;bool reported=false;
 SetMenuTextProbe([&](const char* id,float t,float interior,float,float){if(!std::strcmp(id,"command-feedback")){reported=true;text=t;room=interior;}});
 platform::ServiceSnapshot state;
 for(const float dpi:{1.f,1.5f,2.f}){
  // The window is 940x720 at 96 dpi and grows with the display, so the same room at every scale.
  ApplyTheme(dpi);io.Fonts->Build();io.DisplaySize=ImVec2(924*dpi,681*dpi);
  GameMenu menu;menu.navigation=RecoveryNavigation(false);reported=false;
  for(int i=0;i<3;++i){ImGui::NewFrame();DrawRecoveryMenu(menu,state,message,false);ImGui::Render();}
  const auto* window=ImGui::FindWindowByName("###EmberRecovery");
  Check(reported&&window&&window->ScrollMax.y<1,"The recovery window overflowed with a long launcher message");
  Check(text>4*ImGui::GetTextLineHeight(),"The long launcher message did not wrap to several lines");
  Check(text<=room+.5f,"The recovery window cut off the end of the launcher message at 100%, 150% or 200%");
 }
 SetMenuTextProbe({});ApplyTheme(1.f);io.Fonts->Build();io.DisplaySize=ImVec2(1280,960);
 // Newest wins: a rejected folder is not buried by an earlier update check.
 std::string status;Tone tone=Tone::Neutral;SetMenuStatusProbe([&](const char* s,Tone t){status=s;tone=t;});
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 GameMenu menu;menu.navigation=RecoveryNavigation(false);
 const auto draw=[&](const std::string& launcher,Tone launcherTone,bool updates,bool canStart,bool serviceNewer){
  for(int i=0;i<2;++i){ImGui::NewFrame();DrawRecoveryMenu(menu,state,launcher,updates,launcherTone,canStart,serviceNewer);ImGui::Render();}};
 state.message="You are up to date.";state.succeeded=true;
 draw("That folder does not contain SSFIV.exe.",Tone::Error,false,false,false);
 Check(status=="That folder does not contain SSFIV.exe."&&tone==Tone::Error,"An old update check hid the rejected folder");
 draw("That folder does not contain SSFIV.exe.",Tone::Error,false,false,true);
 Check(status=="You are up to date."&&tone==Tone::Success,"A newer update check was not shown");
 draw("",Tone::Error,false,false,false);
 Check(status=="You are up to date.","With no launcher message the service result was hidden");
 // The launch message that opened recovery keeps its own tone.
 state.message.clear();
 draw(loc::T("launcher.recovery_opened"),Tone::Neutral,false,false,false);
 Check(tone==Tone::Neutral&&status==loc::T("launcher.recovery_opened"),"The recovery notice was shown as an error");
 // The updater can start the game only when told it may, and says what its Close does.
 const auto row=[&](const char* id)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});return it==rows.end()?nullptr:&*it;};
 draw("",Tone::Error,true,false,true);
 Check(!row("retry")&&row("close")&&row("close")->detail==loc::T("updates.close_detail"),"The updater offered Start SF4 when it may not, or kept the recovery Close text");
 draw("",Tone::Error,true,true,true);
 Check(row("retry")&&row("retry")->label==loc::T("updates.start_game")&&row("retry")->enabled,"The updater did not offer Start SF4 when it may");
 draw("",Tone::Error,false,true,true);
 Check(row("retry")&&row("retry")->label==loc::T("recovery.retry"),"The launch window's Retry changed");
 // Selecting Start SF4 answers Retry.
 menu.navigation=RecoveryNavigation(true);
 RecoveryChoice choice=RecoveryChoice::None;
 const auto frame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();choice=DrawRecoveryMenu(menu,state,"",true,Tone::Error,true,true);ImGui::Render();};
 frame();frame();
 for(int i=0;i<20&&menu.navigation.Focus()!="retry";++i){frame(MenuInput::Down);frame();}
 Check(menu.navigation.Focus()=="retry","Start SF4 is unreachable in the updater");
 frame(MenuInput::Select);
 Check(choice==RecoveryChoice::Retry,"Start SF4 did not answer Retry");
 SetMenuStatusProbe({});SetMenuEntriesProbe({});
}
// Options rows save as they step, so they do not promise a Select; the status
// line promises what each page does.
void SelectorPages() {
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 selection::Availability available;available.ready=true;available.costumes=3;available.colors[0]=available.colors[1]=5;available.personalActions=1;
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto frame=[&](bool editable=true){SetMenuInput({0,0});ImGui::NewFrame();
  ImGui::Begin("Selector pages");selector.Draw(pick,true,nullptr,[&](int){return available;},nullptr,editable);ImGui::End();ImGui::Render();};
 const auto at=[&](const char* screen,bool editable=true){selector.Navigation().Home();if(std::string(screen)!="home")selector.Navigation().Push(screen);frame(editable);frame(editable);};
 const auto detail=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;})->detail;};
 at("options");
 Check(detail("handicap")==loc::T("selection.adjust_saves")&&detail("quote")==loc::T("selection.adjust_saves")&&
  detail("handicap").find("Select saves")==std::string::npos,"An option row promised that Select saves");
 Check(status==loc::T("selection.status_adjust"),"The options page promised Select in its status");
 at("home");Check(status==loc::T("selection.status_browse"),"The selector's home promised that Select saves");
 at("appearance");Check(status==loc::T("selection.status_browse"),"The appearance page promised that Select saves");
 for(const char* page:{"roster","costumes","colors","ultra","stage"}){at(page);Check(status==loc::T("selection.status_editable"),"A page where Select saves lost that promise");}
 at("options",false);Check(detail("handicap")==loc::T("selection.locked_detail")&&status==loc::T("selection.status_locked"),"A locked options page kept its editable wording");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// The selector the shell opens from Home starts on its first page too.
void SelectorFromHome() {
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.Screen("home");h.Choose("selection");
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home","Fighter select did not open on its first page");
 selector.Navigation().Push("appearance");selector.Navigation().Push("colors");h.Frame();
 h.Screen("home");h.Choose("selection");
 Check(selector.Navigation().Screen()=="home","Fighter select reopened from Home on the page it was left on");
}
// The developer screen draws its own fighter selectors. They neither inherit
// the room's hints and Back label nor leave a Close behind that would pop the
// player's next Fighter select on its first frame.
void DeveloperSelectors() {
 using namespace sf4e;Harness h;FighterSelector inspector,selector;selection::Pick pick,inspected;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.developer=[&]{inspector.Draw(inspected,false,nullptr,{},nullptr,true);};
 h.view.session.room=netplay::RoomState::Joined;h.view.room.roomEpoch=1;h.Frame();
 h.Screen("selection");
 Check(EmbeddedReturnContext().shortcutHints.size()==3,"A selector in a room does not show the room's hints");
 h.Screen("developer");
 Check(EmbeddedReturnContext().shortcutHints.empty(),"The developer screen's selector inherited the room's shortcut hints");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="home","Back did not leave the developer screen");
 Check(TakeForwardedMenuAction().kind==MenuAction::None,"The developer screen's selector left a Close behind");
 h.Choose("selection");h.Frame();
 Check(h.shell.Navigation().Screen()=="selection","A Close left by the developer screen popped the next Fighter select");
}
void TrainingJourneys() {
 using namespace sf4e;
 HeadlessImGui imgui;auto& io=imgui.io;
 training::View v;v.available=v.ready=v.checkpoint=true;v.generation=77;v.lengths[0]=20;
 std::vector<training::Command> commands;
 auto frame=[&](unsigned held=0){io.DeltaTime=1.f/60;SetMenuInput({MenuInput::Select,0});
  const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
  for(unsigned i=0;i<6;++i)io.AddKeyEvent(keys[i],(held&(1u<<i))!=0);
  ImGui::NewFrame();
  DrawTrainingFlyout(v,[&](training::Command c){commands.push_back(c);return true;});ImGui::Render();};
 auto press=[&](unsigned held){frame();frame(held);frame();};
 auto choose=[&](const char* id){frame();for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Up);
  for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Down);
  Check(TrainingNavigation().Focus()==id,"Training entry unreachable");press(MenuInput::Select);};
 frame();
 Check(TrainingNavigation().Focus()=="recording","Removed save-position section is still in training navigation");
 press(MenuInput::Down);
 Check(TrainingNavigation().Focus()=="history","Removed frame panel is still in training navigation");
 choose("recording");choose("record");press(MenuInput::Select);Check(commands.empty(),"Overwrite default not Cancel");
 choose("record");
 io.AddMousePosEvent(5,5);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
 Check(TrainingNavigation().Confirming()&&commands.empty()&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Outside click dismissed or submitted training confirmation");
 Check(io.WantCaptureKeyboard&&io.WantCaptureMouse,"Flyout lost input capture outside its bounds");
 press(MenuInput::Right);press(MenuInput::Select);Check(commands.size()==1&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Record returned before command acceptance");
 v.commandId=commands.back().requestId;v.commandAccepted=false;v.commandError="Fight not ready";frame();Check((TakeForwardedMenuAction().kind!=MenuAction::Close),"Failed command closed training");
 choose("play");Check(commands.back().action==training::Action::Play&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Play command dispatch");
 v.commandId=commands.back().requestId;v.commandAccepted=true;frame();Check((TakeForwardedMenuAction().kind==MenuAction::Close),"Accepted playback did not return to practice");
 press(MenuInput::Back);Check(TrainingNavigation().Screen()=="home"&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Back skipped the training root");
 press(MenuInput::Back);Check((TakeForwardedMenuAction().kind==MenuAction::Close),"Root Back did not return to practice");
 // F7 on a recorded slot asks about overwriting it: Record is focused with its
 // question open on Cancel, and Right then Select sends exactly one Record.
 commands.clear();ShowTrainingRecordings();frame();frame();
 Check(TrainingNavigation().Screen()=="recording"&&TrainingNavigation().Focus()=="record"&&TrainingNavigation().Confirming()&&
  !TrainingNavigation().ConfirmSelected(),"F7 on a recorded slot did not ask before overwriting");
 press(MenuInput::Select);Check(commands.empty()&&!TrainingNavigation().Confirming(),"The overwrite question did not default to Cancel");
 ShowTrainingRecordings();frame();frame();press(MenuInput::Right);press(MenuInput::Select);
 Check(commands.size()==1&&commands.back().action==training::Action::Record,"Confirming the overwrite did not send one Record");
 TakeForwardedMenuAction();
}
}
int main(){try{Journeys();KeyboardJourneys();ChatJourneys();NoticeOverDialogs();LanguageSaveFailure();SessionReports();RecoveryWindow();SelectorPages();SelectorFromHome();DeveloperSelectors();TrainingJourneys();PresentationJourneys();AppearanceGalleries();std::cout<<"Shell journeys through the renderer passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
