// The Connect Challonge journeys: the Ember ID screen's Challonge row, which
// shows only where Ember's own service keeps Challonge accounts, and the
// Connect Challonge screen, which opens Challonge's page through the helper,
// reads the account until it appears, and disconnects it. Run from the
// identity journey test's main.
#include "identity_journey_support.hxx"
namespace {
const std::string ember="https://bridge.embernetplay.link";

// A ready Ember ID that trusts Ember's own service.
void Ready(Journey& h){
 auto& id=h.view.identity;id.known=true;h.ready();id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
}
// Answers until the profile of Ember's own service is asked for, then gives
// it the Challonge features named.
void Profile(Journey& h,bool offered,bool kept){
 using sf4e::netplay::IdentityOp;auto& id=h.view.identity;
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Ember's own service was not read");
 id.inspected=id.bridges[0];id.inspectedChallonge=offered;id.inspectedChallongeAccounts=kept;h.answer();
}

// Without Challonge accounts on Ember's own service the Ember ID screen has no
// Challonge row; with them it reads the account and shows it.
void ChallongeRow(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);
 h.Screen("identity");Profile(h,false,false);for(int i=0;i<4;++i)h.answer();
 Check(h.row("discord-connect")&&!h.row(ChallongeConnectScreen),"The Ember ID screen offers Challonge on a service without it");
 Check(h.count(IdentityOp::ChallongeStatus)==0,"A Challonge account was read from a service without Challonge");
 h.Screen("home");h.Screen("identity");Profile(h,false,true);
 Check(h.sent().back()->op==IdentityOp::ChallongeStatus&&h.sent().back()->bridge=="brg_1","The Challonge account was not read");
 h.view.identity.challongeUser="1001";h.view.identity.challongeName="kate";h.answer();
 const auto* row=h.row(ChallongeConnectScreen);
 Check(row&&row->value=="kate"&&h.index(ChallongeConnectScreen)==h.index("discord-connect")+1,"The Ember ID screen does not show the Challonge account after Discord");
 h.Choose(ChallongeConnectScreen);
 Check(h.trail()=="home>identity>challonge-connect","The Challonge row did not open Connect Challonge");
}

// Connect opens Challonge's page through the helper; the account is read
// every few seconds until it appears, and Disconnect asks first.
void ChallongeConnects(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 h.Screen(ChallongeConnectScreen);Profile(h,true,true);
 Check(h.sent().back()->op==IdentityOp::ChallongeStatus,"Connect Challonge did not read the account");
 id.challongeUser.clear();id.challongeName.clear();h.answer();
 Check(h.row("ch-connect")&&h.row("ch-connect")->enabled&&!h.row("ch-cancel"),"Connect Challonge is not offered");
 h.Choose("ch-connect");
 Check(h.sent().back()->op==IdentityOp::ChallongeConnect&&h.sent().back()->bridge=="brg_1","Connect did not ask the helper to open Challonge");
 Check(h.row("ch-progress")&&h.row("ch-cancel"),"Opening Challonge is not shown with Cancel");
 h.answer();
 Check(h.status==loc::T("challonge.opened")&&h.row("ch-waiting")&&h.row("ch-cancel"),"Waiting for Challonge is not shown with Cancel");
 const auto polled=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()>polled&&h.sent().back()->op==IdentityOp::ChallongeStatus&&h.sent().back()->bridge=="brg_1","The account was not read again while waiting");
 h.answer();h.Frame(0,1900);
 Check(h.status==loc::T("challonge.waiting"),"The waiting line is not shown once the first notice ends");
 id.challongeUser="1001";id.challongeName="kate";h.until(IdentityOp::ChallongeStatus);h.answer();
 Check(h.status==loc::Tf("challonge.done","kate")&&h.row("ch-connected")&&h.row("ch-connected")->value=="kate","A connected account was not shown");
 Check(h.row("ch-connect")&&h.row("ch-remove")&&!h.row("ch-cancel"),"A connected account offers no other account or Disconnect");
 const auto settled=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()==settled,"The account was still read again after it connected");
 // Disconnect asks first.
 h.Choose("ch-remove");
 Check(h.sent().size()==settled,"Disconnect sent without asking");
 h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.sent().back()->op==IdentityOp::ChallongeRemove&&h.sent().back()->bridge=="brg_1","Disconnect did not send");
 id.challongeUser.clear();id.challongeName.clear();h.answer();
 Check(h.status==loc::T("challonge.removed")&&h.row("ch-connect")&&!h.row("ch-connected"),"A disconnected account is still shown");
}

