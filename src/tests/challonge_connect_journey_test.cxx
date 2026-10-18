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
 Check(h.row("discord-connect")&&!h.row(IdentityPanel::ChallongeScreen),"The Ember ID screen offers Challonge on a service without it");
 Check(h.count(IdentityOp::ChallongeStatus)==0,"A Challonge account was read from a service without Challonge");
 h.Screen("home");h.Screen("identity");Profile(h,false,true);
 Check(h.sent().back()->op==IdentityOp::ChallongeStatus&&h.sent().back()->bridge=="brg_1","The Challonge account was not read");
 h.view.identity.challongeUser="1001";h.view.identity.challongeName="kate";h.answer();
 const auto* row=h.row(IdentityPanel::ChallongeScreen);
 Check(row&&row->value=="kate"&&h.index(IdentityPanel::ChallongeScreen)==h.index("discord-connect")+1,"The Ember ID screen does not show the Challonge account after Discord");
 h.Choose(IdentityPanel::ChallongeScreen);
 Check(h.trail()=="home>identity>challonge-connect","The Challonge row did not open Connect Challonge");
}

// Connect opens Challonge's page through the helper; the account is read
// every few seconds until it appears, and Disconnect asks first.
void ChallongeConnects(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 h.Screen(IdentityPanel::ChallongeScreen);Profile(h,true,true);
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
 h.Screen(IdentityPanel::ChallongeScreen);Profile(h,true,true);h.answer();
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
 h.Screen(IdentityPanel::ChallongeScreen);for(int i=0;i<6;++i)h.answer();
 h.Choose("ch-connect");h.answer();
 const auto cancels=h.count(IdentityOp::ChallongeCancel);
 h.Screen("identity");for(int i=0;i<6;++i)h.answer();
 Check(h.count(IdentityOp::ChallongeCancel)==cancels,"Leaving a sign-in that waits for the browser ended it");
 h.Screen(IdentityPanel::ChallongeScreen);for(int i=0;i<6;++i)h.answer();
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
 h.Screen(IdentityPanel::ChallongeScreen);Profile(h,false,true);
 id.challongeUser.clear();id.challongeName.clear();h.answer();
 Check(h.row("ch-off")&&!h.row("ch-connect"),"Connect Challonge is offered where sign-in is off");
 id.challongeUser="1001";id.challongeName="kate";
 h.Screen("home");h.Screen(IdentityPanel::ChallongeScreen);Profile(h,false,true);h.answer();
 Check(h.row("ch-connected")&&h.row("ch-remove"),"An account kept without sign-in cannot be disconnected");
 h.Screen("home");h.Screen(IdentityPanel::ChallongeScreen);Profile(h,true,true);
 Check(h.sent().back()->op==IdentityOp::ChallongeStatus,"The account was not read");
 h.answer(false,"service_unavailable");
 Check(h.row("ch-retry")&&h.row("ch-connect")&&!h.row("ch-connect")->enabled,"A failed read does not offer Try again");
 h.Choose("ch-retry");Profile(h,true,true);h.answer();
 Check(h.row("ch-connected")&&!h.row("ch-retry"),"Try again did not read the account");
 // Without Ember's own service there is nothing to connect.
 id.bridges.clear();h.Screen("home");h.Screen(IdentityPanel::ChallongeScreen);for(int i=0;i<4;++i)h.answer();
 Check(h.row("ch-off")&&!h.row("ch-connect"),"Connect Challonge is offered without Ember's own service");
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
 Check(h.row(IdentityPanel::ChallongeScreen)&&h.row(IdentityPanel::ChallongeScreen)->value=="Kate_99","The Challonge account is not shown");
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
}
void RunChallongeConnectJourneys(){
 DiscordFirst();
 ChallongeRow();ChallongeConnects();ChallongeStops();ChallongeOffAndRetry();
}
