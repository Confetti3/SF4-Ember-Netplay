// The Ember ID and tournament journeys: identity requests, the matches screen,
// match links, a tournament room and room links from the browser. Connect
// Discord and the onboarding journeys are in discord_connect_journey_test.cxx.
#include "identity_journey_support.hxx"
namespace {
// The Ember ID screens: what each request carries, that one is in flight at a
// time, that passphrases open empty and masked, and that answers and refusals
// reach the status line.
void IdentityJourneys() {
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;id.state="disabled";id.passphraseRequired=true;h.Frame();
 // An answer to the newest identity request sent so far, which must exist.
 const auto answer=[&](bool ok=true,const char* failure=""){Check(!h.sent().empty(),"No identity request to answer");h.answer(ok,failure);};
 h.Screen("home");h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Home does not open the Ember ID screen");
 Check(h.sent().size()==1&&h.sent().back()->op==IdentityOp::Status,"Opening the Ember ID screen did not ask for its status");
 answer();
 // Under Wine the key needs a passphrase, typed twice, before it can be created.
 h.Frame();Check(h.row("id-enable")&&!h.row("id-enable")->enabled,"Create was offered before a passphrase");
 h.Choose("id-new-passphrase");
 Check(h.shell.Navigation().EditingSecret()&&h.shell.Navigation().Draft().empty(),"The passphrase editor is not secret or did not open empty");
 h.type("correct horse");
 Check(h.row("id-new-passphrase")->value==loc::T("identity.secret_set"),"An entered passphrase is not shown as entered");
 h.Choose("id-new-confirm");h.type("correct hose");h.Frame();
 Check(!h.row("id-enable")->enabled&&h.row("id-new-confirm")->detail==loc::T("identity.passphrases_differ"),"Differing passphrases did not hold Create back");
 h.Choose("id-new-confirm");h.type("correct horse");h.Frame();Check(h.row("id-enable")->enabled,"Matching passphrases did not offer Create");
 h.Choose("id-enable");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.sent().back()->op==IdentityOp::Enable&&h.sent().back()->passphrase=="correct horse","Create did not send the passphrase once");
 // One request in flight: nothing else goes out until it is answered.
 const auto inFlight=h.sent().size();h.Frame(0,5);Check(h.sent().size()==inFlight&&h.status==loc::T("identity.working_key"),"Key work is not shown as in progress");
 id.state="ready";id.passphraseRequired=false;h.identify();
 answer();Check(h.status==loc::T("identity.done.enabled"),"A created ID was not announced");
 Check(h.row("id-status")->value==id.fingerprint,"The ready ID does not show its fingerprint");
 // A refusal names its reason, and a helper failure is a sentence.
 h.Choose("identity-backup");answer();
 h.Choose("id-backup-passphrase");h.type("backup words");h.Choose("id-backup-confirm");h.type("backup words");
 h.Choose("id-export");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.sent().back()->op==IdentityOp::Export&&h.sent().back()->passphrase=="backup words"&&h.sent().back()->path.empty(),
  "Export did not leave the file to the runtime");
 h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=0;h.view.identityRefusal="identity.refused.match";h.Frame(0,2);
 Check(h.status==loc::T("identity.refused.match"),"A refused request did not say why");
 h.Choose("id-export");h.Press(MenuInput::Right);h.Press(MenuInput::Select);answer(false,"io");
 Check(h.status==loc::T("identity.failure.storage"),"A helper failure was not put in words");
 // Leaving the screens wipes what was typed.
 h.Screen("home");h.Screen("identity-backup");h.Frame();
 Check(h.row("id-backup-passphrase")->value==loc::T("identity.secret_empty"),"A passphrase survived leaving the screens");
 // So does hiding the overlay, even with the editor open.
 h.Choose("id-backup-passphrase");h.type("hidden words");h.Choose("id-backup-confirm");ImGui::GetIO().AddInputCharactersUTF8("half");h.Frame();
 h.shell.Conceal();Check(!h.shell.Navigation().Editing(),"Hiding the overlay left a passphrase editor open");h.Frame();
 Check(h.row("id-backup-passphrase")->value==loc::T("identity.secret_empty"),"A passphrase survived hiding the overlay");
 // Linked accounts list the services, then inspect and list the first one.
 id.bridges={{"brg_1","https://tournaments.example","Example Tournaments"}};
 // Earlier statuses are still in flight; answer them and this screen's status and list.
 h.Screen("linked-accounts");
 for(int i=0;i<8&&h.sent().back()->op!=IdentityOp::BridgeInspect;++i)answer();
 Check(h.sent().back()->op==IdentityOp::BridgeInspect&&h.sent().back()->origin=="https://tournaments.example","The trusted service was not inspected");
 id.inspected=id.bridges[0];id.connections={{"blumint","BluMint"},{"mock-local","Mock provider"}};answer();
 Check(h.sent().back()->op==IdentityOp::LinkList&&h.sent().back()->bridge=="brg_1","The service's links were not listed");
 id.links={{"lnk_1","blumint","BluMint","PlayerOne"}};answer();
 Check(h.row("id-unlink:lnk_1")&&h.row("id-connection"),"Links and sites are not shown");
 h.Choose("id-code");h.type("ABCDE-12345");h.Choose("id-claim");
 Check(h.sent().back()->op==IdentityOp::LinkClaim&&h.sent().back()->bridge=="brg_1"&&h.sent().back()->connection=="blumint"&&
  h.sent().back()->code=="ABCDE-12345","A claim did not carry its service, site and code");
 id.claimFingerprint="j25zrhe6-pmdhvlja";answer();
 Check(h.status==loc::Tf("identity.done.claimed",id.claimFingerprint)&&h.sent().back()->op==IdentityOp::LinkList,
  "A claim did not show the fingerprint to check, or did not refresh the links");
 // Tournament matches: the list is asked for from the selected service, a
 // playable match sends Play with its service, one the organizer enters cannot
 // be played, and a match that stops says why wherever the player is.
 using netplay::tournament::Command;
 answer();h.Screen("tournament-matches");
 Check(!h.played().empty()&&h.played().back()->op==Command::Op::Refresh&&h.played().back()->bridgeId=="brg_1",
  "The matches screen did not ask the selected service for the list");
 for(int i=0;i<4;++i)answer();
 auto& t=h.view.tournament;t.list.bridge="brg_1";
 netplay::tournament::Assignment playable;playable.matchId="emt_1";playable.state="ready";playable.profile="ember-room-v1";
 playable.slot=1;playable.gamesToWin=3;playable.wins={2,1};playable.roundLabel="Winners R1";playable.opponentFingerprint="abcd1234-efgh5678";
 auto organizer=playable;organizer.matchId="emt_2";organizer.profile="organizer-reported-v1";
 t.list.items={playable,organizer};h.Frame();
 Check(h.row("tm-play:emt_1")&&h.row("tm-play:emt_1")->enabled&&h.row("tm-play:emt_1")->value==loc::Tf("tournament.score",1u,2u,3),
  "A playable match does not show its score from the player's side");
 Check(h.row("tm-play:emt_2")&&!h.row("tm-play:emt_2")->enabled,"A match the organizer enters can be played");
 h.Choose("tm-play:emt_1");h.Frame();
 Check(h.played().back()->op==Command::Op::Play&&h.played().back()->matchId=="emt_1"&&h.played().back()->bridgeId=="brg_1",
  "Play did not name its match and service");
 t.phase=netplay::tournament::Phase::InRoom;t.matchId="emt_1";t.waitingForOpponent=true;h.Frame();
 Check(h.row("tm-current")&&h.row("tm-current")->value==loc::T("tournament.state.waiting_opponent")&&h.row("tm-stop"),
  "The match being played is not shown");
 Check(!h.row("tm-play:emt_1")->enabled,"A second match can start while one is played");
 h.Choose("tm-stop");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.played().back()->op==Command::Op::Stop,"Stop playing did not stop the match");
 h.Screen("home");
 t.phase=netplay::tournament::Phase::Failed;t.reason="incompatible_build";h.Frame(0,2);
 Check(h.status==loc::Tf("tournament.stopped_notice",loc::T("tournament.failure.build")),"A stopped match was not announced");
 // A match link from the browser opens the matches screen on its match, for
 // the player to press Play; nothing is played by itself.
 t=netplay::tournament::Status{};t.list.bridge="brg_1";t.list.items={playable};
 const auto before=h.played().size();
 t.link.bridge="brg_1";t.link.match="emt_1";t.link.sequence=1;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="tournament-matches"&&h.shell.Navigation().Focus()=="tm-play:emt_1",
  "A match link did not open its match's row");
 for(std::size_t i=before;i<h.played().size();++i)Check(h.played()[i]->op!=Command::Op::Play,"A match link started the match by itself");
 for(int i=0;i<4;++i)answer();
 // A match the refreshed list does not hold is not the player's.
 t.link.match="emt_9";t.link.sequence=2;h.Frame(0,2);
 Check(h.played().back()->op==Command::Op::Refresh&&h.played().back()->bridgeId=="brg_1","A match link did not refresh its service's list");
 ++t.list.finished;h.Frame(0,2);
 Check(h.status==loc::T("tournament.failure.not_yours"),"A match missing from the player's list was not said");
 for(int i=0;i<4;++i)answer();
 // Pasted text that is not a match link says so; a pasted page link opens
 // like one from the browser, and a service the player does not trust says so.
 auto& clipboard=ImGui::GetPlatformIO();
 static std::string pastedLink;pastedLink="hello";
 clipboard.Platform_GetClipboardTextFn=[](ImGuiContext*){return pastedLink.c_str();};
 h.Choose("tm-paste");h.Frame();
 Check(h.status==loc::T("tournament.failure.link"),"Pasting something else did not say it is no match link");
 pastedLink="https://embernetplay.link/m#brg_0dbc0598-2312-4ce3-9df8-e160330565e6/emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
 h.Choose("tm-paste");h.Frame();
 for(int i=0;i<4&&h.sent().back()->op!=IdentityOp::BridgeList;++i)answer();
 Check(h.sent().back()->op==IdentityOp::BridgeList,"A pasted match link did not check its service");
 answer();
 Check(h.status==loc::T("tournament.failure.link_service"),"A link to an untrusted service did not say so");
 clipboard.Platform_GetClipboardTextFn=nullptr;
 // In a room, a link only says where to find the match.
 h.view.session.room=netplay::RoomState::Joined;h.Screen("home");
 t.link.sequence=3;h.Frame(0,2);
 Check(h.status==loc::T("tournament.link_waiting")&&h.shell.Navigation().Screen()!="tournament-matches",
  "A match link moved a player who is in a room");
 h.view.session.room=netplay::RoomState::Idle;
}
}
// A room bound to a tournament match offers no seat, rules or kick changes at
// its table, and its status says what the next game waits for.
void TournamentRoom(){
 using namespace sf4e;
 Journey h;
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Match";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;h.view.room.members.push_back(peer);
 h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;
 h.Screen("room");h.FocusOn("table-0");h.Press(MenuInput::Options);h.Frame();
 Check(h.has("unqueue")&&h.has("rounds"),"A casual table lost its seat or rules rows");
 h.view.room.tournament.matchId="emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";h.view.room.tournament.gamesToWin=2;
 h.Frame();
 Check(h.has("ready")&&!h.has("unqueue")&&!h.has("rounds")&&h.has("rules"),"A tournament table offers seat or rules changes");
 h.view.tournament.phase=netplay::tournament::Phase::InRoom;h.view.tournament.waitingForPermit=true;h.Frame(0,2);
 Check(h.status==loc::T("tournament.state.waiting_permit"),"A tournament table does not say it waits for the service");
 // On the board: the room says it is a tournament match, offers no
 // invitation, and B on the fighter's own seat does not try to leave it.
 h.view.tournament=netplay::tournament::Status{};h.Screen("room");h.Frame(0,2);
 Check(h.status==loc::Tf("room.tournament_status",2)&&!h.has("copy")&&!h.has("copy-short"),"A tournament room reads like a casual one");
 const auto sent=h.actions.size();h.FocusOn("table-0");h.Press(MenuInput::Back);
 for(std::size_t i=sent;i<h.actions.size();++i)
  Check(h.actions[i].roomAction.kind!=room::ActionKind::Unqueue,"B on a tournament seat tried to leave it");
 h.Screen("room");h.Choose("room-members");h.Choose("member-2");
 Check(h.has("mute")&&!h.has("kick"),"A tournament room offers Kick");
}
// Copy short link copies the link once the helper has one, and the full
// invitation when it reports a failure. A room link from the browser joins
// by itself when the player is free; one that arrived during a room fills
// the Join screen once no room is open, and sends nothing by itself.
std::string g_clipboard;
void RoomLinks(){
 using namespace sf4e;
 Harness h;
 auto& platform=ImGui::GetPlatformIO();
 platform.Platform_SetClipboardTextFn=[](ImGuiContext*,const char* text){g_clipboard=text?text:"";};
 platform.Platform_GetClipboardTextFn=[](ImGuiContext*){return g_clipboard.c_str();};
 const std::string full="sf4e3:full-invitation",link="https://embernetplay.link/j#7K3M-0X1R-T9PZ";
 h.view.session.room=netplay::RoomState::Joined;h.view.invitation=full;
 h.Screen("room");
 auto before=h.actions.size();g_clipboard.clear();h.Choose("copy-short");
 Check(h.actions.size()==before+1&&h.actions.back().shortInvitation,"Copy short link did not ask for the link");
 Check(g_clipboard.empty(),"Copy short link copied before the link existed");
 h.view.shortInvitation=link;h.Frame();
 Check(g_clipboard==link,"The short link was not copied when it arrived");
 // Once known, it is copied at once and nothing is asked.
 before=h.actions.size();g_clipboard.clear();h.Choose("copy-short");
 Check(h.actions.size()==before&&g_clipboard==link,"A known short link was not copied at once");
 // A failure copies the full invitation instead.
 h.view.shortInvitation.clear();g_clipboard.clear();h.Choose("copy-short");
 h.Frame();Check(g_clipboard.empty(),"Copy short link gave up before any answer");
 ++h.view.shortInvitationFailures;h.Frame();
 Check(g_clipboard==full,"A failed short link did not copy the full invitation");
 // A room link waits while the room is open, then fills the Join screen.
 h.view.pendingJoinLink=link;h.view.pendingJoinSequence=1;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="room","A room link moved the player out of the room");
 before=h.actions.size();
 h.view.session.room=netplay::RoomState::Idle;h.view.invitation.clear();h.view.room=room::Snapshot{};h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="join","A room link did not open the Join screen");
 Check(h.actions.size()==before,"A room link sent a command by itself");
 {
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});h.Frame();SetMenuEntriesProbe({});
  const auto text=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="invite-text";});
  const auto join=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="join-now";});
  Check(text!=rows.end()&&text->value==link,"The Join screen does not hold the room link");
  Check(join!=rows.end()&&join->enabled,"Join room is not available for the room link");
 }
 h.Choose("join-now");
 Check(h.actions.size()==before+1&&h.actions.back().command.kind==Kind::JoinInvite&&h.actions.back().command.invitation==link,
  "Join room did not join with the room link");
 // The same link is not offered twice.
 h.shell.Navigation().Home();h.Frame(0,3);Check(h.shell.Navigation().Screen()=="home","A used room link came back");
 // A link that arrives while the player is free joins by itself, once a
 // room can be opened.
 const std::string second="https://embernetplay.link/j#0X1R-7K3M-T9PZ";
 before=h.actions.size();h.view.canOpenRoom=false;
 h.view.pendingJoinLink=second;h.view.pendingJoinSequence=2;h.view.pendingJoinDirect=true;h.Frame(0,3);
 Check(h.actions.size()==before&&h.shell.Navigation().Screen()=="home","A room link joined before a room could be opened");
 h.view.canOpenRoom=true;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="join","A room link did not open the Join screen while joining");
 Check(h.actions.size()==before+1&&h.actions.back().command.kind==Kind::JoinInvite&&h.actions.back().command.invitation==second,
  "A room link did not join its room");
 h.Frame(0,3);Check(h.actions.size()==before+1,"A room link joined twice");
 // It never interrupts an Ember ID screen, and once the runtime stops
 // offering the direct join it only fills the Join screen.
 const std::string third="https://embernetplay.link/j#T9PZ-0X1R-7K3M";
 h.Screen("identity");before=h.actions.size();
 h.view.pendingJoinLink=third;h.view.pendingJoinSequence=3;h.Frame(0,3);
 Check(h.actions.size()==before&&h.shell.Navigation().Screen()=="identity","A room link interrupted the Ember ID screen");
 h.Screen("linked-accounts");h.Frame(0,3);
 Check(h.actions.size()==before&&h.shell.Navigation().Screen()=="linked-accounts","A room link interrupted the linked accounts");
 h.view.pendingJoinDirect=false;h.shell.Navigation().Home();h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="join"&&h.actions.size()==before,"A stale room link did not just fill the Join screen");
}
int main(){try{IdentityJourneys();TournamentRoom();RoomLinks();RunDiscordConnectJourneys();std::cout<<"Identity and tournament journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