// Cancel ends the sign-in on the service, and so does leaving the screen
// while Challonge's page is still being opened; a sign-in on its way stays.
void ChallongeStops(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 h.Screen(ChallongeConnectScreen);Profile(h,true,true);h.answer();
 h.Choose("ch-connect");h.answer();
 h.Choose("ch-cancel");
 Check(h.status==loc::T("challonge.cancelled")&&!h.row("ch-waiting")&&!h.row("ch-cancel"),"Cancel did not stop waiting");
 h.Frame(0,2);
 Check(h.sent().back()->op==IdentityOp::ChallongeCancel&&h.sent().back()->bridge=="brg_1","Cancel did not end the sign-in on the service");
 h.answer();
 const auto stopped=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()==stopped,"A cancelled sign-in was still waited for");
 // Leaving while the page is being opened ends it there too.
 h.Choose("ch-connect");
 Check(h.sent().back()->op==IdentityOp::ChallongeConnect,"Connect did not send");
 h.Screen("identity");h.answer();
 Check(h.until(IdentityOp::ChallongeCancel)&&h.sent().back()->bridge=="brg_1","Leaving while Challonge opened did not end the sign-in");
 // One already waiting goes on, as Connect Discord's does, and is read again on its screen.
 h.Screen(ChallongeConnectScreen);for(int i=0;i<6;++i)h.answer();
 h.Choose("ch-connect");h.answer();
 const auto cancels=h.count(IdentityOp::ChallongeCancel);
 h.Screen("identity");for(int i=0;i<6;++i)h.answer();
 Check(h.count(IdentityOp::ChallongeCancel)==cancels,"Leaving a sign-in that waits for the browser ended it");
 h.Screen(ChallongeConnectScreen);for(int i=0;i<6;++i)h.answer();
 Check(h.row("ch-waiting"),"Coming back no longer showed the sign-in waiting");
 id.challongeUser="1001";id.challongeName="kate";h.Frame(0,300);
 Check(h.until(IdentityOp::ChallongeStatus),"The waiting sign-in was not read again");
 h.answer();
 Check(h.status==loc::Tf("challonge.done","kate"),"The waiting sign-in did not finish");
}

// Sign-in off with an account kept: it can be disconnected, none connected.
// A failed read offers Try again.
void ChallongeOffAndRetry(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 h.Screen(ChallongeConnectScreen);Profile(h,false,true);
 id.challongeUser.clear();id.challongeName.clear();h.answer();
 Check(h.row("ch-off")&&!h.row("ch-connect"),"Connect Challonge is offered where sign-in is off");
 id.challongeUser="1001";id.challongeName="kate";
 h.Screen("home");h.Screen(ChallongeConnectScreen);Profile(h,false,true);h.answer();
 Check(h.row("ch-connected")&&h.row("ch-remove"),"An account kept without sign-in cannot be disconnected");
 h.Screen("home");h.Screen(ChallongeConnectScreen);Profile(h,true,true);
 Check(h.sent().back()->op==IdentityOp::ChallongeStatus,"The account was not read");
 h.answer(false,"service_unavailable");
 Check(h.row("ch-retry")&&h.row("ch-connect")&&!h.row("ch-connect")->enabled,"A failed read does not offer Try again");
 h.Choose("ch-retry");Profile(h,true,true);h.answer();
 Check(h.row("ch-connected")&&!h.row("ch-retry"),"Try again did not read the account");
 // Without Ember's own service there is nothing to connect.
 id.bridges.clear();h.Screen("home");h.Screen(ChallongeConnectScreen);for(int i=0;i<4;++i)h.answer();
 Check(h.row("ch-no-service")&&h.row("linked-accounts")&&!h.row("ch-connect")&&!h.row("ch-off"),"Without Ember's own service the screen does not say to set it up");
}
// On a service that offers both, Discord's requests and rows come first and
// as before: the Ember ID screen reads Discord's account, then Challonge's,
// and a tournament site's connect link (BluMint's one-link onboarding) asks
// nothing of Challonge on its way to Discord's page.
void DiscordFirst(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 const auto both=[&]{id.inspected=id.bridges[0];id.inspectedDiscord=id.inspectedDiscordAccounts=id.inspectedChallonge=id.inspectedChallongeAccounts=true;};
 h.Screen("identity");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"The Ember ID screen did not read Ember's service");
 both();h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.count(IdentityOp::ChallongeStatus)==0,"Discord's account was not read first");
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(h.row("discord-connect")&&h.row("discord-connect")->value=="kate","Discord's account waited for Challonge's");
 Check(h.sent().back()->op==IdentityOp::ChallongeStatus,"Challonge's account was not read after Discord's");
 id.challongeUser="1001";id.challongeName="Kate_99";h.answer();
 Check(h.row(ChallongeConnectScreen)&&h.row(ChallongeConnectScreen)->value=="Kate_99","The Challonge account is not shown");
 // A connect link runs as it always has.
 id.discordUser.clear();id.discordName.clear();
 const auto challongeReads=h.count(IdentityOp::ChallongeStatus);
 h.Screen("home");h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
 Check(h.until(IdentityOp::BridgeInspect),"The connect link did not read its service");
 both();h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_1","The connect link did not read Discord's account");
 h.answer();
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_1","The connect link did not open Discord");
 Check(h.count(IdentityOp::ChallongeStatus)==challongeReads,"The connect link asked about Challonge");
}

