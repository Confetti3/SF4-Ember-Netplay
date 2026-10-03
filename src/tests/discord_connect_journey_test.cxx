// The Connect Discord and onboarding journeys: Discord on a service that offers
// it, a tournament site's connect link, the wait for a sign-in, and the steps a
// brand-new player's link takes. Run from the identity journey test's main.
#include "identity_journey_support.hxx"
namespace {
// Discord on a service that offers it: optional, connected from the Linked
// accounts screen through the browser, read back with Refresh, disconnected
// with a confirmation. A brand-new player's match link asks for an Ember ID
// first, not for a service.
void DiscordAndFirstLink(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;id.state="disabled";h.Frame();
 // No Ember ID yet: the link opens the matches screen, which says to create one.
 h.view.tournament.link.bridge="brg_1";h.view.tournament.link.match="emt_1";h.view.tournament.link.sequence=1;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="tournament-matches"&&h.row("id-linked-unavailable"),"A new player's match link did not ask for an Ember ID");
 for(int i=0;i<6;++i)h.answer();
 Check(h.status!=loc::T("tournament.failure.link_service"),"A new player's match link asked to trust a service");
 // With an ID, the address field starts on Ember's own service.
 h.ready();
 h.Screen("linked-accounts");for(int i=0;i<4;++i)h.answer();
 Check(h.row("id-origin")&&h.row("id-origin")->value=="https://bridge.embernetplay.link","The service address does not start on Ember's own service");
 // A service with Discord: its status is asked for, and Connect opens the browser through the helper.
 id.bridges={{"brg_1","https://bridge.embernetplay.link","Ember"}};h.Screen("home");h.Screen("linked-accounts");
 for(int i=0;i<8&&h.sent().back()->op!=IdentityOp::BridgeInspect;++i)h.answer();
 id.inspected=id.bridges[0];id.connections={{"blumint","BluMint"}};id.inspectedDiscord=true;h.answer();
 for(int i=0;i<4&&h.sent().back()->op!=IdentityOp::DiscordStatus;++i)h.answer();
 Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_1","A Discord service's account was not asked for");
 id.discordUser.clear();id.discordName.clear();h.answer();
 Check(h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled&&h.row("id-discord-connect")->value==loc::T("identity.discord_none"),
  "An unconnected Discord account is not offered");
 h.Choose("id-discord-connect");
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_1","Connect Discord did not name its service");
 h.answer();Check(h.status==loc::T("identity.done.discord_opened"),"Connect Discord did not say to finish in the browser");
 // Refresh reads the account back; a connected one is disconnected after a confirmation.
 h.Choose("id-refresh");
 for(int i=0;i<10&&h.sent().back()->op!=IdentityOp::DiscordStatus;++i)h.answer();
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(h.row("id-discord-remove")&&h.row("id-discord-remove")->value=="kate","A connected Discord account is not shown");
 h.Choose("id-discord-remove");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.sent().back()->op==IdentityOp::DiscordRemove,"Disconnect did not send");
 id.discordUser.clear();id.discordName.clear();h.answer();
 Check(h.status==loc::T("identity.done.discord_removed")&&h.row("id-discord-connect"),"A disconnected account is still shown");
 // A service without Discord shows no row.
 id.inspectedDiscord=false;h.Choose("id-refresh");for(int i=0;i<10;++i)h.answer();
 Check(!h.row("id-discord-connect")&&!h.row("id-discord-remove"),"A service without Discord offers it");
 // Sign-in off now, but an account connected earlier: it can still be disconnected, and none can be connected.
 id.inspectedDiscordAccounts=true;h.Choose("id-refresh");
 for(int i=0;i<10&&h.sent().back()->op!=IdentityOp::DiscordStatus;++i)h.answer();
 Check(h.sent().back()->op==IdentityOp::DiscordStatus,"A service keeping Discord accounts was not asked for one");
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(h.row("id-discord-remove")&&!h.row("id-discord-connect"),"An account kept without sign-in cannot be disconnected");
 id.discordUser.clear();id.discordName.clear();h.Choose("id-refresh");for(int i=0;i<10;++i)h.answer();
 Check(!h.row("id-discord-connect")&&!h.row("id-discord-remove"),"A service without sign-in offers to connect Discord");
}
// A tournament site's connect link onboards a brand-new player by itself:
// the link they just clicked creates the Ember ID, trusts Ember's own
// service and opens Discord's page, so Discord's Authorize is the only
// press; the screen then reads the account until it is connected. A link
// that waited for a room asks first. A connected account is only shown, and
// a link for a service Ember does not know trusts nothing.
void DiscordConnectLink(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;id.state="disabled";h.Frame();
 h.untilLimit=12;
 const std::string ember="https://bridge.embernetplay.link";
 // In a room the link only says it will open later, and then asks first.
 h.view.session.room=netplay::RoomState::Joined;h.Screen("home");
 h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
 Check(h.status==loc::T("connect.link_waiting")&&h.shell.Navigation().Screen()!="discord-connect","A connect link moved a player who is in a room");
 const auto waited=h.sent().size();
 h.view.session.room=netplay::RoomState::Idle;h.Frame(0,2);
 Check(h.shell.Navigation().Screen()=="discord-connect","A connect link did not open Connect Discord once the room closed");
 for(int i=0;i<4;++i)h.answer();
 Check(h.row("dc-go")&&h.row("dc-go")->enabled&&h.count(IdentityOp::Enable,waited)==0,"A link that waited for a room did not ask first");
 // A link the player just clicked runs by itself up to Discord's page.
 h.Screen("home");const auto start=h.sent().size();
 h.view.tournament.connect.sequence=2;h.Frame(0,2);
 Check(h.shell.Navigation().Screen()=="discord-connect"&&h.shell.Navigation().Parent()=="identity","A connect link did not open Connect Discord under Ember ID");
 Check(h.until(IdentityOp::Enable),"The link did not create the Ember ID by itself");
 Check(h.row("dc-progress")!=nullptr,"Connect Discord does not show it is setting up");
 h.ready();h.answer();
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"The link did not look up Ember's own service");
 id.inspected={"brg_1",ember,"Ember"};id.inspectedDiscord=true;h.answer();
 Check(h.sent().back()->op==IdentityOp::BridgeApprove&&h.sent().back()->bridge=="brg_1"&&h.sent().back()->origin==ember,"The link did not trust Ember's own service by itself");
 id.bridges={{"brg_1",ember,"Ember"}};h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_1","The link did not read the account first");
 id.discordUser.clear();id.discordName.clear();h.answer();
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_1","The link did not open Discord by itself");
 Check(h.count(IdentityOp::LinkList,start)==0,"Connect Discord asked for the linked accounts");
 h.answer();Check(h.status==loc::T("identity.done.discord_opened"),"Opening Discord did not say to finish in the browser");
 Check(h.row("dc-waiting")&&h.row("dc-cancel"),"Waiting for Discord is not shown with Cancel");
 // While the browser is open the account is read again by itself.
 const auto polled=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()>polled&&h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_1","The account was not read again");
 h.answer();h.Frame(0,1900);
 Check(h.status==loc::T("connect.waiting"),"Waiting for Discord is not said once the first notice ends");
 id.discordUser="274220342558756145";id.discordName="kate";h.until(IdentityOp::DiscordStatus);h.answer();
 Check(h.status==loc::Tf("connect.done","kate")&&h.row("dc-connected")&&h.row("dc-connected")->value=="kate","A connected account was not shown");
 Check(h.row("id-discord-connect")&&h.row("id-discord-remove"),"A connected account offers no other account or Unlink");
 const auto settled=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()==settled,"The account was still read again after it connected");
 // A second link for a connected account only shows it.
 h.Screen("home");h.view.tournament.connect.sequence=3;h.Frame(0,2);
 Check(h.shell.Navigation().Screen()=="discord-connect","A second connect link did not open Connect Discord");
 for(int i=0;i<8;++i)h.answer();
 Check(h.row("dc-connected")&&h.count(IdentityOp::DiscordConnect,start)==1,"A link for a connected account opened Discord again");
 // A link for a service Ember does not know says so; nothing is trusted by itself.
 const auto before=h.sent().size();
 h.view.tournament.connect.bridge="brg_9";h.view.tournament.connect.sequence=4;h.Frame(0,2);
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"A connect link for another service did not check Ember's own");
 id.inspected={"brg_1",ember,"Ember"};h.answer();for(int i=0;i<6;++i)h.answer();
 Check(h.row("dc-unknown")&&h.row("dc-retry")&&!h.row("id-approve")&&!h.row("id-discord-connect"),"A link for an unknown service was not said");
 Check(h.count(IdentityOp::BridgeApprove,before)==0,"A connect link trusted another service by itself");
 // The Ember ID screen opens Connect Discord too.
 h.Screen("identity");for(int i=0;i<4;++i)h.answer();
 Check(h.row("discord-connect")!=nullptr,"The Ember ID screen does not offer Connect Discord");
}
// The wait for a Discord sign-in belongs to the service it was opened for:
// another service that already has an account does not end it, its polls do
// not change what another selected service shows, and a new connect link
// stops waiting for the old one. An old Connect's late answer, success or
// failure, changes nothing in the new visit.
void DiscordWaitsForItsService(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;id.state="ready";h.view.tournament.connect.confirm=true;
 h.identify();
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
 const auto open=[&](const char* bridge,std::uint64_t sequence,const netplay::IdentityBridge& profile){
  h.view.tournament.connect.bridge=bridge;h.view.tournament.connect.sequence=sequence;h.Frame(0,2);
  Check(h.until(IdentityOp::BridgeInspect),"Connect Discord did not inspect its service");
  id.inspected=profile;id.inspectedDiscord=true;h.answer();
  Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge==bridge,"Connect Discord did not read its service's account");
 };
 // A sign-in opened on Ember's service.
 open("brg_1",1,id.bridges[0]);id.discordUser.clear();id.discordName.clear();h.answer();
 h.Choose("id-discord-connect");Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_1","Connect did not name its service");
 // Its answer arrives only after a second link, for a service that already
 // has an account, started a new visit.
 const auto connect=h.sent().back()->ticket;
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=2;h.Frame(0,2);
 h.view.identityTicket=connect;h.view.identityRequest=id.requestId=connect+100;id.ok=true;h.Frame(0,2);
 Check(h.status!=loc::T("identity.done.discord_opened"),"A replaced visit's Connect answer changed the status");
 Check(h.until(IdentityOp::BridgeInspect),"The second link did not inspect its service");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","The second link did not read its service's account");
 id.discordUser="111";id.discordName="sam";h.answer();
 Check(h.status!=loc::Tf("connect.done","sam"),"Another service's account ended the sign-in's wait");
 // Nothing is read again for the first service's sign-in.
 const auto before=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()==before,"A sign-in from an earlier visit was still waited for");
 // In one visit, polls go to the sign-in's service and only its account ends the wait.
 open("brg_1",3,id.bridges[0]);id.discordUser.clear();id.discordName.clear();h.answer();
 h.Choose("id-discord-connect");h.answer();
 h.Frame(0,300);
 Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_1","The poll did not read the sign-in's service");
 id.discordUser="222";id.discordName="kate";h.answer();
 Check(h.status==loc::Tf("connect.done","kate"),"The sign-in's own account did not end the wait");
 // A failed late answer does not reach the new visit either.
 id.discordUser.clear();id.discordName.clear();
 open("brg_1",4,id.bridges[0]);h.answer();
 h.Choose("id-discord-connect");const auto failing=h.sent().back()->ticket;
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=5;h.Frame(0,2);
 h.view.identityTicket=failing;h.view.identityRequest=id.requestId=failing+100;id.ok=false;id.failure="rate_limited";h.Frame(0,2);
 Check(h.status!=loc::T("identity.failure.rate_limited"),"A replaced visit's failed Connect changed the status");
 Check(h.until(IdentityOp::BridgeInspect),"A replaced visit's failed Connect dropped the new visit's requests");
 // Under Linked accounts, the selected service keeps showing its own
 // account while the sign-in on Ember's service is polled.
 open("brg_1",6,id.bridges[0]);id.discordUser.clear();id.discordName.clear();h.answer();
 h.Choose("id-discord-connect");h.answer();
 h.Screen("linked-accounts");for(int i=0;i<8;++i)h.answer();
 h.FocusOn("id-bridge");h.Press(MenuInput::Select);h.Press(MenuInput::Down);h.Press(MenuInput::Select);
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"Selecting the other service did not inspect it");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","The other service's account was not read");
 id.discordUser="111";id.discordName="sam";h.answer();for(int i=0;i<4;++i)h.answer();
 Check(h.row("id-discord-remove")&&h.row("id-discord-remove")->value=="sam","The other service's account is not shown");
 id.discordUser.clear();id.discordName.clear();
 h.Frame(0,300);
 Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_1","The sign-in's service was not polled");
 h.answer();
 Check(h.row("id-discord-remove")&&h.row("id-discord-remove")->value=="sam","A poll of another service replaced the shown account");
 // A poll from a replaced journey that fails, or never answers, does not
 // reach the new journey either.
 h.Frame(0,300);
 Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_1","No poll was in flight");
 const auto poll=h.sent().back()->ticket;
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=7;h.Frame(0,2);
 h.view.identityTicket=poll;h.view.identityRequest=id.requestId=poll+100;id.ok=false;id.failure="rate_limited";h.Frame(0,2);
 Check(h.status!=loc::T("identity.failure.rate_limited"),"A replaced journey's failed poll changed the status");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"A replaced journey's failed poll dropped the new journey's requests");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"The new journey did not read its account");
 id.discordUser.clear();id.discordName.clear();h.answer();
 h.Choose("id-discord-connect");h.answer();h.Frame(0,300);
 Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_2","No poll was in flight on the new journey");
 h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=8;h.Frame(0,7300);
 Check(h.status!=loc::T("identity.failure.timeout"),"A replaced journey's unanswered poll timed out on the new journey");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"A replaced journey's unanswered poll dropped the new journey's requests");
}
// A link's journey keeps its service through a locked Ember ID, unlocked on
// Connect Discord itself, and through a visit to the Ember ID screen.
void DiscordConnectKeepsItsService(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;id.state="locked";h.Frame();h.view.tournament.connect.confirm=true;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 // A link for a trusted service other than Ember's own, with the ID locked.
 h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
 for(int i=0;i<4;++i)h.answer();
 Check(h.shell.Navigation().Screen()=="discord-connect"&&h.row("id-unlock"),"A locked ID is not unlocked on Connect Discord");
 h.Choose("id-unlock");h.type("correct horse");
 Check(h.sent().back()->op==IdentityOp::Unlock&&h.sent().back()->passphrase=="correct horse","Unlock did not send the passphrase");
 h.ready();
 id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.answer();
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"Unlocking did not go on to the link's service");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","The link's service's account was not read");
 id.discordUser.clear();id.discordName.clear();h.answer();
 // A visit to the Ember ID screen comes back to the same journey.
 id.state="recovery_required";h.Frame(0,2);
 Check(h.row("identity")!=nullptr,"An ID needing recovery does not lead to the Ember ID screen");
 h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Connect Discord did not open the Ember ID screen");
 id.state="ready";for(int i=0;i<4;++i)h.answer();
 h.Choose("discord-connect");
 Check(h.shell.Navigation().Screen()=="discord-connect"&&h.shell.Navigation().Parent()=="identity","Connect Discord from the Ember ID screen did not go back to the journey");
 for(int i=0;i<8;++i)h.answer();
 Check(h.row("dc-service")&&h.row("dc-service")->value=="Other","The journey lost the link's service");
 h.Choose("id-discord-connect");
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_2","Connect did not name the link's service");
}
// Where the browser cannot open Ember, the page's link pasted into Connect
// Discord, opened from the menu, starts the journey for the site's service,
// not Ember's own. Something else pasted says so and changes nothing.
std::string g_pasted;
void DiscordConnectPastedLink(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;id.state="ready";
 h.identify();
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 const std::string site="brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
 id.bridges={{"brg_1",ember,"Ember"},{site,other,"Other"}};h.Frame();
 auto& clipboard=ImGui::GetPlatformIO();
 clipboard.Platform_GetClipboardTextFn=[](ImGuiContext*){return g_pasted.c_str();};
 // From the menu, Connect Discord is for Ember's own service.
 h.Screen("identity");for(int i=0;i<4;++i)h.answer();
 h.Choose("discord-connect");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Connect Discord from the menu is not for Ember's own service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();for(int i=0;i<4;++i)h.answer();
 Check(h.row("dc-paste")!=nullptr,"Connect Discord does not offer to paste the page's link");
 // Something else pasted says so.
 g_pasted="hello";const auto before=h.sent().size();h.Choose("dc-paste");h.Frame();
 Check(h.status==loc::T("connect.paste_failed")&&h.sent().size()==before,"Pasting something else did not say so");
 // The page's link starts the journey for the site's service.
 g_pasted="https://embernetplay.link/start#"+site;h.Choose("dc-paste");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"A pasted page link did not start the journey for its service");
 id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge==site,"The site's service's account was not read");
 id.discordUser.clear();id.discordName.clear();h.answer();
 // Pasting is the player's press: Discord opens by itself, for the site's service.
 Check(h.row("dc-service")&&h.row("dc-service")->value=="Other","Connect Discord does not show the site's service");
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge==site,"A pasted link did not open Discord for the site's service");
 clipboard.Platform_GetClipboardTextFn=nullptr;
}
// A sign-in started under Linked accounts before any Connect Discord visit
// is retired like any other once a connect link arrives: its late Connect
// answer, a failed or an unanswered poll neither stop the new journey nor
// wait again.
void FirstSignInRetires(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 for(int ending=0;ending<3;++ending){
  Journey h;auto& id=h.view.identity;id.known=true;id.state="ready";h.view.tournament.connect.confirm=true;
  h.identify();
  id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
  const auto late=[&](std::uint64_t ticket,bool ok){
   h.view.identityTicket=ticket;h.view.identityRequest=id.requestId=ticket+100;id.ok=ok;id.failure=ok?"":"rate_limited";h.Frame(0,2);
  };
  h.Screen("linked-accounts");
  Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not inspect the service");
  id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"Linked accounts did not read the account");
  id.discordUser.clear();id.discordName.clear();h.answer();for(int i=0;i<4;++i)h.answer();
  h.Choose("id-discord-connect");
  Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_1","Connect did not name its service");
  if(ending>0){h.answer();h.Frame(0,300);Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_1","No poll was in flight");}
  const auto ticket=h.sent().back()->ticket;
  h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
  if(ending==0)late(ticket,true);
  else if(ending==1)late(ticket,false);
  else h.Frame(0,7300);
  // Only a Connect answered after the link could say to finish in the browser.
  Check((ending>0||h.status!=loc::T("identity.done.discord_opened"))&&h.status!=loc::T("identity.failure.rate_limited")&&
   h.status!=loc::T("identity.failure.timeout"),"The first sign-in's late answer changed the new journey's status");
  Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"The first sign-in's late answer stopped the new journey");
  id.inspected=id.bridges[1];h.answer();for(int i=0;i<4;++i)h.answer();
  const auto before=h.sent().size();h.Frame(0,300);
  for(std::size_t i=before;i<h.sent().size();++i)Check(h.sent()[i]->bridge!="brg_1","The first sign-in was waited for again");
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
 for(int ending=0;ending<4;++ending){
  Journey h;auto& id=h.view.identity;id.known=true;id.state="ready";h.view.tournament.connect.confirm=true;
  h.identify();
  id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
  const auto reply=[&](bool ok){
   h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=ok;id.failure=ok?"":"service_unavailable";h.Frame(0,2);
  };
  if(ending<2){
   // Linked accounts is reading Ember's service when the link for the other arrives.
   h.Screen("linked-accounts");
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Linked accounts did not inspect its service");
   if(ending==1){id.inspected=id.bridges[0];h.answer();Check(h.sent().back()->op==IdentityOp::LinkList,"Linked accounts did not list its links");}
   h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   reply(false);
   Check(h.status!=loc::T("identity.failure.unreachable"),"A replaced Linked accounts read said it failed");
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"A replaced Linked accounts read held the journey back");
   id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
   Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","The journey did not read its service's account");
   id.discordUser.clear();id.discordName.clear();h.answer();
   Check(h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled&&!h.row("dc-retry"),"The journey did not offer Connect");
  }else if(ending==2){
   // The journey's own reads fail: Try again reads them again.
   h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"The journey did not inspect its service");
   reply(false);
   Check(h.status==loc::T("identity.failure.unreachable")&&h.row("dc-retry")&&!h.row("id-discord-connect")->enabled,
    "A failed inspection did not offer Try again");
   h.Choose("dc-retry");
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"Try again did not inspect the service again");
   id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
   Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","The service's account was not read");
   reply(false);
   Check(h.row("dc-retry")&&!h.row("id-discord-connect")->enabled,"A failed account read did not offer Try again");
   h.Choose("dc-retry");
   Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","Try again did not read the account again");
   id.discordUser.clear();id.discordName.clear();h.answer();
   Check(h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled&&!h.row("dc-retry"),"A read account did not offer Connect");
  }
  if(ending==3){
   // A sign-in on the journey's service: a poll fails partway.
   h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   Check(h.until(IdentityOp::BridgeInspect),"The journey did not inspect its service");
   id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
   Check(h.until(IdentityOp::DiscordStatus),"The journey did not read its account");
   id.discordUser.clear();id.discordName.clear();h.answer();
   h.Choose("id-discord-connect");h.answer();h.Frame(0,300);
   Check(h.sent().back()->op==IdentityOp::DiscordStatus&&h.sent().back()->bridge=="brg_2","No poll was sent");
   reply(false);
   Check(h.row("dc-retry")&&!h.row("id-discord-connect")->enabled,"A failed poll did not offer Try again in place of Connect");
   const auto paused=h.sent().size();h.Frame(0,300);
   Check(h.sent().size()==paused,"Polls went on after a failed poll");
   // Try again reads the account; the sign-in finished meanwhile.
   h.Choose("dc-retry");
   Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","Try again did not read the account");
   id.discordUser="333";id.discordName="kate";h.answer();
   Check(h.status==loc::Tf("connect.done","kate")&&h.row("dc-connected"),"Try again did not finish the sign-in");
   const auto connects=h.count(IdentityOp::DiscordConnect);
   Check(connects==1,"Recovering sent another Connect");
   // A later read of that account that fails no longer shows it connected.
   h.view.tournament.connect.sequence=2;h.Frame(0,2);
   Check(h.until(IdentityOp::DiscordStatus),"The second visit did not read the account");
   reply(false);
   Check(!h.row("dc-connected")&&h.row("dc-retry"),"A failed read still showed the account connected");
  }
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
 for(int ending=0;ending<2;++ending){
  Journey h;auto& id=h.view.identity;id.known=true;id.state=ending==0?"ready":"locked";h.view.tournament.connect.confirm=true;
  h.identify();
  id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
  const auto link=[&]{h.view.tournament.connect.bridge="brg_2";h.view.tournament.connect.sequence=1;h.Frame(0,2);};
  std::size_t before=0;
  if(ending==0){
   // An unlink under Linked accounts finishes after the link arrived.
   h.Screen("linked-accounts");
   Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not inspect its service");
   id.inspected=id.bridges[0];h.answer();
   Check(h.sent().back()->op==IdentityOp::LinkList,"Linked accounts did not list its links");
   id.links={{"lnk_1","blumint","BluMint","PlayerOne"}};h.answer();for(int i=0;i<4;++i)h.answer();
   h.Choose("id-unlink:lnk_1");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(h.sent().back()->op==IdentityOp::LinkRemove,"Unlink was not sent");
   before=h.sent().size();link();h.answer();
  }else{
   // An unlock typed while the Ember ID screen's status is still out waits
   // behind it; the link arrives; the unlock's submission is refused.
   h.Screen("identity");
   Check(h.sent().back()->op==IdentityOp::Status,"The Ember ID screen did not ask for its status");
   h.Choose("id-unlock");h.type("correct horse");
   Check(h.sent().back()->op==IdentityOp::Status,"The unlock did not wait behind the status");
   before=h.sent().size();link();
   // Only that one submission is refused: the frame that answers the status sends it.
   id.state="ready";
   h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;
   h.accept=false;h.Frame();h.accept=true;
   Check(h.sent().back()->op==IdentityOp::Unlock&&h.status==loc::T("error.queue_failed"),"The unlock was not the refused submission");
   h.Frame();
  }
  Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==other,"An earlier change held the journey back");
  id.inspected=id.bridges[1];id.inspectedDiscord=true;h.answer();
  Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_2","The journey did not read its service's account");
  for(std::size_t i=before;i<h.sent().size();++i)
   Check(!(h.sent()[i]->op==IdentityOp::LinkList&&h.sent()[i]->bridge=="brg_1"),"An earlier change's follow-up read was queued");
 }
}
// Ember ID is on Home. Its screen leads with the matches, then Discord on
// Ember's own service, showing the connected account, which Connect Discord
// can also disconnect.
void EmberIdFromHome(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;id.state="ready";
 h.identify();
 const std::string ember="https://bridge.embernetplay.link";
 id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
 h.Screen("home");
 Check(h.row("identity")!=nullptr,"Home has no Ember ID entry");
 h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","Ember ID on Home did not open its screen");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"The Ember ID screen did not read Ember's own service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_1","The Ember ID screen did not read the Discord account");
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(h.row("tournament-matches")&&h.row("discord-connect")&&h.index("tournament-matches")<h.index("discord-connect")&&h.index("discord-connect")<h.index("linked-accounts"),
  "The Ember ID screen does not lead with matches, then Discord");
 Check(h.row("discord-connect")->value=="kate","The Ember ID screen does not show the connected Discord account");
 // Connect Discord shows it connected and can disconnect it.
 h.Choose("discord-connect");
 Check(h.until(IdentityOp::DiscordStatus),"Connect Discord did not read the account");
 h.answer();for(int i=0;i<4;++i)h.answer();
 Check(h.row("dc-connected")&&h.row("id-discord-remove"),"Connect Discord does not offer to disconnect");
 h.Choose("id-discord-remove");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.sent().back()->op==IdentityOp::DiscordRemove&&h.sent().back()->bridge=="brg_1","Disconnect did not name Ember's service");
 id.discordUser.clear();id.discordName.clear();h.answer();
 Check(!h.row("dc-connected")&&h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled,"A disconnected account still shows connected");
 // Back on the Ember ID screen it reads Not connected.
 h.shell.Navigation().Return();h.Frame(0,2);for(int i=0;i<8;++i)h.answer();
 Check(h.row("discord-connect")&&h.row("discord-connect")->value==loc::T("identity.discord_none"),"The Ember ID screen still shows the account");
}
// Home guides the player: Ember ID says to start there before setup, and,
// once matches are read in the background, how many are ready to play, with
// a notice for each new one. The matches screen's empty states lead to
// Connect Discord.
void HomeGuides(){
 using namespace sf4e;using netplay::IdentityOp;using netplay::tournament::Command;
 const std::string ember="https://bridge.embernetplay.link";
 {
 Journey h;auto& id=h.view.identity;id.known=true;id.state="disabled";
 // The state is not known yet: Home asks for it, and again after a failure.
 id.known=false;h.Screen("home");h.Frame();
 Check(!h.sent().empty()&&h.sent().back()->op==IdentityOp::Status,"Home did not learn the Ember ID's state");
 h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=0;h.view.identityRefusal="identity.refused.helper";h.Frame(0,2);
 h.view.identityRefusal.clear();
 Check(h.status!=loc::T("identity.refused.helper"),"A failed background read was said on Home");
 const auto tries=h.sent().size();h.Frame(0,1900);
 Check(h.sent().size()>tries&&h.sent().back()->op==IdentityOp::Status,"Home did not read the state again after a failure");
 id.known=true;h.answer();
 Check(h.row("identity")&&h.row("identity")->detail==loc::T("home.identity_start"),"Home does not say to start at Ember ID");
 // With an ID and Ember's service, the matches are read in the background.
 h.ready();
 id.bridges={{"brg_1",ember,"Ember"}};h.Frame(0,2);
 Check(h.sent().back()->op==IdentityOp::BridgeList,"Home did not learn the services");
 h.answer();h.Frame(0,2);
 Check(!h.played().empty()&&h.played().back()->op==Command::Op::Refresh&&h.played().back()->bridgeId=="brg_1","Home did not read the matches");
 Check(h.row("identity")->detail==loc::T("home.identity_detail"),"Home says to start with an ID and a service");
 auto& t=h.view.tournament;t.list.bridge="brg_1";
 netplay::tournament::Assignment match;match.matchId="emt_1";match.state="ready";match.profile="ember-room-v1";match.gamesToWin=2;
 t.list.items={match};h.Frame(0,2);
 Check(h.status==loc::T("tournament.assigned_notice"),"A new match was not announced");
 Check(h.row("identity")->detail==loc::Tf("home.identity_matches",1),"Home does not count the ready match");
 // Read again a minute later.
 const auto reads=h.played().size();h.Frame(0,3700);
 Check(h.played().size()>reads&&h.played().back()->op==Command::Op::Refresh,"The matches were not read again");
 }
 // The matches screen's empty states lead to Connect Discord.
 Journey fresh;auto& other=fresh.view.identity;other.known=true;other.state="disabled";
 fresh.Screen("tournament-matches");fresh.Frame();
 Check(fresh.row("id-linked-unavailable")&&fresh.row("discord-connect"),"Without an ID the matches screen does not lead to Connect Discord");
 fresh.ready();fresh.Frame(0,2);
 Check(fresh.row("tm-no-service")&&fresh.row("tm-no-service")->detail==loc::T("tournament.needs_discord_detail")&&fresh.row("discord-connect"),
  "Without a service the matches screen does not lead to Connect Discord");
}
// The one-link attempt's guards: a second link while Discord's page is being
// opened opens no second page; Cancel during setup stops it, and a later link
// for the same service shows it rather than starting again; changing account
// finishes only when another account is read; a service the player removed
// is not trusted by itself.
void OnboardingGuards(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link";
 for(int part=0;part<3;++part){
  Journey h;auto& id=h.view.identity;id.known=true;
  id.state=part==0?"ready":"disabled";
  if(part==0){h.identify();id.bridges={{"brg_1",ember,"Ember"}};}
  if(part==2)id.removedBridges={"brg_1"};
  h.Frame();
  h.untilLimit=12;
  const auto link=[&](std::uint64_t sequence){h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=sequence;h.Frame(0,2);};
  if(part==0){
   // A second link while Discord's page is being opened opens no second one.
   link(1);
   Check(h.until(IdentityOp::BridgeInspect),"The link did not inspect its service");
   id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
   Check(h.until(IdentityOp::DiscordStatus),"The link did not read the account");
   id.discordUser.clear();id.discordName.clear();h.answer();
   Check(h.sent().back()->op==IdentityOp::DiscordConnect,"The link did not open Discord");
   const auto opening=h.sent().back()->ticket;
   link(2);
   h.view.identityTicket=opening;h.view.identityRequest=id.requestId=opening+100;id.ok=true;h.Frame(0,2);
   Check(h.row("dc-waiting")&&h.count(IdentityOp::DiscordConnect)==1,"A second link opened a second page or lost the wait");
   // Changing account: the old account read back keeps waiting; another finishes.
   id.discordUser="111";id.discordName="kate";h.Frame(0,300);h.until(IdentityOp::DiscordStatus);h.answer();
   Check(h.row("dc-connected")&&h.status==loc::Tf("connect.done","kate"),"The first account did not finish the sign-in");
   h.Choose("id-discord-connect");
   Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Use a different Discord account did not open Discord");
   h.answer();h.Frame(0,300);
   Check(h.sent().back()->op==IdentityOp::DiscordStatus,"The change of account was not read");
   h.answer();h.Frame(0,300);h.answer();
   Check(h.row("dc-waiting")&&!h.row("dc-connected"),"The old account read back ended the change of account");
   id.discordUser="222";id.discordName="sam";h.Frame(0,300);h.answer();
   Check(h.row("dc-connected")&&h.row("dc-connected")->value=="sam","The new account did not finish the change");
  }else if(part==1){
   // Cancel during setup stops it; a later link for the service only shows it.
   link(1);
   Check(h.row("dc-progress")&&h.row("dc-cancel"),"Setting up offers no Cancel");
   h.Choose("dc-cancel");h.Frame(0,2);for(int i=0;i<6;++i)h.answer();
   Check(h.count(IdentityOp::Enable)==0&&h.status==loc::T("connect.cancelled"),"Cancel did not stop the setup");
   link(2);for(int i=0;i<6;++i)h.answer();
   Check(h.count(IdentityOp::Enable)==0&&h.row("dc-go"),"A link restarted a cancelled attempt");
   h.Choose("dc-go");
   Check(h.sent().back()->op==IdentityOp::Enable,"Connect did not restart the attempt");
  }else{
   // A service the player removed is offered to trust, not trusted.
   link(1);
   Check(h.until(IdentityOp::Enable),"The link did not create the Ember ID");
   h.ready();h.answer();
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"The link did not look up Ember's own service");
   id.inspected={"brg_1",ember,"Ember"};h.answer();for(int i=0;i<4;++i)h.answer();
   Check(h.row("id-approve")&&h.count(IdentityOp::BridgeApprove)==0,"A removed service was trusted by itself");
  }
 }
}
// Cancel reaches the service even when something clears the requests first:
// an account read out when Cancel is pressed fails or times out, or Ember is
// hidden while Discord's page is being opened.
void CancelReachesTheService(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link";
 for(int ending=0;ending<3;++ending){
  Journey h;auto& id=h.view.identity;id.known=true;h.ready();id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
  h.untilLimit=12;
  h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
  Check(h.until(IdentityOp::BridgeInspect),"The link did not read the service");
  id.inspected={"brg_1",ember,"Ember"};id.inspectedDiscord=true;h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"The link did not read the account");
  id.discordUser.clear();id.discordName.clear();h.answer();
  Check(h.sent().back()->op==IdentityOp::DiscordConnect,"The link did not open Discord");
  if(ending<2){
   h.answer();h.Frame(0,300);
   Check(h.sent().back()->op==IdentityOp::DiscordStatus,"The account was not read while waiting");
   h.Choose("dc-cancel");
   if(ending==0)h.answer(false,"service_unavailable");
   else h.Frame(0,7300);
   h.Frame(0,2);
  }else{
   // Hidden and left hidden: only the hidden frames run.
   const auto hidden=[&]{
    ImGui::GetIO().DeltaTime=1.f/60;ImGui::NewFrame();
    h.shell.Background(h.view,[&](auto a){h.actions.push_back(a);return h.accept;});ImGui::Render();
   };
   hidden();
   h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=true;id.failure.clear();
   hidden();hidden();
  }
  Check(h.sent().back()->op==IdentityOp::DiscordCancel&&h.sent().back()->bridge=="brg_1",
   ending==0?"A failed account read lost the cancellation":ending==1?"A timed-out account read lost the cancellation":"Hiding Ember lost the cancellation");
  Check(h.count(IdentityOp::DiscordCancel)==1,"The cancellation was sent more than once");
 }
}
// A link meets a locked Ember ID: Unlock shows, not an endless Setting up,
// and unlocking goes on to Discord by itself. Cancel is there at every step
// of setup. Hiding Ember during setup stops it: reopening sends nothing more.
void OnboardingSteps(){
 using namespace sf4e;using netplay::IdentityOp;
 const std::string ember="https://bridge.embernetplay.link";
 for(int part=0;part<3;++part){
  Journey h;auto& id=h.view.identity;id.known=true;id.state=part==0?"locked":"disabled";h.Frame();
  h.untilLimit=12;
  h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
  if(part==0){
   for(int i=0;i<6;++i)h.answer();
   Check(h.row("id-unlock")&&!h.row("dc-progress"),"A locked ID hid Unlock behind Setting up");
   h.Choose("id-unlock");h.type("correct horse");
   Check(h.sent().back()->op==IdentityOp::Unlock,"Unlock did not send");
   h.ready();h.answer();
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Unlocking did not go on");
   id.inspected={"brg_1",ember,"Ember"};id.inspectedDiscord=true;h.answer();
   Check(h.sent().back()->op==IdentityOp::BridgeApprove,"Unlocking did not go on to trust Ember's service");
   id.bridges={{"brg_1",ember,"Ember"}};
   Check(h.until(IdentityOp::DiscordStatus),"Unlocking did not go on to read the account");
   id.discordUser.clear();id.discordName.clear();h.answer();
   Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Unlocking did not go on to open Discord");
   // Cancel while Discord's page is open ends the sign-in on the service
   // too. A service that cannot end it says nothing, and Connect works again.
   h.answer();Check(h.row("dc-waiting")&&h.row("dc-cancel"),"Not waiting for Discord");
   h.Choose("dc-cancel");h.Frame(0,2);
   // An account read may still be out from the wait.
   Check(h.until(IdentityOp::DiscordCancel)&&h.sent().back()->bridge=="brg_1","Cancel did not end the sign-in on the service");
   h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=false;id.failure="bridge_unavailable";h.Frame(0,2);
   id.ok=true;id.failure.clear();
   Check(h.status==loc::T("connect.cancelled"),"A service that could not end the sign-in was reported");
   Check(h.row("id-discord-connect")&&h.row("id-discord-connect")->enabled&&!h.row("dc-waiting"),"Connect is not offered again after Cancel");
   h.Choose("id-discord-connect");h.Frame(0,2);
   Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Connect did not open Discord again after Cancel");
  }else if(part==1){
   // Cancel at each step: creating the ID, looking up and trusting the service.
   Check(h.row("dc-cancel")!=nullptr,"No Cancel while the state is read");
   Check(h.until(IdentityOp::Enable)&&h.row("dc-cancel"),"No Cancel while the ID is created");
   h.ready();h.answer();
   Check(h.until(IdentityOp::BridgeInspect)&&h.row("dc-cancel"),"No Cancel while the service is looked up");
   id.inspected={"brg_1",ember,"Ember"};id.inspectedDiscord=true;h.answer();
   Check(h.sent().back()->op==IdentityOp::BridgeApprove&&h.row("dc-cancel"),"No Cancel while the service is trusted");
   h.Choose("dc-cancel");id.bridges={{"brg_1",ember,"Ember"}};for(int i=0;i<8;++i)h.answer();
   // The trust already sent finishes; nothing goes on to open Discord.
   Check(h.count(IdentityOp::DiscordConnect)==0&&h.row("id-discord-connect")&&!h.row("dc-cancel"),"Cancel did not stop the setup");
   Check(h.count(IdentityOp::DiscordCancel)==0,"Cancel before Discord's page asked the service to end a sign-in");
  }else{
   // Hiding Ember while the state is read stops the setup.
   Check(h.sent().back()->op==IdentityOp::Status,"The link did not read the state");
   const auto before=h.sent().size();
   h.shell.Conceal();h.Frame();
   h.shell.Navigation().Home();h.shell.Navigation().Push("discord-connect");h.Frame(0,2);
   for(int i=0;i<8;++i)h.answer();
   for(std::size_t i=before;i<h.sent().size();++i)
    Check(h.sent()[i]->op!=IdentityOp::Enable&&h.sent()[i]->op!=IdentityOp::DiscordConnect,"Reopening Ember resumed a hidden setup");
   Check(h.row("dc-go")!=nullptr,"A stopped setup does not offer Connect");
  }
 }
}
// A link whose first read of the state fails offers Try again, which goes on
// by itself. Visiting the Ember ID screen reads Ember's Discord account
// without changing the tournament service the player selected. A failed read
// of a service whose sign-in is off offers Try again, not "off".
void OnboardingRecovers(){
 using namespace sf4e;using netplay::IdentityOp;using netplay::tournament::Command;
 const std::string ember="https://bridge.embernetplay.link",other="https://tournaments.example";
 for(int part=0;part<3;++part){
  Journey h;auto& id=h.view.identity;
  h.untilLimit=12;
  if(part==0){
   // The state is not known yet when the link arrives, and its read is refused.
   id.known=false;h.Frame();
   h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
   // Home's own read was still out when the link arrived; the link's follows.
   for(int i=0;i<2;++i){
    Check(h.sent().back()->op==IdentityOp::Status,"The link did not read the state");
    h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=0;h.view.identityRefusal="identity.refused.helper";h.Frame(0,2);
   }
   h.view.identityRefusal.clear();h.Frame(0,400);
   Check(h.row("dc-retry")&&!h.row("dc-progress"),"A failed first read left Setting up with nothing to wait for");
   h.Choose("dc-retry");
   Check(h.sent().back()->op==IdentityOp::Status,"Try again did not read the state");
   id.known=true;id.state="disabled";
   Check(h.until(IdentityOp::Enable),"Try again did not go on by itself");
  }else if(part==2){
   id.known=true;h.ready();
   id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
   const auto next=[&](IdentityOp op){
    const auto from=h.sent().size();
    for(int i=0;i<12&&!(h.sent().size()>from&&h.sent().back()->op==op);++i)h.answer();
    return h.sent().size()>from&&h.sent().back()->op==op;
   };
   h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.view.tournament.connect.confirm=true;h.Frame(0,2);
   Check(h.until(IdentityOp::BridgeInspect),"Connect Discord did not read the service");
   id.inspected=id.bridges[0];id.inspectedDiscord=false;id.inspectedDiscordAccounts=false;h.answer();
   Check(h.row("dc-off")&&!h.row("dc-retry"),"A service without sign-in was not said");
   // The next visit's read fails.
   h.Screen("home");h.Screen("discord-connect");
   Check(next(IdentityOp::BridgeInspect),"Connect Discord did not read the service again");
   h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=false;id.failure="bridge_unavailable";h.Frame(0,2);
   id.ok=true;id.failure.clear();
   Check(h.row("dc-retry")&&!h.row("dc-off"),"A failed read of a service without sign-in said it is off");
   h.Choose("dc-retry");
   Check(next(IdentityOp::BridgeInspect),"Try again did not read the service");
   h.answer();
   Check(h.row("dc-off")&&!h.row("dc-retry"),"A good read after a failed one did not say off");
  }else{
   id.known=true;h.ready();
   id.bridges={{"brg_1",ember,"Ember"},{"brg_2",other,"Other"}};h.Frame();
   // The player selects the other service for its matches.
   h.Screen("tournament-matches");for(int i=0;i<6;++i)h.answer();
   h.FocusOn("id-bridge");h.Press(MenuInput::Select);h.Press(MenuInput::Down);h.Press(MenuInput::Select);
   for(int i=0;i<6;++i)h.answer();
   Check(h.row("id-bridge")&&h.row("id-bridge")->value=="Other","The other service was not selected");
   // The Ember ID screen reads Ember's own Discord account.
   h.Screen("identity");
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"The Ember ID screen did not read Ember's service");
   id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
   Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_1","The Ember ID screen did not read the account");
   id.discordUser="111";id.discordName="kate";h.answer();
   Check(h.row("discord-connect")&&h.row("discord-connect")->value=="kate","The Ember ID screen does not show the account");
   // A failed read of Ember's service no longer shows its account as current.
   h.Screen("home");h.Screen("identity");
   Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"The Ember ID screen did not read Ember's service again");
   h.view.identityTicket=h.sent().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;id.ok=false;id.failure="service_unavailable";h.Frame(0,2);
   Check(h.row("discord-connect")&&h.row("discord-connect")->value!="kate","A failed read still showed Ember's account as current");
   // Back on the matches, the selection is the player's.
   h.Screen("tournament-matches");for(int i=0;i<6;++i)h.answer();
   Check(h.row("id-bridge")&&h.row("id-bridge")->value=="Other","Visiting Ember ID changed the selected service");
   Check(!h.played().empty()&&h.played().back()->bridgeId=="brg_2","The matches are no longer read from the selected service");
  }
 }
}
}
void RunDiscordConnectJourneys(){
 DiscordAndFirstLink();DiscordConnectLink();DiscordWaitsForItsService();DiscordConnectKeepsItsService();FirstSignInRetires();ConnectReadsRecover();
 RetainedChangesStayTheirs();EmberIdFromHome();HomeGuides();OnboardingGuards();OnboardingSteps();CancelReachesTheService();OnboardingRecovers();DiscordConnectPastedLink();
}
