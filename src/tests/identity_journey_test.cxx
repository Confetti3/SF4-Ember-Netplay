// The Ember ID and tournament journeys: identity requests, the matches screen,
// match links, a tournament room, room links from the browser, and Connect
// Discord from a tournament site's link.
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
 h.Screen("home");h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Home does not open the Ember ID screen");
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
 t.link.bridge="brg_1";t.link.match="emt_1";t.link.sequence=1;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="tournament-matches"&&h.shell.Navigation().Focus()=="tm-play:emt_1",
  "A match link did not open its match's row");
 for(std::size_t i=before;i<played().size();++i)Check(played()[i]->op!=Command::Op::Play,"A match link started the match by itself");
 for(int i=0;i<4;++i)answer();
 // A match the refreshed list does not hold is not the player's.
 t.link.match="emt_9";t.link.sequence=2;h.Frame(0,2);
 Check(played().back()->op==Command::Op::Refresh&&played().back()->bridgeId=="brg_1","A match link did not refresh its service's list");
 ++t.list.finished;h.Frame(0,2);
 Check(status==loc::T("tournament.failure.not_yours"),"A match missing from the player's list was not said");
 for(int i=0;i<4;++i)answer();
 // Pasted text that is not a match link says so; a pasted page link opens
 // like one from the browser, and a service the player does not trust says so.
 auto& clipboard=ImGui::GetPlatformIO();
 static std::string pastedLink;pastedLink="hello";
 clipboard.Platform_GetClipboardTextFn=[](ImGuiContext*){return pastedLink.c_str();};
 h.Choose("tm-paste");h.Frame();
 Check(status==loc::T("tournament.failure.link"),"Pasting something else did not say it is no match link");
 pastedLink="https://embernetplay.link/m#brg_0dbc0598-2312-4ce3-9df8-e160330565e6/emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
 h.Choose("tm-paste");h.Frame();
 for(int i=0;i<4&&sent().back()->op!=IdentityOp::BridgeList;++i)answer();
 Check(sent().back()->op==IdentityOp::BridgeList,"A pasted match link did not check its service");
 answer();
 Check(status==loc::T("tournament.failure.link_service"),"A link to an untrusted service did not say so");
 clipboard.Platform_GetClipboardTextFn=nullptr;
 // In a room, a link only says where to find the match.
 h.view.session.room=netplay::RoomState::Joined;h.Screen("home");
 t.link.sequence=3;h.Frame(0,2);
 Check(status==loc::T("tournament.link_waiting")&&h.shell.Navigation().Screen()!="tournament-matches",
  "A match link moved a player who is in a room");
 h.view.session.room=netplay::RoomState::Idle;
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
// Discord on a service that offers it: optional, connected from the Linked
// accounts screen through the browser, read back with Refresh, disconnected
// with a confirmation. A brand-new player's match link asks for an Ember ID
// first, not for a service.
void DiscordAndFirstLink(){
 using namespace sf4e;using netplay::IdentityOp;
 Harness h;auto& id=h.view.identity;id.known=true;id.state="disabled";h.Frame();
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto answer=[&]{
  h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
 };
 // No Ember ID yet: the link opens the matches screen, which says to create one.
 h.view.tournament.link.bridge="brg_1";h.view.tournament.link.match="emt_1";h.view.tournament.link.sequence=1;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="tournament-matches"&&row("id-linked-unavailable"),"A new player's match link did not ask for an Ember ID");
 for(int i=0;i<6;++i)answer();
 Check(status!=loc::T("tournament.failure.link_service"),"A new player's match link asked to trust a service");
 // With an ID, the address field starts on Ember's own service.
 id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
 h.Screen("linked-accounts");for(int i=0;i<4;++i)answer();
 Check(row("id-origin")&&row("id-origin")->value=="https://bridge.embernetplay.link","The service address does not start on Ember's own service");
 // A service with Discord: its status is asked for, and Connect opens the browser through the helper.
 id.bridges={{"brg_1","https://bridge.embernetplay.link","Ember"}};h.Screen("home");h.Screen("linked-accounts");
 for(int i=0;i<8&&sent().back()->op!=IdentityOp::BridgeInspect;++i)answer();
 id.inspected=id.bridges[0];id.connections={{"blumint","BluMint"}};id.inspectedDiscord=true;answer();
 for(int i=0;i<4&&sent().back()->op!=IdentityOp::DiscordStatus;++i)answer();
 Check(sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_1","A Discord service's account was not asked for");
 id.discordUser.clear();id.discordName.clear();answer();
 Check(row("id-discord-connect")&&row("id-discord-connect")->enabled&&row("id-discord-connect")->value==loc::T("identity.discord_none"),
  "An unconnected Discord account is not offered");
 h.Choose("id-discord-connect");
 Check(sent().back()->op==IdentityOp::DiscordConnect&&sent().back()->bridge=="brg_1","Connect Discord did not name its service");
 answer();Check(status==loc::T("identity.done.discord_opened"),"Connect Discord did not say to finish in the browser");
 // Refresh reads the account back; a connected one is disconnected after a confirmation.
 h.Choose("id-refresh");
 for(int i=0;i<10&&sent().back()->op!=IdentityOp::DiscordStatus;++i)answer();
 id.discordUser="274220342558756145";id.discordName="kate";answer();
 Check(row("id-discord-remove")&&row("id-discord-remove")->value=="kate","A connected Discord account is not shown");
 h.Choose("id-discord-remove");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent().back()->op==IdentityOp::DiscordRemove,"Disconnect did not send");
 id.discordUser.clear();id.discordName.clear();answer();
 Check(status==loc::T("identity.done.discord_removed")&&row("id-discord-connect"),"A disconnected account is still shown");
 // A service without Discord shows no row.
 id.inspectedDiscord=false;h.Choose("id-refresh");for(int i=0;i<10;++i)answer();
 Check(!row("id-discord-connect")&&!row("id-discord-remove"),"A service without Discord offers it");
 // Sign-in off now, but an account connected earlier: it can still be disconnected, and none can be connected.
 id.inspectedDiscordAccounts=true;h.Choose("id-refresh");
 for(int i=0;i<10&&sent().back()->op!=IdentityOp::DiscordStatus;++i)answer();
 Check(sent().back()->op==IdentityOp::DiscordStatus,"A service keeping Discord accounts was not asked for one");
 id.discordUser="274220342558756145";id.discordName="kate";answer();
 Check(row("id-discord-remove")&&!row("id-discord-connect"),"An account kept without sign-in cannot be disconnected");
 id.discordUser.clear();id.discordName.clear();h.Choose("id-refresh");for(int i=0;i<10;++i)answer();
 Check(!row("id-discord-connect")&&!row("id-discord-remove"),"A service without sign-in offers to connect Discord");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// A tournament site's connect link onboards a brand-new player by itself:
// the link they just clicked creates the Ember ID, trusts Ember's own
// service and opens Discord's page, so Discord's Authorize is the only
// press; the screen then reads the account until it is connected. A link
// that waited for a room asks first. A connected account is only shown, and
// a link for a service Ember does not know trusts nothing.
void DiscordConnectLink(){
 using namespace sf4e;using netplay::IdentityOp;
 Harness h;auto& id=h.view.identity;id.known=true;id.state="disabled";h.Frame();
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto answer=[&]{
  h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
 };
 const auto until=[&](IdentityOp op){for(int i=0;i<12&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
 const auto count=[&](IdentityOp op,std::size_t from){
  std::size_t n=0;const auto all=sent();for(std::size_t i=from;i<all.size();++i)n+=all[i]->op==op;return n;};
 const std::string ember="https://bridge.embernetplay.link";
 // In a room the link only says it will open later, and then asks first.
 h.view.session.room=netplay::RoomState::Joined;h.Screen("home");
 h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
 Check(status==loc::T("connect.link_waiting")&&h.shell.Navigation().Screen()!="discord-connect","A connect link moved a player who is in a room");
 const auto waited=sent().size();
 h.view.session.room=netplay::RoomState::Idle;h.Frame(0,2);
 Check(h.shell.Navigation().Screen()=="discord-connect","A connect link did not open Connect Discord once the room closed");
 for(int i=0;i<4;++i)answer();
 Check(row("dc-go")&&row("dc-go")->enabled&&count(IdentityOp::Enable,waited)==0,"A link that waited for a room did not ask first");
 // A link the player just clicked runs by itself up to Discord's page.
 h.Screen("home");const auto start=sent().size();
 h.view.tournament.connect.sequence=2;h.Frame(0,2);
 Check(h.shell.Navigation().Screen()=="discord-connect"&&h.shell.Navigation().Parent()=="identity","A connect link did not open Connect Discord under Ember ID");
 Check(until(IdentityOp::Enable),"The link did not create the Ember ID by itself");
 Check(row("dc-progress")!=nullptr,"Connect Discord does not show it is setting up");
 id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";answer();
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"The link did not look up Ember's own service");
 id.inspected={"brg_1",ember,"Ember"};id.inspectedDiscord=true;answer();
 Check(sent().back()->op==IdentityOp::BridgeApprove&&sent().back()->bridge=="brg_1"&&sent().back()->origin==ember,"The link did not trust Ember's own service by itself");
 id.bridges={{"brg_1",ember,"Ember"}};answer();
 Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_1","The link did not read the account first");
 id.discordUser.clear();id.discordName.clear();answer();
 Check(sent().back()->op==IdentityOp::DiscordConnect&&sent().back()->bridge=="brg_1","The link did not open Discord by itself");
 Check(count(IdentityOp::LinkList,start)==0,"Connect Discord asked for the linked accounts");
 answer();Check(status==loc::T("identity.done.discord_opened"),"Opening Discord did not say to finish in the browser");
 Check(row("dc-waiting")&&row("dc-cancel"),"Waiting for Discord is not shown with Cancel");
 // While the browser is open the account is read again by itself.
 const auto polled=sent().size();h.Frame(0,300);
 Check(sent().size()>polled&&sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_1","The account was not read again");
 answer();h.Frame(0,1900);
 Check(status==loc::T("connect.waiting"),"Waiting for Discord is not said once the first notice ends");
 id.discordUser="274220342558756145";id.discordName="kate";until(IdentityOp::DiscordStatus);answer();
 Check(status==loc::Tf("connect.done","kate")&&row("dc-connected")&&row("dc-connected")->value=="kate","A connected account was not shown");
 Check(row("id-discord-connect")&&row("id-discord-remove"),"A connected account offers no other account or Unlink");
 const auto settled=sent().size();h.Frame(0,300);
 Check(sent().size()==settled,"The account was still read again after it connected");
 // A second link for a connected account only shows it.
 h.Screen("home");h.view.tournament.connect.sequence=3;h.Frame(0,2);
 Check(h.shell.Navigation().Screen()=="discord-connect","A second connect link did not open Connect Discord");
 for(int i=0;i<8;++i)answer();
 Check(row("dc-connected")&&count(IdentityOp::DiscordConnect,start)==1,"A link for a connected account opened Discord again");
 // A link for a service Ember does not know says so; nothing is trusted by itself.
 const auto before=sent().size();
 h.view.tournament.connect.bridge="brg_9";h.view.tournament.connect.sequence=4;h.Frame(0,2);
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"A connect link for another service did not check Ember's own");
 id.inspected={"brg_1",ember,"Ember"};answer();for(int i=0;i<6;++i)answer();
 Check(row("dc-unknown")&&row("dc-retry")&&!row("id-approve")&&!row("id-discord-connect"),"A link for an unknown service was not said");
 Check(count(IdentityOp::BridgeApprove,before)==0,"A connect link trusted another service by itself");
 // The Ember ID screen opens Connect Discord too.
 h.Screen("identity");for(int i=0;i<4;++i)answer();
 Check(row("discord-connect")!=nullptr,"The Ember ID screen does not offer Connect Discord");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// The wait for a Discord sign-in belongs to the service it was opened for:
// another service that already has an account does not end it, its polls do
// not change what another selected service shows, and a new connect link
// stops waiting for the old one. An old Connect's late answer, success or
// failure, changes nothing in the new visit.
void DiscordWaitsForItsService(){
 using namespace sf4e;using netplay::IdentityOp;
 Harness h;auto& id=h.view.identity;id.known=true;id.state="ready";h.view.tournament.connect.confirm=true;
 id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto answer=[&]{
  h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
 };
 const auto until=[&](IdentityOp op){for(int i=0;i<10&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
 const auto open=[&](const char* bridge,std::uint64_t sequence,const netplay::IdentityBridge& profile){
  h.view.tournament.connect.bridge=bridge;h.view.tournament.connect.sequence=sequence;h.Frame(0,2);
  Check(until(IdentityOp::BridgeInspect),"Connect Discord did not inspect its service");
  id.inspected=profile;id.inspectedDiscord=true;answer();
  Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge==bridge,"Connect Discord did not read its service's account");
 };
 // A sign-in opened on Ember's service.
 open("brg_1",1,id.bridges[0]);id.discordUser.clear();id.discordName.clear();answer();
 h.Choose("id-discord-connect");Check(sent().back()->op==IdentityOp::DiscordConnect&&sent().back()->bridge=="brg_1","Connect did not name its service");
 // Its answer arrives only after a second link, for a service that already
 // has an account, started a new visit.
 const auto connect=sent().back()->ticket;
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=2;h.Frame(0,2);
 h.view.identityTicket=connect;h.view.identityRequest=id.requestId=connect+100;id.ok=true;h.Frame(0,2);
 Check(status!=loc::T("identity.done.discord_opened"),"A replaced visit's Connect answer changed the status");
 Check(until(IdentityOp::BridgeInspect),"The second link did not inspect its service");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
 Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","The second link did not read its service's account");
 id.discordUser="111";id.discordName="sam";answer();
 Check(status!=loc::Tf("connect.done","sam"),"Another service's account ended the sign-in's wait");
 // Nothing is read again for the first service's sign-in.
 const auto before=sent().size();h.Frame(0,300);
 Check(sent().size()==before,"A sign-in from an earlier visit was still waited for");
 // In one visit, polls go to the sign-in's service and only its account ends the wait.
 open("brg_1",3,id.bridges[0]);id.discordUser.clear();id.discordName.clear();answer();
 h.Choose("id-discord-connect");answer();
 h.Frame(0,300);
 Check(sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_1","The poll did not read the sign-in's service");
 id.discordUser="222";id.discordName="kate";answer();
 Check(status==loc::Tf("connect.done","kate"),"The sign-in's own account did not end the wait");
 // A failed late answer does not reach the new visit either.
 id.discordUser.clear();id.discordName.clear();
 open("brg_1",4,id.bridges[0]);answer();
 h.Choose("id-discord-connect");const auto failing=sent().back()->ticket;
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=5;h.Frame(0,2);
 h.view.identityTicket=failing;h.view.identityRequest=id.requestId=failing+100;id.ok=false;id.failure="rate_limited";h.Frame(0,2);
 Check(status!=loc::T("identity.failure.rate_limited"),"A replaced visit's failed Connect changed the status");
 Check(until(IdentityOp::BridgeInspect),"A replaced visit's failed Connect dropped the new visit's requests");
 // Under Linked accounts, the selected service keeps showing its own
 // account while the sign-in on Ember's service is polled.
 open("brg_1",6,id.bridges[0]);id.discordUser.clear();id.discordName.clear();answer();
 h.Choose("id-discord-connect");answer();
 h.Screen("linked-accounts");for(int i=0;i<8;++i)answer();
 h.FocusOn("id-bridge");h.Press(MenuInput::Select);h.Press(MenuInput::Down);h.Press(MenuInput::Select);
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"Selecting the other service did not inspect it");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
 Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","The other service's account was not read");
 id.discordUser="111";id.discordName="sam";answer();for(int i=0;i<4;++i)answer();
 Check(row("id-discord-remove")&&row("id-discord-remove")->value=="sam","The other service's account is not shown");
 id.discordUser.clear();id.discordName.clear();
 h.Frame(0,300);
 Check(sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_1","The sign-in's service was not polled");
 answer();
 Check(row("id-discord-remove")&&row("id-discord-remove")->value=="sam","A poll of another service replaced the shown account");
 // A poll from a replaced journey that fails, or never answers, does not
 // reach the new journey either.
 h.Frame(0,300);
 Check(sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_1","No poll was in flight");
 const auto poll=sent().back()->ticket;
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=7;h.Frame(0,2);
 h.view.identityTicket=poll;h.view.identityRequest=id.requestId=poll+100;id.ok=false;id.failure="rate_limited";h.Frame(0,2);
 Check(status!=loc::T("identity.failure.rate_limited"),"A replaced journey's failed poll changed the status");
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"A replaced journey's failed poll dropped the new journey's requests");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
 Check(until(IdentityOp::DiscordStatus),"The new journey did not read its account");
 id.discordUser.clear();id.discordName.clear();answer();
 h.Choose("id-discord-connect");answer();h.Frame(0,300);
 Check(sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_2","No poll was in flight on the new journey");
 h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=8;h.Frame(0,7300);
 Check(status!=loc::T("identity.failure.timeout"),"A replaced journey's unanswered poll timed out on the new journey");
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"A replaced journey's unanswered poll dropped the new journey's requests");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// A link's journey keeps its service through a locked Ember ID, unlocked on
// Connect Discord itself, and through a visit to the Ember ID screen.
void DiscordConnectKeepsItsService(){
 using namespace sf4e;using netplay::IdentityOp;
 Harness h;auto& id=h.view.identity;id.known=true;id.state="locked";h.Frame();h.view.tournament.connect.confirm=true;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto answer=[&]{
  h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
 };
 const auto until=[&](IdentityOp op){for(int i=0;i<10&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
 const auto type=[&](const char* text){
  ImGui::GetIO().AddInputCharactersUTF8(text);h.Frame();
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 };
 // A link for a trusted service other than Ember's own, with the ID locked.
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
 for(int i=0;i<4;++i)answer();
 Check(h.shell.Navigation().Screen()=="discord-connect"&&row("id-unlock"),"A locked ID is not unlocked on Connect Discord");
 h.Choose("id-unlock");type("correct horse");
 Check(sent().back()->op==IdentityOp::Unlock&&sent().back()->passphrase=="correct horse","Unlock did not send the passphrase");
 id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
 id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};answer();
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"Unlocking did not go on to the link's service");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
 Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","The link's service's account was not read");
 id.discordUser.clear();id.discordName.clear();answer();
 // A visit to the Ember ID screen comes back to the same journey.
 id.state="recovery_required";h.Frame(0,2);
 Check(row("identity")!=nullptr,"An ID needing recovery does not lead to the Ember ID screen");
 h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Connect Discord did not open the Ember ID screen");
 id.state="ready";for(int i=0;i<4;++i)answer();
 h.Choose("discord-connect");
 Check(h.shell.Navigation().Screen()=="discord-connect"&&h.shell.Navigation().Parent()=="identity","Connect Discord from the Ember ID screen did not go back to the journey");
 for(int i=0;i<8;++i)answer();
 Check(row("dc-service")&&row("dc-service")->value=="Other","The journey lost the link's service");
 h.Choose("id-discord-connect");
 Check(sent().back()->op==IdentityOp::DiscordConnect&&sent().back()->bridge=="brg_2","Connect did not name the link's service");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// Where the browser cannot open Ember, the page's link pasted into Connect
// Discord, opened from the menu, starts the journey for the site's service,
// not Ember's own. Something else pasted says so and changes nothing.
std::string g_pasted;
void DiscordConnectPastedLink(){
 using namespace sf4e;using netplay::IdentityOp;
 Harness h;auto& id=h.view.identity;id.known=true;id.state="ready";
 id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 const std::string site="brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
 id.bridges={{"brg_1",ember,"Ember"},{site,other,"Other"}};h.Frame();
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto answer=[&]{
  h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
 };
 const auto until=[&](IdentityOp op){for(int i=0;i<10&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
 auto& clipboard=ImGui::GetPlatformIO();
 clipboard.Platform_GetClipboardTextFn=[](ImGuiContext*){return g_pasted.c_str();};
 // From the menu, Connect Discord is for Ember's own service.
 h.Screen("identity");for(int i=0;i<4;++i)answer();
 h.Choose("discord-connect");
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"Connect Discord from the menu is not for Ember's own service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;answer();for(int i=0;i<4;++i)answer();
 Check(row("dc-paste")!=nullptr,"Connect Discord does not offer to paste the page's link");
 // Something else pasted says so.
 g_pasted="hello";const auto before=sent().size();h.Choose("dc-paste");h.Frame();
 Check(status==loc::T("connect.paste_failed")&&sent().size()==before,"Pasting something else did not say so");
 // The page's link starts the journey for the site's service.
 g_pasted="https://embernetplay.link/start#"+site;h.Choose("dc-paste");
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"A pasted page link did not start the journey for its service");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
 Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge==site,"The site's service's account was not read");
 id.discordUser.clear();id.discordName.clear();answer();
 // Pasting is the player's press: Discord opens by itself, for the site's service.
 Check(row("dc-service")&&row("dc-service")->value=="Other","Connect Discord does not show the site's service");
 Check(sent().back()->op==IdentityOp::DiscordConnect&&sent().back()->bridge==site,"A pasted link did not open Discord for the site's service");
 clipboard.Platform_GetClipboardTextFn=nullptr;
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// A sign-in started under Linked accounts before any Connect Discord visit
// is retired like any other once a connect link arrives: its late Connect
// answer, a failed or an unanswered poll neither stop the new journey nor
// wait again.
void FirstSignInRetires(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 std::vector<MenuEntry> rows;std::string status;
 for(int ending=0;ending<3;++ending){
  Harness h;auto& id=h.view.identity;id.known=true;id.state="ready";h.view.tournament.connect.confirm=true;
  id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
  id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
  const auto answer=[&]{
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
  };
  const auto until=[&](IdentityOp op){for(int i=0;i<10&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
  const auto late=[&](std::uint64_t ticket,bool ok){
   h.view.identityTicket=ticket;h.view.identityRequest=id.requestId=ticket+100;id.ok=ok;id.failure=ok?"":"rate_limited";h.Frame(0,2);
  };
  h.Screen("linked-accounts");
  Check(until(IdentityOp::BridgeInspect),"Linked accounts did not inspect the service");
  id.inspected=id.bridges[0];id.inspectedDiscord=true;answer();
  Check(until(IdentityOp::DiscordStatus),"Linked accounts did not read the account");
  id.discordUser.clear();id.discordName.clear();answer();for(int i=0;i<4;++i)answer();
  h.Choose("id-discord-connect");
  Check(sent().back()->op==IdentityOp::DiscordConnect&&sent().back()->bridge=="brg_1","Connect did not name its service");
  if(ending>0){answer();h.Frame(0,300);Check(sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_1","No poll was in flight");}
  const auto ticket=sent().back()->ticket;
  h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
  if(ending==0)late(ticket,true);
  else if(ending==1)late(ticket,false);
  else h.Frame(0,7300);
  // Only a Connect answered after the link could say to finish in the browser.
  Check((ending>0||status!=loc::T("identity.done.discord_opened"))&&status!=loc::T("identity.failure.rate_limited")&&
   status!=loc::T("identity.failure.timeout"),"The first sign-in's late answer changed the new journey's status");
  Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"The first sign-in's late answer stopped the new journey");
  id.inspected=id.bridges[1];answer();for(int i=0;i<4;++i)answer();
  const auto before=sent().size();h.Frame(0,300);
  for(std::size_t i=before;i<sent().size();++i)Check(sent()[i]->bridge!="brg_1","The first sign-in was waited for again");
  SetMenuEntriesProbe({});SetMenuStatusProbe({});
 }
}
// Reads Linked accounts still had out when a connect link arrived are
// retired: their failure neither says so nor holds the new journey back.
// A read of the journey's own service that fails offers Try again, which
// reads it again; so does a poll that fails during a sign-in, which pauses
// the polls until Try again resumes them without a second Connect, and a
// failed read of an account shown connected before.
void ConnectReadsRecover(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 std::vector<MenuEntry> rows;std::string status;
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 for(int ending=0;ending<4;++ending){
  Harness h;auto& id=h.view.identity;id.known=true;id.state="ready";h.view.tournament.connect.confirm=true;
  id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
  id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
  const auto reply=[&](bool ok){
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=ok;id.failure=ok?"":"service_unavailable";h.Frame(0,2);
  };
  const auto answer=[&]{reply(true);};
  const auto until=[&](IdentityOp op){for(int i=0;i<10&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
  if(ending<2){
   // Linked accounts is reading Ember's service when the link for the other arrives.
   h.Screen("linked-accounts");
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"Linked accounts did not inspect its service");
   if(ending==1){id.inspected=id.bridges[0];answer();Check(sent().back()->op==IdentityOp::LinkList,"Linked accounts did not list its links");}
   h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   reply(false);
   Check(status!=loc::T("identity.failure.unreachable"),"A replaced Linked accounts read said it failed");
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"A replaced Linked accounts read held the journey back");
   id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
   Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","The journey did not read its service's account");
   id.discordUser.clear();id.discordName.clear();answer();
   Check(row("id-discord-connect")&&row("id-discord-connect")->enabled&&!row("dc-retry"),"The journey did not offer Connect");
  }else if(ending==2){
   // The journey's own reads fail: Try again reads them again.
   h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"The journey did not inspect its service");
   reply(false);
   Check(status==loc::T("identity.failure.unreachable")&&row("dc-retry")&&!row("id-discord-connect")->enabled,
    "A failed inspection did not offer Try again");
   h.Choose("dc-retry");
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"Try again did not inspect the service again");
   id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
   Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","The service's account was not read");
   reply(false);
   Check(row("dc-retry")&&!row("id-discord-connect")->enabled,"A failed account read did not offer Try again");
   h.Choose("dc-retry");
   Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","Try again did not read the account again");
   id.discordUser.clear();id.discordName.clear();answer();
   Check(row("id-discord-connect")&&row("id-discord-connect")->enabled&&!row("dc-retry"),"A read account did not offer Connect");
  }
  if(ending==3){
   // A sign-in on the journey's service: a poll fails partway.
   h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   Check(until(IdentityOp::BridgeInspect),"The journey did not inspect its service");
   id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
   Check(until(IdentityOp::DiscordStatus),"The journey did not read its account");
   id.discordUser.clear();id.discordName.clear();answer();
   h.Choose("id-discord-connect");answer();h.Frame(0,300);
   Check(sent().back()->op==IdentityOp::DiscordStatus&&sent().back()->bridge=="brg_2","No poll was sent");
   reply(false);
   Check(row("dc-retry")&&!row("id-discord-connect")->enabled,"A failed poll did not offer Try again in place of Connect");
   const auto paused=sent().size();h.Frame(0,300);
   Check(sent().size()==paused,"Polls went on after a failed poll");
   // Try again reads the account; the sign-in finished meanwhile.
   h.Choose("dc-retry");
   Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","Try again did not read the account");
   id.discordUser="333";id.discordName="kate";answer();
   Check(status==loc::Tf("connect.done","kate")&&row("dc-connected"),"Try again did not finish the sign-in");
   std::size_t connects=0;for(const auto* r:sent())connects+=r->op==IdentityOp::DiscordConnect;
   Check(connects==1,"Recovering sent another Connect");
   // A later read of that account that fails no longer shows it connected.
   h.view.tournament.connect.sequence=2;h.Frame(0,2);
   Check(until(IdentityOp::DiscordStatus),"The second visit did not read the account");
   reply(false);
   Check(!row("dc-connected")&&row("dc-retry"),"A failed read still showed the account connected");
  }
  SetMenuEntriesProbe({});SetMenuStatusProbe({});
 }
}
// A change the player asked for just before a connect link arrived still
// finishes, but it is not the new journey's: an unlink's follow-up read is
// not queued, and an unlock typed while the Ember ID screen was loading is
// sent after the link, and its refused submission drops none of the new
// journey's requests.
void RetainedChangesStayTheirs(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 std::vector<MenuEntry> rows;std::string status;
 for(int ending=0;ending<2;++ending){
  Harness h;auto& id=h.view.identity;id.known=true;id.state=ending==0?"ready":"locked";h.view.tournament.connect.confirm=true;
  id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
  id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
  const auto answer=[&]{
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
  };
  const auto until=[&](IdentityOp op){for(int i=0;i<10&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
  const auto link=[&]{h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);};
  std::size_t before=0;
  if(ending==0){
   // An unlink under Linked accounts finishes after the link arrived.
   h.Screen("linked-accounts");
   Check(until(IdentityOp::BridgeInspect),"Linked accounts did not inspect its service");
   id.inspected=id.bridges[0];answer();
   Check(sent().back()->op==IdentityOp::LinkList,"Linked accounts did not list its links");
   id.links={{"lnk_1","blumint","BluMint","PlayerOne"}};answer();for(int i=0;i<4;++i)answer();
   h.Choose("id-unlink:lnk_1");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(sent().back()->op==IdentityOp::LinkRemove,"Unlink was not sent");
   before=sent().size();link();answer();
  }else{
   // An unlock typed while the Ember ID screen's status is still out waits
   // behind it; the link arrives; the unlock's submission is refused.
   h.Screen("identity");
   Check(sent().back()->op==IdentityOp::Status,"The Ember ID screen did not ask for its status");
   h.Choose("id-unlock");ImGui::GetIO().AddInputCharactersUTF8("correct horse");h.Frame();
   ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
   Check(sent().back()->op==IdentityOp::Status,"The unlock did not wait behind the status");
   before=sent().size();link();
   // Only that one submission is refused: the frame that answers the status sends it.
   id.state="ready";
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;
   h.accept=false;h.Frame();h.accept=true;
   Check(sent().back()->op==IdentityOp::Unlock&&status==loc::T("error.queue_failed"),"The unlock was not the refused submission");
   h.Frame();
  }
  Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==other,"An earlier change held the journey back");
  id.inspected=id.bridges[1];id.inspectedDiscord=true;answer();
  Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_2","The journey did not read its service's account");
  for(std::size_t i=before;i<sent().size();++i)
   Check(!(sent()[i]->op==IdentityOp::LinkList&&sent()[i]->bridge=="brg_1"),"An earlier change's follow-up read was queued");
  SetMenuEntriesProbe({});SetMenuStatusProbe({});
 }
}
// Ember ID is on Home. Its screen leads with the matches, then Discord on
// Ember's own service, showing the connected account, which Connect Discord
// can also disconnect.
void EmberIdFromHome(){
 using namespace sf4e;using netplay::IdentityOp;
 Harness h;auto& id=h.view.identity;id.known=true;id.state="ready";
 id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
 const std::string ember="https://bridge.embernetplay.link";
 id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 const auto index=[&](const char* name){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;})-rows.begin();};
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto answer=[&]{
  h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
 };
 const auto until=[&](IdentityOp op){for(int i=0;i<10&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
 h.Screen("home");
 Check(row("identity")!=nullptr,"Home has no Ember ID entry");
 h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Ember ID on Home did not open its screen");
 Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"The Ember ID screen did not read Ember's own service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;answer();
 Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_1","The Ember ID screen did not read the Discord account");
 id.discordUser="274220342558756145";id.discordName="kate";answer();
 Check(row("tournament-matches")&&row("discord-connect")&&index("tournament-matches")<index("discord-connect")&&index("discord-connect")<index("linked-accounts"),
  "The Ember ID screen does not lead with matches, then Discord");
 Check(row("discord-connect")->value=="kate","The Ember ID screen does not show the connected Discord account");
 // Connect Discord shows it connected and can disconnect it.
 h.Choose("discord-connect");
 Check(until(IdentityOp::DiscordStatus),"Connect Discord did not read the account");
 answer();for(int i=0;i<4;++i)answer();
 Check(row("dc-connected")&&row("id-discord-remove"),"Connect Discord does not offer to disconnect");
 h.Choose("id-discord-remove");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent().back()->op==IdentityOp::DiscordRemove&&sent().back()->bridge=="brg_1","Disconnect did not name Ember's service");
 id.discordUser.clear();id.discordName.clear();answer();
 Check(!row("dc-connected")&&row("id-discord-connect")&&row("id-discord-connect")->enabled,"A disconnected account still shows connected");
 // Back on the Ember ID screen it reads Not connected.
 h.shell.Navigation().Return();h.Frame(0,2);for(int i=0;i<8;++i)answer();
 Check(row("discord-connect")&&row("discord-connect")->value==loc::T("identity.discord_none"),"The Ember ID screen still shows the account");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// Home guides the player: Ember ID says to start there before setup, and,
// once matches are read in the background, how many are ready to play, with
// a notice for each new one. The matches screen's empty states lead to
// Connect Discord.
void HomeGuides(){
 using namespace sf4e;using netplay::IdentityOp;using netplay::tournament::Command;
 const std::string ember="https://bridge.embernetplay.link";
 std::vector<MenuEntry> rows;std::string status;
 {
 Harness h;auto& id=h.view.identity;id.known=true;id.state="disabled";
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
 const auto played=[&]{std::vector<const Command*> out;for(const auto& a:h.actions)if(a.tournament.op!=Command::Op::None)out.push_back(&a.tournament);return out;};
 const auto answer=[&]{
  h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
 };
 // The state is not known yet: Home asks for it, and again after a failure.
 id.known=false;h.Screen("home");h.Frame();
 Check(!sent().empty()&&sent().back()->op==IdentityOp::Status,"Home did not learn the Ember ID's state");
 h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=0;h.view.identityRefusal="identity.refused.helper";h.Frame(0,2);
 h.view.identityRefusal.clear();
 Check(status!=loc::T("identity.refused.helper"),"A failed background read was said on Home");
 const auto tries=sent().size();h.Frame(0,1900);
 Check(sent().size()>tries&&sent().back()->op==IdentityOp::Status,"Home did not read the state again after a failure");
 id.known=true;answer();
 Check(row("identity")&&row("identity")->detail==loc::T("home.identity_start"),"Home does not say to start at Ember ID");
 // With an ID and Ember's service, the matches are read in the background.
 id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
 id.bridges={{"brg_1",ember,"Ember"}};h.Frame(0,2);
 Check(sent().back()->op==IdentityOp::BridgeList,"Home did not learn the services");
 answer();h.Frame(0,2);
 Check(!played().empty()&&played().back()->op==Command::Op::Refresh&&played().back()->bridgeId=="brg_1","Home did not read the matches");
 Check(row("identity")->detail==loc::T("home.identity_detail"),"Home says to start with an ID and a service");
 auto& t=h.view.tournament;t.list.bridge="brg_1";
 netplay::tournament::Assignment match;match.matchId="emt_1";match.state="ready";match.profile="ember-room-v1";match.gamesToWin=2;
 t.list.items={match};h.Frame(0,2);
 Check(status==loc::T("tournament.assigned_notice"),"A new match was not announced");
 Check(row("identity")->detail==loc::Tf("home.identity_matches",1),"Home does not count the ready match");
 // Read again a minute later.
 const auto reads=played().size();h.Frame(0,3700);
 Check(played().size()>reads&&played().back()->op==Command::Op::Refresh,"The matches were not read again");
 }
 // The matches screen's empty states lead to Connect Discord.
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 Harness fresh;auto& other=fresh.view.identity;other.known=true;other.state="disabled";
 fresh.Screen("tournament-matches");fresh.Frame();
 Check(row("id-linked-unavailable")&&row("discord-connect"),"Without an ID the matches screen does not lead to Connect Discord");
 other.state="ready";other.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";other.fingerprint="j25zrhe6-pmdhvlja";fresh.Frame(0,2);
 Check(row("tm-no-service")&&row("tm-no-service")->detail==loc::T("tournament.needs_discord_detail")&&row("discord-connect"),
  "Without a service the matches screen does not lead to Connect Discord");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// The one-link attempt's guards: a second link while Discord's page is being
// opened opens no second page; Cancel during setup stops it, and a later link
// for the same service shows it rather than starting again; changing account
// finishes only when another account is read; a service the player removed
// is not trusted by itself.
void OnboardingGuards(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link";
 std::vector<MenuEntry> rows;std::string status;
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 for(int part=0;part<3;++part){
  Harness h;auto& id=h.view.identity;id.known=true;
  id.state=part==0?"ready":"disabled";
  if(part==0){id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";id.bridges={{"brg_1",ember,"Ember"}};}
  if(part==2)id.removedBridges={"brg_1"};
  h.Frame();
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
  const auto answer=[&]{
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
  };
  const auto until=[&](IdentityOp op){for(int i=0;i<12&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
  const auto count=[&](IdentityOp op){std::size_t n=0;for(const auto* r:sent())n+=r->op==op;return n;};
  const auto link=[&](std::uint64_t sequence){h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=sequence;h.Frame(0,2);};
  if(part==0){
   // A second link while Discord's page is being opened opens no second one.
   link(1);
   Check(until(IdentityOp::BridgeInspect),"The link did not inspect its service");
   id.inspected=id.bridges[0];id.inspectedDiscord=true;answer();
   Check(until(IdentityOp::DiscordStatus),"The link did not read the account");
   id.discordUser.clear();id.discordName.clear();answer();
   Check(sent().back()->op==IdentityOp::DiscordConnect,"The link did not open Discord");
   const auto opening=sent().back()->ticket;
   link(2);
   h.view.identityTicket=opening;h.view.identityRequest=id.requestId=opening+100;id.ok=true;h.Frame(0,2);
   Check(row("dc-waiting")&&count(IdentityOp::DiscordConnect)==1,"A second link opened a second page or lost the wait");
   // Changing account: the old account read back keeps waiting; another finishes.
   id.discordUser="111";id.discordName="kate";h.Frame(0,300);until(IdentityOp::DiscordStatus);answer();
   Check(row("dc-connected")&&status==loc::Tf("connect.done","kate"),"The first account did not finish the sign-in");
   h.Choose("id-discord-connect");
   Check(sent().back()->op==IdentityOp::DiscordConnect,"Use a different Discord account did not open Discord");
   answer();h.Frame(0,300);
   Check(sent().back()->op==IdentityOp::DiscordStatus,"The change of account was not read");
   answer();h.Frame(0,300);answer();
   Check(row("dc-waiting")&&!row("dc-connected"),"The old account read back ended the change of account");
   id.discordUser="222";id.discordName="sam";h.Frame(0,300);answer();
   Check(row("dc-connected")&&row("dc-connected")->value=="sam","The new account did not finish the change");
  }else if(part==1){
   // Cancel during setup stops it; a later link for the service only shows it.
   link(1);
   Check(row("dc-progress")&&row("dc-cancel"),"Setting up offers no Cancel");
   h.Choose("dc-cancel");h.Frame(0,2);for(int i=0;i<6;++i)answer();
   Check(count(IdentityOp::Enable)==0&&status==loc::T("connect.cancelled"),"Cancel did not stop the setup");
   link(2);for(int i=0;i<6;++i)answer();
   Check(count(IdentityOp::Enable)==0&&row("dc-go"),"A link restarted a cancelled attempt");
   h.Choose("dc-go");
   Check(sent().back()->op==IdentityOp::Enable,"Connect did not restart the attempt");
  }else{
   // A service the player removed is offered to trust, not trusted.
   link(1);
   Check(until(IdentityOp::Enable),"The link did not create the Ember ID");
   id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";answer();
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"The link did not look up Ember's own service");
   id.inspected={"brg_1",ember,"Ember"};answer();for(int i=0;i<4;++i)answer();
   Check(row("id-approve")&&count(IdentityOp::BridgeApprove)==0,"A removed service was trusted by itself");
  }
  SetMenuEntriesProbe({});SetMenuStatusProbe({});
 }
}
// A link meets a locked Ember ID: Unlock shows, not an endless Setting up,
// and unlocking goes on to Discord by itself. Cancel is there at every step
// of setup. Hiding Ember during setup stops it: reopening sends nothing more.
void OnboardingSteps(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link";
 std::vector<MenuEntry> rows;std::string status;
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 for(int part=0;part<3;++part){
  Harness h;auto& id=h.view.identity;id.known=true;id.state=part==0?"locked":"disabled";h.Frame();
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
  const auto answer=[&]{
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
  };
  const auto until=[&](IdentityOp op){for(int i=0;i<12&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
  const auto count=[&](IdentityOp op){std::size_t n=0;for(const auto* r:sent())n+=r->op==op;return n;};
  const auto type=[&](const char* text){
   ImGui::GetIO().AddInputCharactersUTF8(text);h.Frame();
   ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
  };
  const auto ready=[&]{id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";};
  h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
  if(part==0){
   for(int i=0;i<6;++i)answer();
   Check(row("id-unlock")&&!row("dc-progress"),"A locked ID hid Unlock behind Setting up");
   h.Choose("id-unlock");type("correct horse");
   Check(sent().back()->op==IdentityOp::Unlock,"Unlock did not send");
   ready();answer();
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"Unlocking did not go on");
   id.inspected={"brg_1",ember,"Ember"};id.inspectedDiscord=true;answer();
   Check(sent().back()->op==IdentityOp::BridgeApprove,"Unlocking did not go on to trust Ember's service");
   id.bridges={{"brg_1",ember,"Ember"}};
   Check(until(IdentityOp::DiscordStatus),"Unlocking did not go on to read the account");
   id.discordUser.clear();id.discordName.clear();answer();
   Check(sent().back()->op==IdentityOp::DiscordConnect,"Unlocking did not go on to open Discord");
   // Cancel while Discord's page is open ends the sign-in on the service
   // too. A service that cannot end it says nothing, and Connect works again.
   answer();Check(row("dc-waiting")&&row("dc-cancel"),"Not waiting for Discord");
   h.Choose("dc-cancel");h.Frame(0,2);
   // An account read may still be out from the wait.
   Check(until(IdentityOp::DiscordCancel)&&sent().back()->bridge=="brg_1","Cancel did not end the sign-in on the service");
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=false;id.failure="bridge_unavailable";h.Frame(0,2);
   id.ok=true;id.failure.clear();
   Check(status==loc::T("connect.cancelled"),"A service that could not end the sign-in was reported");
   Check(row("id-discord-connect")&&row("id-discord-connect")->enabled&&!row("dc-waiting"),"Connect is not offered again after Cancel");
   h.Choose("id-discord-connect");h.Frame(0,2);
   Check(sent().back()->op==IdentityOp::DiscordConnect,"Connect did not open Discord again after Cancel");
  }else if(part==1){
   // Cancel at each step: creating the ID, looking up and trusting the service.
   Check(row("dc-cancel")!=nullptr,"No Cancel while the state is read");
   Check(until(IdentityOp::Enable)&&row("dc-cancel"),"No Cancel while the ID is created");
   ready();answer();
   Check(until(IdentityOp::BridgeInspect)&&row("dc-cancel"),"No Cancel while the service is looked up");
   id.inspected={"brg_1",ember,"Ember"};id.inspectedDiscord=true;answer();
   Check(sent().back()->op==IdentityOp::BridgeApprove&&row("dc-cancel"),"No Cancel while the service is trusted");
   h.Choose("dc-cancel");id.bridges={{"brg_1",ember,"Ember"}};for(int i=0;i<8;++i)answer();
   // The trust already sent finishes; nothing goes on to open Discord.
   Check(count(IdentityOp::DiscordConnect)==0&&row("id-discord-connect")&&!row("dc-cancel"),"Cancel did not stop the setup");
   Check(count(IdentityOp::DiscordCancel)==0,"Cancel before Discord's page asked the service to end a sign-in");
  }else{
   // Hiding Ember while the state is read stops the setup.
   Check(sent().back()->op==IdentityOp::Status,"The link did not read the state");
   const auto before=sent().size();
   h.shell.Conceal();h.Frame();
   h.shell.Navigation().Home();h.shell.Navigation().Push("discord-connect");h.Frame(0,2);
   for(int i=0;i<8;++i)answer();
   for(std::size_t i=before;i<sent().size();++i)
    Check(sent()[i]->op!=IdentityOp::Enable&&sent()[i]->op!=IdentityOp::DiscordConnect,"Reopening Ember resumed a hidden setup");
   Check(row("dc-go")!=nullptr,"A stopped setup does not offer Connect");
  }
  SetMenuEntriesProbe({});SetMenuStatusProbe({});
 }
}
// A link whose first read of the state fails offers Try again, which goes on
// by itself. Visiting the Ember ID screen reads Ember's Discord account
// without changing the tournament service the player selected. A failed read
// of a service whose sign-in is off offers Try again, not "off".
void OnboardingRecovers(){
 using namespace sf4e;using netplay::IdentityOp;using netplay::tournament::Command;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 std::vector<MenuEntry> rows;std::string status;
 const auto row=[&](const char* name)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;};
 for(int part=0;part<3;++part){
  Harness h;auto& id=h.view.identity;
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  const auto sent=[&]{std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;};
  const auto played=[&]{std::vector<const Command*> out;for(const auto& a:h.actions)if(a.tournament.op!=Command::Op::None)out.push_back(&a.tournament);return out;};
  const auto answer=[&]{
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();h.Frame(0,2);
  };
  const auto until=[&](IdentityOp op){for(int i=0;i<12&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;};
  if(part==0){
   // The state is not known yet when the link arrives, and its read is refused.
   id.known=false;h.Frame();
   h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   // Home's own read was still out when the link arrived; the link's follows.
   for(int i=0;i<2;++i){
    Check(sent().back()->op==IdentityOp::Status,"The link did not read the state");
    h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=0;h.view.identityRefusal="identity.refused.helper";h.Frame(0,2);
   }
   h.view.identityRefusal.clear();h.Frame(0,400);
   Check(row("dc-retry")&&!row("dc-progress"),"A failed first read left Setting up with nothing to wait for");
   h.Choose("dc-retry");
   Check(sent().back()->op==IdentityOp::Status,"Try again did not read the state");
   id.known=true;id.state="disabled";
   Check(until(IdentityOp::Enable),"Try again did not go on by itself");
  }else if(part==2){
   id.known=true;id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
   id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
   const auto next=[&](IdentityOp op){
    const auto from=sent().size();
    for(int i=0;i<12&&!(sent().size()>from&&sent().back()->op==op);++i)answer();
    return sent().size()>from&&sent().back()->op==op;
   };
   h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.view.tournament.connect.confirm=true;h.Frame(0,2);
   Check(until(IdentityOp::BridgeInspect),"Connect Discord did not read the service");
   id.inspected=id.bridges[0];id.inspectedDiscord=false;id.inspectedDiscordAccounts=false;answer();
   Check(row("dc-off")&&!row("dc-retry"),"A service without sign-in was not said");
   // The next visit's read fails.
   h.Screen("home");h.Screen("discord-connect");
   Check(next(IdentityOp::BridgeInspect),"Connect Discord did not read the service again");
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=false;id.failure="bridge_unavailable";h.Frame(0,2);
   id.ok=true;id.failure.clear();
   Check(row("dc-retry")&&!row("dc-off"),"A failed read of a service without sign-in said it is off");
   h.Choose("dc-retry");
   Check(next(IdentityOp::BridgeInspect),"Try again did not read the service");
   answer();
   Check(row("dc-off")&&!row("dc-retry"),"A good read after a failed one did not say off");
  }else{
   id.known=true;id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
   id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
   // The player selects the other service for its matches.
   h.Screen("tournament-matches");for(int i=0;i<6;++i)answer();
   h.FocusOn("id-bridge");h.Press(MenuInput::Select);h.Press(MenuInput::Down);h.Press(MenuInput::Select);
   for(int i=0;i<6;++i)answer();
   Check(row("id-bridge")&&row("id-bridge")->value=="Other","The other service was not selected");
   // The Ember ID screen reads Ember's own Discord account.
   h.Screen("identity");
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"The Ember ID screen did not read Ember's service");
   id.inspected=id.bridges[0];id.inspectedDiscord=true;answer();
   Check(until(IdentityOp::DiscordStatus)&&sent().back()->bridge=="brg_1","The Ember ID screen did not read the account");
   id.discordUser="111";id.discordName="kate";answer();
   Check(row("discord-connect")&&row("discord-connect")->value=="kate","The Ember ID screen does not show the account");
   // A failed read of Ember's service no longer shows its account as current.
   h.Screen("home");h.Screen("identity");
   Check(until(IdentityOp::BridgeInspect)&&sent().back()->origin==ember,"The Ember ID screen did not read Ember's service again");
   h.view.identityTicket=sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=false;id.failure="service_unavailable";h.Frame(0,2);
   Check(row("discord-connect")&&row("discord-connect")->value!="kate","A failed read still showed Ember's account as current");
   // Back on the matches, the selection is the player's.
   h.Screen("tournament-matches");for(int i=0;i<6;++i)answer();
   Check(row("id-bridge")&&row("id-bridge")->value=="Other","Visiting Ember ID changed the selected service");
   Check(!played().empty()&&played().back()->bridgeId=="brg_2","The matches are no longer read from the selected service");
  }
  SetMenuEntriesProbe({});SetMenuStatusProbe({});
 }
}
int main(){try{IdentityJourneys();TournamentRoom();RoomLinks();DiscordAndFirstLink();DiscordConnectLink();DiscordWaitsForItsService();DiscordConnectKeepsItsService();FirstSignInRetires();ConnectReadsRecover();RetainedChangesStayTheirs();EmberIdFromHome();HomeGuides();OnboardingGuards();OnboardingSteps();OnboardingRecovers();DiscordConnectPastedLink();std::cout<<"Identity and tournament journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