// Ember's own service offering both sites, through the frames that run
// while Ember is hidden.
void Both(Journey& h){
 auto& id=h.view.identity;id.inspected=id.bridges[0];
 id.inspectedDiscord=id.inspectedDiscordAccounts=id.inspectedChallonge=id.inspectedChallongeAccounts=true;
}
void Hidden(Journey& h){
 ImGui::GetIO().DeltaTime=1.f/60;ImGui::NewFrame();
 h.shell.Background(h.view,h.view.room,[&](auto a){h.actions.push_back(a);return h.accept;});ImGui::Render();
}
// The newest request is answered as Ember stays hidden.
void AnswerHidden(Journey& h){
 auto& id=h.view.identity;
 h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();
 Hidden(h);Hidden(h);
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
// A Challonge sign-in waiting for the browser.
void ChallongeWaiting(Journey& h){
 using sf4e::netplay::IdentityOp;auto& id=h.view.identity;
 h.Screen(ChallongeConnectScreen);Profile(h,true,true);
 id.challongeUser.clear();id.challongeName.clear();h.answer();
 h.Choose("ch-connect");
 Check(h.sent().back()->op==IdentityOp::ChallongeConnect,"Connect did not ask the helper to open Challonge");
 h.answer();
 Check(h.row("ch-waiting")!=nullptr,"Challonge is not waited for");
}

// One sign-in at a time: starting one site's ends the other's on its service
// first, and the new one shows as being set up, not as waiting.
void OneSignInAtATime(){
 using namespace sf4e;using netplay::IdentityOp;
 {
  Journey h;Ready(h);h.untilLimit=12;
  DiscordWaiting(h);
  h.Screen(ChallongeConnectScreen);Profile(h,true,true);h.answer();
  const auto before=h.sent().size();
  h.Choose("ch-connect");
  Check(h.sent().size()==before+1&&h.sent().back()->op==IdentityOp::DiscordCancel&&h.sent().back()->bridge=="brg_1",
   "Connect Challonge did not end Discord's sign-in first");
  Check(h.row("ch-progress")&&!h.row("ch-waiting"),"Challonge showed as waiting before its page opened");
  h.answer();
  Check(h.sent().back()->op==IdentityOp::ChallongeConnect,"Challonge was not opened after Discord's sign-in ended");
 }
 {
  Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
  ChallongeWaiting(h);
  h.Screen("linked-accounts");
  Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not read the service");
  Both(h);h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"Linked accounts did not read Discord's account");
  id.discordUser.clear();id.discordName.clear();h.answer();for(int i=0;i<4;++i)h.answer();
  const auto before=h.sent().size();
  h.Choose("id-discord-connect");
  Check(h.sent().size()==before+1&&h.sent().back()->op==IdentityOp::ChallongeCancel&&h.sent().back()->bridge=="brg_1",
   "Connecting Discord did not end Challonge's sign-in first");
  h.answer();
  Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Discord was not opened after Challonge's sign-in ended");
 }
 {
  // Hiding Ember before Challonge's page was asked for sends nothing to
  // Challonge: only Discord's sign-in ends.
  Journey h;Ready(h);h.untilLimit=12;
  DiscordWaiting(h);
  h.Screen(ChallongeConnectScreen);Profile(h,true,true);h.answer();
  h.Choose("ch-connect");
  Check(h.sent().back()->op==IdentityOp::DiscordCancel,"Connect Challonge did not end Discord's sign-in first");
  Hidden(h);AnswerHidden(h);
  Check(h.count(IdentityOp::ChallongeConnect)==0&&h.count(IdentityOp::ChallongeCancel)==0,"Hiding Ember still opened Challonge");
 }
 {
  // A sign-in asked of the helper and then hidden still ends on the service.
  Journey h;Ready(h);
  h.Screen(ChallongeConnectScreen);Profile(h,true,true);h.answer();
  h.Choose("ch-connect");
  Check(h.sent().back()->op==IdentityOp::ChallongeConnect,"Connect did not ask the helper to open Challonge");
  Hidden(h);AnswerHidden(h);
  Check(h.sent().back()->op==IdentityOp::ChallongeCancel&&h.sent().back()->bridge=="brg_1","Hiding Ember lost Challonge's cancellation");
  Check(h.count(IdentityOp::ChallongeCancel)==1,"Challonge's cancellation was sent more than once");
 }
 {
  // A tournament site's link while Challonge's page is being opened: the
  // Challonge sign-in ends on the service, and the link runs as always.
  Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
  h.Screen(ChallongeConnectScreen);Profile(h,true,true);h.answer();
  h.Choose("ch-connect");
  Check(h.sent().back()->op==IdentityOp::ChallongeConnect,"Connect did not ask the helper to open Challonge");
  h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
  h.answer();
  Check(h.sent().back()->op==IdentityOp::ChallongeCancel&&h.sent().back()->bridge=="brg_1","A connect link left Challonge's sign-in open");
  Check(h.until(IdentityOp::BridgeInspect),"The connect link did not read its service");
  Both(h);h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"The connect link did not read Discord's account");
  id.discordUser.clear();id.discordName.clear();h.answer();
  Check(h.sent().back()->op==IdentityOp::DiscordConnect,"The connect link did not open Discord");
  Check(h.count(IdentityOp::ChallongeConnect)==1,"Challonge was opened again");
 }
}

