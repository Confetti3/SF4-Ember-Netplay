// Discord's browser sign-in as the Ember ID screens run it, and the
// tournament desk on Ember's bracket site. Run from the identity journey
// test's main.
#include "identity_journey_support.hxx"
#include <nlohmann/json.hpp>
namespace {
const std::string ember="https://bridge.embernetplay.link";

// A ready Ember ID that trusts Ember's own service.
void Ready(Journey& h){
 auto& id=h.view.identity;id.known=true;h.ready();id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
}
// Answers until the profile of Ember's own service is asked for, then gives it.
void Profile(Journey& h){
 using sf4e::netplay::IdentityOp;
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Ember's own service was not read");
 h.view.identity.inspected=h.view.identity.bridges[0];h.answer();
}
// A service that offers Discord sign-in and keeps Discord accounts.
void Both(Journey& h){
 auto& id=h.view.identity;id.inspected=id.bridges[0];id.inspectedDiscord=id.inspectedDiscordAccounts=true;
}
// A Discord sign-in waiting for the browser, through a connect link.
void DiscordWaiting(Journey& h){
 using sf4e::netplay::IdentityOp;auto& id=h.view.identity;
 h.view.tournament.connect.bridge="brg_1";++h.view.tournament.connect.sequence;h.Frame(0,2);
 Check(h.until(IdentityOp::BridgeInspect),"The connect link did not read its service");
 Both(h);h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"The connect link did not read Discord's account");
 id.discordUser.clear();id.discordName.clear();h.answer();
 Check(h.sent().back()->op==IdentityOp::DiscordConnect,"The connect link did not open Discord");
 h.answer();
 Check(h.row("dc-waiting")!=nullptr,"Discord is not waited for");
}

// Discord's change of account keeps the wait already running as it always has.
void DiscordKeepsItsWait(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 DiscordWaiting(h);
 h.Screen("linked-accounts");
 Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not read the service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"Linked accounts did not read Discord's account");
 h.answer();for(int i=0;i<4;++i)h.answer();
 Check(h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled,"Discord's row offers no Connect while it waits");
 h.Choose("id-discord-connect");
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_1","Connect did not open Discord again");
 Check(h.count(IdentityOp::DiscordCancel)==0,"Opening Discord again cancelled its own sign-in");
 h.Screen(DiscordConnectScreen);
 Check(h.row("dc-waiting")&&!h.row("dc-progress"),"Opening Discord again stopped showing the wait");
 h.answer(false,"browser_unavailable");
 h.Frame(0,300);
 Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_1","The wait stopped reading the account");
 Check(h.count(IdentityOp::DiscordCancel)==0,"The wait was cancelled");
}

// Connect Discord started again while Discord's own page is waited for ends
// that sign-in on its service first.
void DiscordGoEndsItsWait(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 DiscordWaiting(h);
 // The service is forgotten, so Connect Discord offers to set it up again.
 id.bridges.clear();h.Screen("home");h.Screen(DiscordConnectScreen);
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Connect Discord did not look Ember's service up");
 id.inspected={"brg_1",ember,"Ember"};h.answer();for(int i=0;i<4;++i)h.answer();
 Check(h.row("dc-go")!=nullptr,"Connect Discord does not offer to start again");
 const auto cancels=h.count(IdentityOp::DiscordCancel);
 h.Choose("dc-go");h.Frame(0,2);
 Check(h.count(IdentityOp::DiscordCancel)==cancels+1,"Starting again did not end Discord's waiting sign-in");
}

// A link for the service Discord's page was opened for, from Linked
// accounts, shows that sign-in rather than opening a second page.
void LinkForTheWaitedService(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 h.Screen("linked-accounts");
 Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not read the service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"Linked accounts did not read Discord's account");
 id.discordUser.clear();id.discordName.clear();h.answer();for(int i=0;i<4;++i)h.answer();
 h.Choose("id-discord-connect");
 Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Connect did not open Discord");
 h.answer();
 const auto opened=h.count(IdentityOp::DiscordConnect);
 h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
 for(int i=0;i<8;++i)h.answer();
 Check(h.count(IdentityOp::DiscordConnect)==opened&&h.count(IdentityOp::DiscordCancel)==0,
  "A link for the service Discord's page waits on started the sign-in again");
}

