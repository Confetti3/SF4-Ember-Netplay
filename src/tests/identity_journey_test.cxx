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
 h.Screen("online");h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Online play does not open the Ember ID screen");
 Check(h.sent().size()==1&&h.sent().back()->op==IdentityOp::Status,"Opening the Ember ID screen did not ask for its status");
 answer();
 // Under Wine the key needs a passphrase, typed twice, before it can be created.
 h.Frame();Check(h.row("id-enable")&&!h.row("id-enable")->enabled,"Create was offered before a passphrase");
 Check(h.row("id-enable")->detail==loc::T("identity.enable_passphrase_detail"),"Create under Wine does not point to the passphrase");
 // On Windows the key is tied to the Windows account and needs no passphrase.
 id.passphraseRequired=false;h.Frame();
 Check(!h.row("id-new-passphrase")&&h.row("id-enable")&&h.row("id-enable")->detail==loc::T("identity.enable_detail"),"Create on Windows does not describe the Windows account");
 id.passphraseRequired=true;h.Frame();
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
 // A match that expired shows as information, by the bridge's word or by this PC's clock, and cannot be played.
 {
  h.view.unixNow=5000;
  auto expired=playable;expired.matchId="emt_3";expired.state="expired";expired.expiresAt=100;
  auto lapsed=playable;lapsed.matchId="emt_4";lapsed.expiresAt=4000;
  auto skewed=playable;skewed.matchId="emt_5";skewed.expiresAt=4800;
  t.list.items={playable,organizer,expired,lapsed,skewed};h.Frame();
  for(const char* id:{"tm-play:emt_3","tm-play:emt_4"}){
   Check(h.row(id)&&h.row(id)->info&&!h.row(id)->enabled,"An expired match can be played");
   Check(h.row(id)->value==loc::T("tournament.expired")&&h.row(id)->detail.find(loc::T("tournament.expired_detail"))!=std::string::npos,"An expired match is not labelled Expired");
  }
  Check(h.row("tm-play:emt_5")->enabled&&h.row("tm-play:emt_5")->value!=loc::T("tournament.expired"),"A match within the clock skew was taken for expired");
  Check(h.row("tm-play:emt_1")->enabled,"A match with no expiry cannot be played");
  // Running, awaiting reports, in review, or a state Ember does not know: the bridge's
  // state stands, with no Play, and past the match's time too, which the bridge holds it beyond.
  std::vector<netplay::tournament::Assignment> busy;
  for(const char* state:{"running","awaiting_reports","needs_review","created"})
   for(const std::uint64_t expires:{std::uint64_t(0),std::uint64_t(4000)}){
    auto m=playable;m.matchId="emt_"+std::string(state)+std::to_string(expires);m.state=state;m.expiresAt=expires;busy.push_back(m);
   }
  t.list.items=busy;h.Frame();
  for(const auto& m:busy){
   const auto* row=h.row(("tm-play:"+m.matchId).c_str());
   Check(row&&row->info&&!row->enabled,"A match running or in review can be played");
   Check(row->value==loc::Tf("tournament.score",1u,2u,3)&&row->detail.find(loc::T("tournament.state.waiting_permit"))!=std::string::npos,
    "A match running or in review does not show its score and that it waits for the service");
   Check(row->detail.find(loc::T("tournament.expired_detail"))==std::string::npos,"A match the bridge holds past its time is labelled Expired");
  }
  t.list.items={playable,organizer};h.view.unixNow=0;h.Frame();
 }
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
// Play looks at the match again when pressed: a row older than the list, for a
// match no longer waiting for its players, past its time or gone, or pressed while
// the player is in a room, sends nothing; a match that can be played sends Play.
void PlayRechecks(){
 using namespace sf4e;using netplay::tournament::Command;
 IdentityPanel panel;ShellView v;MenuNavigation nav;std::vector<ShellAction> sent;double now=0;
 const auto submit=[&](ShellAction a){sent.push_back(std::move(a));return true;};
 v.identity.known=true;v.identity.state="ready";v.identity.bridges={{"brg_1","https://bridge.example","Example"}};v.unixNow=5000;
 panel.OpenMatch("brg_1","emt_1");
 netplay::tournament::Assignment match;match.matchId="emt_1";match.profile="ember-room-v1";match.gamesToWin=2;
 v.tournament.list.bridge="brg_1";
 const auto press=[&](const char* state,std::uint64_t expires,bool listed){
  match.state=state;match.expiresAt=expires;v.tournament.list.items.clear();if(listed)v.tournament.list.items.push_back(match);
  MenuAction a;a.kind=MenuAction::Activate;a.id="tm-play:emt_1";panel.Activate(a,v,nav);
  sent.clear();panel.Update(v,"tournament-matches",submit,now+=1);
  return std::any_of(sent.begin(),sent.end(),[](const ShellAction& s){return s.tournament.op==Command::Op::Play;});
 };
 for(const char* state:{"running","awaiting_reports","needs_review","created","expired","completed"})
  Check(!press(state,0,true),"Play went out for a match that is not waiting for its players");
 Check(!press("running",4000,true),"Play went out for a match the bridge holds past its time");
 Check(!press("ready",4000,true),"Play went out for a match past its time");
 Check(!press("ready",0,false),"Play went out for a match the list no longer holds");
 v.session.room=netplay::RoomState::Joined;Check(!press("ready",0,true),"Play went out while the player is in a room");
 v.session.room=netplay::RoomState::Idle;
 Check(press("between_games",9000,true)&&sent.back().tournament.matchId=="emt_1"&&sent.back().tournament.bridgeId=="brg_1","Play did not go out for a match that can be played");
}
// A match that ended for a lost connection says so, and that the tournament
// is setting up a new one (Ember does not start it), and its service is read
// every few seconds for a few minutes,
// from any screen, so a match the bridge sets up again is told at once, on the
// matches screen too. Past that the matches screen is read only when asked.
void RestartWatch(){
 using namespace sf4e;using netplay::tournament::Command;using netplay::tournament::Phase;
 Journey h;auto& id=h.view.identity;id.known=true;h.ready();
 id.bridges={{"brg_1","https://bridge.embernetplay.link","Ember"}};
 h.Screen("home");h.Frame(0,2);h.answer();h.Frame(0,2);
 const auto reads=[&]{const auto all=h.played();return std::count_if(all.begin(),all.end(),[](const Command* c){return c->op==Command::Op::Refresh;});};
 Check(reads()>=1,"Home did not read the matches");
 auto& t=h.view.tournament;t.list.bridge="brg_1";t.bridgeId="brg_1";t.matchId="emt_1";
 t.phase=Phase::InRoom;h.Frame(0,2);
 const auto before=reads();
 t.phase=Phase::Finished;t.reason=netplay::tournament::ConnectionLostReason;h.Frame(0,2);
 Check(h.status==loc::T("tournament.connection_lost_notice"),"A match ended for a lost connection was announced as complete");
 Check(reads()==before+1,"The matches were not read when the match ended for a lost connection");
 h.Screen("tournament-matches");for(int i=0;i<6;++i)h.answer();
 Check(h.row("tm-current")&&h.row("tm-current")->value==loc::T("tournament.connection_lost_notice"),
  "The matches screen does not say the connection was lost");
 const auto onScreen=reads();h.Frame(0,6,1.f);
 Check(reads()>onScreen,"The matches screen was not read again within seconds");
 netplay::tournament::Assignment next;next.matchId="emt_2";next.state="ready";next.profile="ember-room-v1";next.gamesToWin=2;
 t.list.items={next};h.Frame(0,2);
 Check(h.status==loc::T("tournament.assigned_notice"),"The match set up again was not told on the matches screen");
 h.Frame(0,200,1.f);
 const auto later=reads();h.Frame(0,30,1.f);
 Check(reads()==later,"The matches screen was still read every few seconds minutes later");
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
// Settings > Discord names the Discord account tournaments use: the one on the
// selected service, read again each time the screen opens, or Not connected
// without an Ember ID or a service. Left and Right leave it alone; Select
// opens Connect Discord for that service, and Back returns to Settings > Discord.
void TournamentAccount(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 Journey h;auto& id=h.view.identity;id.known=true;id.state="disabled";
 const auto account=[&]{return h.row("tournament-account");};
 const auto open=[&]{h.Screen("settings");h.Choose("discord");};
 open();
 Check(account()&&h.index("tournament-account")==h.index("invites")+1,"Settings > Discord has no Tournament account row after its settings");
 Check(account()->value==loc::T("identity.discord_none")&&!account()->adjustable&&!account()->info,"Without an Ember ID the Tournament account is not Not connected");
 const auto before=h.actions.size();
 h.FocusOn("tournament-account");h.Press(MenuInput::Right);h.Press(MenuInput::Left);
 Check(h.actions.size()==before&&account()->value==loc::T("identity.discord_none"),"Left or Right changed the Tournament account");
 h.Press(MenuInput::Select);
 Check(h.trail()=="home>settings>discord>discord-connect"&&h.row("dc-go"),"The Tournament account did not open Connect Discord");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="discord","Back from Connect Discord did not return to Settings > Discord");
 // With an ID and Ember's service: the account on it.
 h.ready();id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};
 open();
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Settings > Discord did not read the selected service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_1","Settings > Discord did not read the Discord account");
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(account()->value=="kate"&&account()->userText,"The Tournament account does not name the connected account");
 h.Choose("tournament-account");h.until(IdentityOp::DiscordStatus);h.answer();
 Check(h.row("dc-connected")&&h.row("dc-connected")->value=="kate","Connect Discord does not show the same account");
 // Each visit reads it again.
 h.Press(MenuInput::Back);id.discordUser.clear();id.discordName.clear();
 Check(h.until(IdentityOp::DiscordStatus),"Settings > Discord did not read the account again");
 h.answer();
 Check(account()->value==loc::T("identity.discord_none")&&!account()->userText,"A disconnected account is still named");
 // Another selected service: its own account, and Connect Discord for it.
 h.Screen("linked-accounts");for(int i=0;i<8;++i)h.answer();
 h.FocusOn("id-bridge");h.Press(MenuInput::Select);h.Press(MenuInput::Down);h.Press(MenuInput::Select);
 id.inspected=id.bridges[1];h.until(IdentityOp::DiscordStatus);
 id.discordUser="111";id.discordName="sam";h.answer();
 open();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","Settings > Discord did not read the other selected service");
 h.answer();
 Check(account()->value=="sam","The Tournament account does not show the selected service's account");
 h.Choose("tournament-account");h.until(IdentityOp::DiscordStatus);h.answer();
 Check(h.row("dc-service")&&h.row("dc-service")->value=="Other"&&h.row("dc-connected")&&h.row("dc-connected")->value=="sam",
  "Connect Discord did not open for the selected service");
 // No service: nothing is linked.
 id.bridges.clear();open();for(int i=0;i<4;++i)h.answer();
 Check(account()->value==loc::T("identity.discord_none"),"Without a service the Tournament account is not Not connected");
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
int main(){try{IdentityJourneys();PlayRechecks();RestartWatch();TournamentRoom();TournamentAccount();RoomLinks();RunDiscordConnectJourneys();RunBuiltInServiceJourneys();RunSignInSiteJourneys();std::cout<<"Identity and tournament journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