// Discord's change of account, a new page for the same site on the same
// service, keeps the wait already running as it always has: Connect Discord
// shows it waiting, not setting up, and even when the new page fails to
// open, the account is still read every few seconds. Nothing is cancelled.
void DiscordKeepsItsWait(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 DiscordWaiting(h);
 h.Screen("linked-accounts");
 Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not read the service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;id.inspectedChallonge=id.inspectedChallongeAccounts=false;h.answer();
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

// A failed Challonge read says nothing off its own screen: Discord-only
// players on the Ember ID screen see Discord as before, and Settings >
// Discord reads only Discord's account.
void ChallongeReadsQuietly(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 h.Screen("identity");
 Check(h.until(IdentityOp::BridgeInspect),"The Ember ID screen did not read Ember's service");
 Both(h);h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"The Ember ID screen did not read Discord's account");
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(h.sent().back()->op==IdentityOp::ChallongeStatus,"The Ember ID screen did not read Challonge's account");
 h.answer(false,"service_unavailable");h.Frame(0,2);
 Check(h.status!=loc::T("identity.failure.unreachable"),"A failed Challonge read was said on the Ember ID screen");
 Check(h.row("discord-connect")&&h.row("discord-connect")->value=="kate","A failed Challonge read changed Discord's row");
 Check(h.row(ChallongeConnectScreen)&&h.row(ChallongeConnectScreen)->value.empty(),"A failed Challonge read still showed an account");
 // On its own screen it is said, with Try again.
 h.Screen(ChallongeConnectScreen);Profile(h,true,true);h.answer(false,"service_unavailable");
 Check(h.status==loc::T("identity.failure.unreachable")&&h.row("ch-retry"),"A failed read on Connect Challonge was not said");
 // Settings > Discord reads Discord's account only.
 const auto challongeReads=h.count(IdentityOp::ChallongeStatus);
 h.Screen("discord");
 Check(h.until(IdentityOp::BridgeInspect),"Settings > Discord did not read the service");
 Both(h);h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"Settings > Discord did not read Discord's account");
 h.answer();for(int i=0;i<4;++i)h.answer();
 Check(h.count(IdentityOp::ChallongeStatus)==challongeReads,"Settings > Discord read Challonge's account");
}
}
void RunChallongeConnectJourneys(){
 DiscordFirst();OneSignInAtATime();DiscordKeepsItsWait();ChallongeReadsQuietly();
 ChallongeRow();ChallongeConnects();ChallongeStops();ChallongeOffAndRetry();
}