// Connect Discord with an account connected on a service that keeps Discord
// accounts but does not offer Discord sign-in: the account can be unlinked,
// but another one cannot be connected. Where sign-in is offered, it can.
void DiscordChangeNeedsSignIn(){
 using namespace sf4e;using netplay::IdentityOp;
 for(const bool offered:{false,true}){
  Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
  h.Screen(DiscordConnectScreen);
  Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Connect Discord did not read Ember's service");
  id.inspected=id.bridges[0];id.inspectedDiscord=offered;id.inspectedDiscordAccounts=true;h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"Connect Discord did not read Discord's account");
  id.discordUser="274220342558756145";id.discordName="player1";h.answer();for(int i=0;i<4;++i)h.answer();
  Check(h.row("dc-connected")&&h.row("id-discord-remove")&&h.row("id-discord-remove")->enabled,"A connected Discord account cannot be unlinked");
  Check(h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled==offered,
   offered?"Another Discord account cannot be connected where sign-in is offered":"Another Discord account is offered where sign-in is off");
 }
}

// Signing in again finishes at once: the service connects the account anew,
// which its read says, so the wait does not run to its deadline. So does
// another account that connected at the same time. A read of the connection
// the page started from does not.
void ConnectedAgain(){
 using namespace sf4e;using netplay::IdentityOp;
 for(const bool same:{true,false}){
  Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
  const auto set=[&](const char* user,const char* name,std::uint64_t at){id.discordUser=user;id.discordName=name;id.discordConnectedAt=at;};
  h.Screen(DiscordConnectScreen);
  Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Connect Discord did not read Ember's service");
  id.inspected=id.bridges[0];id.inspectedDiscord=id.inspectedDiscordAccounts=true;h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"Connect Discord did not read Discord's account");
  set("111","player1",10);h.answer();for(int i=0;i<4;++i)h.answer();
  Check(h.row("dc-connected")&&h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled,"Another account is not offered");
  h.Choose("id-discord-connect");
  Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Connect did not ask the helper to open the page");
  h.answer();
  Check(h.row("dc-waiting")!=nullptr,"The page is not waited for");
  h.Frame(0,300);
  Check(h.sent().back()->op==IdentityOp::DiscordStatus,"The account was not read while waiting");
  h.answer();
  Check(h.row("dc-waiting")!=nullptr,"The connection the page started from ended the wait");
  if(same)set("111","player1",20);else set("222","player2",10);
  h.Frame(0,300);
  Check(h.until(IdentityOp::DiscordStatus),"The account was not read again after the connection changed");
  h.answer();
  const char* name=same?"player1":"player2";
  Check(h.status==loc::Tf("connect.done",name)&&!h.row("dc-waiting")&&h.row("dc-connected"),
   same?"Signing in again with the same account did not finish":"Another account connected at the same time did not finish");
 }
}
// The organizer runs a tournament on the bracket site Ember's own service
// names, from the game alone: offered once the service names a site, the
// invite asked for until it is redeemed, a tournament made from its title
// with its date and first-to saved as they change, moved through its states
// as the site allows, its bracket started, a set held and a game decided,
// and a draft deleted.
void DeskInGame(){
 using namespace sf4e;using netplay::IdentityOp;
 {Journey plain;Ready(plain);plain.Screen("identity");Profile(plain);for(int i=0;i<6;++i)plain.answer();
  Check(!plain.row("tournament-desk"),"The desk was offered on a service without a bracket site");}
 Journey h;Ready(h);auto& id=h.view.identity;
 const auto settle=[&]{for(int i=0;i<3;++i)h.answer();};
 const auto& sent=[&]{return h.sent().back();};
 id.inspectedDesk=true;h.Screen("identity");Profile(h);for(int i=0;i<6;++i)h.answer();
 Check(h.row("tournament-desk")!=nullptr,"The desk was not offered on a service with a bracket site");
 h.Choose("tournament-desk");
 Check(h.until(IdentityOp::DeskRead)&&sent()->target.empty(),"The desk did not read the tournaments");
 id.deskAccess={true,true,false};settle();
 Check(h.row("dk-invite")&&!h.row("dk-new"),"The invite was not asked for first");
 h.Choose("dk-invite");h.type("ABCDE-FGHJK");
 Check(sent()->op==IdentityOp::DeskRedeem&&sent()->command=="ABCDE-FGHJK","The invite was not redeemed");
 id.deskAccess={true,true,true};settle();
 Check(h.row("dk-new")&&h.row("dk-new")->enabled&&h.row("dk-open"),"A new tournament cannot be made in the game");
 h.Choose("dk-new");settle();
 Check(h.shell.Navigation().Screen()=="desk-draft"&&h.row("dk-d-title")&&!h.row("dk-d-ft"),"A new tournament does not start with its title");
 h.Choose("dk-d-title");h.type("Thursday");
 Check(sent()->op==IdentityOp::DeskAct&&sent()->target=="/tournaments"&&sent()->command==R"({"title":"Thursday"})","The title did not make a tournament");
 id.deskMade="t9";h.answer();id.deskMade.clear();
 netplay::DeskEvent made;made.id="t9";made.title="Thursday";made.status="draft";made.actions={"plan","cancel"};id.deskEvents={made};
 for(int i=0;i<3&&!h.row("dk-d-ft");++i)h.answer();
 Check(h.row("dk-d-ft")&&h.row("dk-d-checkin")&&h.row("dk-d-discord")&&h.row("dk-d-delete"),"The draft does not offer its settings");
 h.FocusOn("dk-d-day");h.Press(MenuInput::Right);
 Check(sent()->target=="/tournaments/t9"&&sent()->command.find("\"starts_at\":")!=std::string::npos,"The date was not saved on the site");
 settle();
 h.FocusOn("dk-d-ft");h.Press(MenuInput::Right);
 Check(sent()->target=="/tournaments/t9"&&sent()->command==R"({"games_to_win":3})","The first-to was not saved on the site");
 settle();
 h.Choose("dk-d-open");settle();
 Check(h.shell.Navigation().Screen()=="desk-event"&&h.row("dk-move:plan")&&h.row("dk-move:cancel")&&!h.row("dk-start"),
  "The tournament does not offer the moves the site allows");
 h.Choose("dk-move:plan");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent()->target=="/tournaments/t9/state"&&sent()->command==R"({"action":"plan"})","The move was not sent");
 id.deskEvents[0].status="sign_ups";id.deskEvents[0].actions={"close_sign_ups","start","cancel"};settle();
 Check(h.row("dk-start")&&h.row("dk-start")->enabled,"Start was not offered once the site allowed it");
 h.Choose("dk-start");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent()->target=="/tournaments/t9/state"&&sent()->command==R"({"action":"start"})","Start was not sent");
 id.deskEvents[0].status="live";id.deskEvents[0].actions={"reset","cancel"};
 netplay::DeskSet set;set.id="r1-1";set.roundLabel="Final";set.round=1;set.players[0]="Kuro";set.players[1]="Velvet";set.state="needs_review";
 set.matchId="emt_1";set.revision="4";set.games={{"att_1","","review"}};
 id.deskEvent="t9";id.deskSets={set};settle();
 Check(!h.row("dk-edit")&&h.row("dk-set:r1-1"),"A live tournament's sets are not shown, or its settings still change");
 h.Choose("dk-set:r1-1");
 Check(h.until(IdentityOp::DeskRead)&&sent()->target=="t9"&&sent()->command=="r1-1","The set's games were not read");
 settle();
 h.Choose("dk-hold");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent()->target=="/tournaments/t9/sets/r1-1/hold"&&sent()->command==R"({"held":true})","Hold was not sent");
 settle();
 h.Choose("dk-draw:att_1");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 const auto decided=nlohmann::json::parse(sent()->command);
 Check(sent()->target=="/tournaments/t9/sets/r1-1/adjudications"&&decided.value("kind","")=="game_result"&&decided.value("draw",false)&&
  decided.value("attempt_id","")=="att_1"&&decided.value("expected_revision","")=="4","A draw was not sent for the game");
 settle();
 // A draft is deleted from its setup.
 netplay::DeskEvent other;other.id="t10";other.title="Friday";other.status="draft";id.deskEvents={other};
 h.Screen("tournament-desk");settle();
 h.Choose("dk-event:t10");settle();
 h.Choose("dk-d-delete");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(sent()->op==IdentityOp::DeskStop&&sent()->target=="t10","Deleting the draft was not asked for");
 Check(h.shell.Navigation().Screen()=="tournament-desk","Deleting the draft did not go back to the tournaments");
}
}

void RunSignInSiteJourneys(){
 DiscordKeepsItsWait();DiscordGoEndsItsWait();LinkForTheWaitedService();DiscordChangeNeedsSignIn();ConnectedAgain();DeskInGame();
}
