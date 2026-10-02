// The Ember ID and tournament journeys: identity requests, the matches screen,
// match links, a tournament room, and room links from the browser.
#include "shell_journey_support.hxx"
namespace {
// The Ember ID screens: what each request carries, that one is in flight at a
// time, that passphrases open empty and masked, and that answers and refusals
// reach the status line.
void IdentityJourneys() {
 using namespace sf4e;using netplay::IdentityOp;
 Harness h;auto& id=h.view.identity;id.known=true;id.state="disabled";id.passphraseRequired=true;h.Frame();
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 // The identity requests sent so far, and an answer to the newest one.
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto answer=[&](bool ok=true,const char* failure=""){
  const auto requests=sent();Check(!requests.empty(),"No identity request to answer");
  h.view.identityTicket=requests.back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;
  id.ok=ok;id.failure=failure;h.Frame(0,2);
 };
 const auto type=[&](const char* text){
  ImGui::GetIO().AddInputCharactersUTF8(text);h.Frame();
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 };
 h.Screen("settings");h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Settings does not open the Ember ID screen");
 Check(sent().size()==1&&sent().back()->op==IdentityOp::Status,"Opening the Ember ID screen did not ask for its status");
 answer();
 // Under Wine the key needs a passphrase, typed twice, before it can be created.
 h.Frame();Check(row("id-enable")&&!row("id-enable")->enabled,"Create was offered before a passphrase");
 h.Choose("id-new-passphrase");
 Check(h.shell.Navigation().EditingSecret()&&h.shell.Navigation().Draft().empty(),"The passphrase editor is not secret or did not open empty");
 type("correct horse");
 Check(row("id-new-passphrase")->value==loc::T("identity.secret_set"),"An entered passphrase is not shown as entered");
 h.Choose("id-new-confirm");type("correct hose");h.Frame();
 Check(!row("id-enable")->enabled&&row("id-new-confirm")->detail==loc::T("identity.passphrases_differ"),"Differing passphrases did not hold Create back");
 h.Choose("id-new-confirm");type("correct horse");h.Frame();Check(row("id-enable")->enabled,"Matching passphrases did not offer Create");
 h.Choose("id-enable");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent().back()->op==IdentityOp::Enable&&sent().back()->passphrase=="correct horse","Create did not send the passphrase once");
 // One request in flight: nothing else goes out until it is answered.
 const auto inFlight=sent().size();h.Frame(0,5);Check(sent().size()==inFlight&&status==loc::T("identity.working_key"),"Key work is not shown as in progress");
 id.state="ready";id.passphraseRequired=false;id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
 answer();Check(status==loc::T("identity.done.enabled"),"A created ID was not announced");
 Check(row("id-status")->value==id.fingerprint,"The ready ID does not show its fingerprint");
 // A refusal names its reason, and a helper failure is a sentence.
 h.Choose("identity-backup");answer();
 h.Choose("id-backup-passphrase");type("backup words");h.Choose("id-backup-confirm");type("backup words");
 h.Choose("id-export");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent().back()->op==IdentityOp::Export&&sent().back()->passphrase=="backup words"&&sent().back()->path.empty(),
  "Export did not leave the file to the runtime");
 h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=0;h.view.identityRefusal="identity.refused.match";h.Frame(0,2);
 Check(status==loc::T("identity.refused.match"),"A refused request did not say why");
 h.Choose("id-export");h.Press(MenuInput::Right);h.Press(MenuInput::Select);answer(false,"io");
 Check(status==loc::T("identity.failure.storage"),"A helper failure was not put in words");
 // Leaving the screens wipes what was typed.
 h.Screen("home");h.Screen("identity-backup");h.Frame();
 Check(row("id-backup-passphrase")->value==loc::T("identity.secret_empty"),"A passphrase survived leaving the screens");
 // So does hiding the overlay, even with the editor open.
 h.Choose("id-backup-passphrase");type("hidden words");h.Choose("id-backup-confirm");ImGui::GetIO().AddInputCharactersUTF8("half");h.Frame();
 h.shell.Conceal();Check(!h.shell.Navigation().Editing(),"Hiding the overlay left a passphrase editor open");h.Frame();
 Check(row("id-backup-passphrase")->value==loc::T("identity.secret_empty"),"A passphrase survived hiding the overlay");
 // Linked accounts list the services, then inspect and list the first one.
 id.bridges={{"brg_1","https://tournaments.example","Example Tournaments"}};
 // Earlier statuses are still in flight; answer them and this screen's status and list.
 h.Screen("linked-accounts");
 for(int i=0;i<8&&sent().back()->op!=IdentityOp::BridgeInspect;++i)answer();
 Check(sent().back()->op==IdentityOp::BridgeInspect&&sent().back()->origin=="https://tournaments.example","The trusted service was not inspected");
 id.inspected=id.bridges[0];id.connections={{"blumint","BluMint"},{"mock-local","Mock provider"}};answer();
 Check(sent().back()->op==IdentityOp::LinkList&&sent().back()->bridge=="brg_1","The service's links were not listed");
 id.links={{"lnk_1","blumint","BluMint","PlayerOne"}};answer();
 Check(row("id-unlink:lnk_1")&&row("id-connection"),"Links and sites are not shown");
 h.Choose("id-code");type("ABCDE-12345");h.Choose("id-claim");
 Check(sent().back()->op==IdentityOp::LinkClaim&&sent().back()->bridge=="brg_1"&&sent().back()->connection=="blumint"&&
  sent().back()->code=="ABCDE-12345","A claim did not carry its service, site and code");
 id.claimFingerprint="j25zrhe6-pmdhvlja";answer();
 Check(status==loc::Tf("identity.done.claimed",id.claimFingerprint)&&sent().back()->op==IdentityOp::LinkList,
  "A claim did not show the fingerprint to check, or did not refresh the links");
 // Tournament matches: the list is asked for from the selected service, a
 // playable match sends Play with its service, one the organizer enters cannot
 // be played, and a match that stops says why wherever the player is.
 using netplay::tournament::Command;
 const auto played=[&]{std::vector<const Command*> out;for(const auto& a:h.actions)if(a.tournament.op!=Command::Op::None)out.push_back(&a.tournament);return out;};
 answer();h.Screen("tournament-matches");
 Check(!played().empty()&&played().back()->op==Command::Op::Refresh&&played().back()->bridgeId=="brg_1",
  "The matches screen did not ask the selected service for the list");
 for(int i=0;i<4;++i)answer();
 auto& t=h.view.tournament;t.list.bridge="brg_1";
 netplay::tournament::Assignment playable;playable.matchId="emt_1";playable.state="ready";playable.profile="ember-room-v1";
 playable.slot=1;playable.gamesToWin=3;playable.wins={2,1};playable.roundLabel="Winners R1";playable.opponentFingerprint="abcd1234-efgh5678";
 auto organizer=playable;organizer.matchId="emt_2";organizer.profile="organizer-reported-v1";
 t.list.items={playable,organizer};h.Frame();
 Check(row("tm-play:emt_1")&&row("tm-play:emt_1")->enabled&&row("tm-play:emt_1")->value==loc::Tf("tournament.score",1u,2u,3),
  "A playable match does not show its score from the player's side");
 Check(row("tm-play:emt_2")&&!row("tm-play:emt_2")->enabled,"A match the organizer enters can be played");
 h.Choose("tm-play:emt_1");h.Frame();
 Check(played().back()->op==Command::Op::Play&&played().back()->matchId=="emt_1"&&played().back()->bridgeId=="brg_1",
  "Play did not name its match and service");
 t.phase=netplay::tournament::Phase::InRoom;t.matchId="emt_1";t.waitingForOpponent=true;h.Frame();
 Check(row("tm-current")&&row("tm-current")->value==loc::T("tournament.state.waiting_opponent")&&row("tm-stop"),
  "The match being played is not shown");
 Check(!row("tm-play:emt_1")->enabled,"A second match can start while one is played");
 h.Choose("tm-stop");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(played().back()->op==Command::Op::Stop,"Stop playing did not stop the match");
 h.Screen("home");
 t.phase=netplay::tournament::Phase::Failed;t.reason="incompatible_build";h.Frame(0,2);
 Check(status==loc::Tf("tournament.stopped_notice",loc::T("tournament.failure.build")),"A stopped match was not announced");
 // A match link from the browser opens the matches screen on its match, for
 // the player to press Play; nothing is played by itself.
 t=netplay::tournament::Status{};t.list.bridge="brg_1";t.list.items={playable};
 const auto before=played().size();
 t.handoff.bridge="brg_1";t.handoff.match="emt_1";t.handoff.sequence=1;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="tournament-matches"&&h.shell.Navigation().Focus()=="tm-play:emt_1",
  "A match link did not open its match's row");
 for(std::size_t i=before;i<played().size();++i)Check(played()[i]->op!=Command::Op::Play,"A match link started the match by itself");
 // A pasted link goes to the runtime as it was pasted.
 auto& clipboard=ImGui::GetPlatformIO();
 static std::string pastedLink;pastedLink="ember://tournament/open?bridge=brg_1&handoff=x";
 clipboard.Platform_GetClipboardTextFn=[](ImGuiContext*){return pastedLink.c_str();};
 h.Choose("tm-paste");h.Frame();
 Check(played().back()->op==Command::Op::Redeem&&played().back()->handoff==pastedLink&&played().back()->bridgeId=="brg_1",
  "Paste match link did not send the pasted link");
 clipboard.Platform_GetClipboardTextFn=nullptr;
 // In a room, a link only says where to find the match.
 h.view.session.room=netplay::RoomState::Joined;h.Screen("home");
 t.handoff.sequence=2;h.Frame(0,2);
 Check(status==loc::T("tournament.handoff_waiting")&&h.shell.Navigation().Screen()!="tournament-matches",
  "A match link moved a player who is in a room");
 h.view.session.room=netplay::RoomState::Idle;
 // A link that cannot be opened says why.
 t.handoff.match.clear();t.handoff.error="not_found";t.handoff.sequence=3;h.Screen("home");h.Frame(0,2);
 Check(status==loc::T("tournament.failure.handoff"),"A refused match link did not say why");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
}
// A room bound to a tournament match offers no seat, rules or kick changes at
// its table, and its status says what the next game waits for.
void TournamentRoom(){
 using namespace sf4e;
 Harness h;std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto has=[&](const char* id){return std::any_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Match";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;h.view.room.members.push_back(peer);
 h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;
 h.Screen("room");h.FocusOn("table-0");h.Press(MenuInput::Options);h.Frame();
 Check(has("unqueue")&&has("rounds"),"A casual table lost its seat or rules rows");
 h.view.room.tournament.matchId="emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";h.view.room.tournament.gamesToWin=2;
 h.Frame();
 Check(has("ready")&&!has("unqueue")&&!has("rounds")&&has("rules"),"A tournament table offers seat or rules changes");
 h.view.tournament.phase=netplay::tournament::Phase::InRoom;h.view.tournament.waitingForPermit=true;h.Frame(0,2);
 Check(status==loc::T("tournament.state.waiting_permit"),"A tournament table does not say it waits for the service");
 // On the board: the room says it is a tournament match, offers no
 // invitation, and B on the fighter's own seat does not try to leave it.
 h.view.tournament=netplay::tournament::Status{};h.Screen("room");h.Frame(0,2);
 Check(status==loc::Tf("room.tournament_status",2)&&!has("copy")&&!has("copy-short"),"A tournament room reads like a casual one");
 const auto sent=h.actions.size();h.FocusOn("table-0");h.Press(MenuInput::Back);
 for(std::size_t i=sent;i<h.actions.size();++i)
  Check(h.actions[i].roomAction.kind!=room::ActionKind::Unqueue,"B on a tournament seat tried to leave it");
 h.Screen("room");h.Choose("room-members");h.Choose("member-2");
 Check(has("mute")&&!has("kick"),"A tournament room offers Kick");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
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
int main(){try{IdentityJourneys();TournamentRoom();RoomLinks();std::cout<<"Identity and tournament journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
