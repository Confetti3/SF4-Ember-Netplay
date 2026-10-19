// The sign-in site journeys: one row per site Ember's own service lists in
// its capabilities, under Online play > Ember ID after Discord, and each
// site's Connect screen, which opens the site's page through the helper,
// reads the account until it appears, and unlinks it. The sites are only
// their capabilities entries: Challonge, start.gg and a site nobody wrote
// code for run the same journeys with the same words. Run from the identity
// journey test's main.
#include "identity_journey_support.hxx"
#include <map>
#include <set>
#include <vector>
namespace {
const std::string ember="https://bridge.embernetplay.link";
struct Case{const char* id;const char* name;};
const Case Challonge{"challonge","Challonge"},Startgg{"startgg","start.gg"},Other{"brackets-plus","Brackets Plus"};

// A ready Ember ID that trusts Ember's own service.
void Ready(Journey& h){
 auto& id=h.view.identity;id.known=true;h.ready();id.bridges={{"brg_1",ember,"Ember"}};h.Frame();
}
// Ember's own service's profile lists these sites, offering sign-in or not.
void List(Journey& h,const std::vector<Case>& sites,bool signIn){
 auto& id=h.view.identity;id.inspected=id.bridges[0];id.signInSites.clear();
 for(const auto& s:sites)id.signInSites.push_back({s.id,s.name,signIn});
}
// Answers until the profile of Ember's own service is asked for, then gives it `sites`.
void Profile(Journey& h,const std::vector<Case>& sites,bool signIn=true){
 using sf4e::netplay::IdentityOp;
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Ember's own service was not read");
 List(h,sites,signIn);h.answer();
}
// The newest request reads `site`'s account on Ember's own service.
bool ReadsSite(const Journey& h,const Case& site){
 const auto* r=h.sent().back();return r->op==sf4e::netplay::IdentityOp::SiteStatus&&r->bridge=="brg_1"&&r->site==site.id;
}
std::string RowId(const Case& site){return std::string(SiteRowPrefix)+site.id;}
// Opens the site's Connect screen from its row on the Ember ID screen; the
// screen then reads Ember's own service, whose profile lists `sites`.
void OpenSite(Journey& h,const Case& site,bool signIn=true,const std::vector<Case>& sites={}){
 const auto listed=sites.empty()?std::vector<Case>{site}:sites;
 h.Screen("identity");Profile(h,listed,signIn);for(int i=0;i<6;++i)h.answer();
 h.Choose(RowId(site).c_str());
 Check(h.shell.Navigation().Screen()==SiteConnectScreen,"The site's row did not open its Connect screen");
 Profile(h,listed,signIn);
}
template<class... A> std::string Words(const char* id,const Case& site,A&&... a){return sf4e::loc::Tf(id,site.name,std::forward<A>(a)...);}

// The Ember ID screen lists exactly the sites the service does, after
// Discord, with the account read on each; a service that stops listing a site
// loses its row.
void SiteRows(const Case& site){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);
 h.Screen("identity");Profile(h,{});for(int i=0;i<4;++i)h.answer();
 Check(h.row("discord-connect")&&!h.row(RowId(site).c_str()),"The Ember ID screen lists a site the service does not");
 Check(h.count(IdentityOp::SiteStatus)==0,"An account was read from a site the service does not list");
 h.Screen("home");h.Screen("identity");Profile(h,{site},false);
 Check(ReadsSite(h,site),"The site's account was not read");
 h.view.identity.siteUser="1001";h.view.identity.siteName="kate";h.answer();
 const auto* row=h.row(RowId(site).c_str());
 Check(row&&row->label==site.name&&row->value=="kate"&&h.index(RowId(site).c_str())==h.index("discord-connect")+1,
  "The Ember ID screen does not show the site's account after Discord");
 h.Choose(RowId(site).c_str());
 Check(h.trail()=="home>identity>sign-in-site","The site's row did not open its Connect screen");
 h.Screen("home");h.Screen("identity");Profile(h,{});for(int i=0;i<4;++i)h.answer();
 Check(h.row("discord-connect")&&!h.row(RowId(site).c_str()),"A site's row stayed on a service that no longer lists it");
}

// Connect opens the site's page through the helper; the account is read
// every few seconds until it appears, and Unlink asks first.
void SiteConnects(const Case& site){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 OpenSite(h,site);
 Check(ReadsSite(h,site),"The site's Connect screen did not read the account");
 id.siteUser.clear();id.siteName.clear();h.answer();
 Check(h.row("st-connect")&&h.row("st-connect")->enabled&&h.row("st-connect")->label==Words("site.title",site)&&!h.row("st-cancel"),
  "Connect is not offered in the site's words");
 h.Choose("st-connect");
 Check(h.sent().back()->op==IdentityOp::SiteConnect&&h.sent().back()->bridge=="brg_1"&&h.sent().back()->site==site.id,
  "Connect did not ask the helper to open the site");
 Check(h.row("st-progress")&&h.row("st-cancel"),"Opening the site is not shown with Cancel");
 h.answer();
 Check(h.status==Words("site.opened",site)&&h.row("st-waiting")&&h.row("st-cancel"),"Waiting for the site is not shown with Cancel");
 const auto polled=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()>polled&&ReadsSite(h,site),"The account was not read again while waiting");
 h.answer();h.Frame(0,1900);
 Check(h.status==Words("site.waiting",site),"The waiting line is not shown once the first notice ends");
 id.siteUser="1001";id.siteName="kate";h.until(IdentityOp::SiteStatus);h.answer();
 Check(h.status==Words("site.done",site,"kate")&&h.row("st-connected")&&h.row("st-connected")->value=="kate","A connected account was not shown");
 Check(h.row("st-connect")&&h.row("st-remove")&&!h.row("st-cancel"),"A connected account offers no other account or Unlink");
 const auto settled=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()==settled,"The account was still read again after it connected");
 h.Choose("st-remove");
 Check(h.sent().size()==settled,"Unlink sent without asking");
 h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.sent().back()->op==IdentityOp::SiteRemove&&h.sent().back()->site==site.id,"Unlink did not send");
 id.siteUser.clear();id.siteName.clear();h.answer();
 Check(h.status==Words("site.removed",site)&&h.row("st-connect")&&!h.row("st-connected"),"An unlinked account is still shown");
}

// Cancel ends the sign-in on the service, and so does leaving the screen
// while the page is still being opened; a sign-in on its way stays.
void SiteStops(const Case& site){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 OpenSite(h,site);h.answer();
 h.Choose("st-connect");h.answer();
 h.Choose("st-cancel");
 Check(h.status==Words("site.cancelled",site)&&!h.row("st-waiting")&&!h.row("st-cancel"),"Cancel did not stop waiting");
 h.Frame(0,2);
 Check(h.sent().back()->op==IdentityOp::SiteCancel&&h.sent().back()->site==site.id,"Cancel did not end the sign-in on the service");
 h.answer();
 const auto stopped=h.sent().size();h.Frame(0,300);
 Check(h.sent().size()==stopped,"A cancelled sign-in was still waited for");
 h.Choose("st-connect");
 Check(h.sent().back()->op==IdentityOp::SiteConnect,"Connect did not send");
 h.Screen("identity");h.answer();
 Check(h.until(IdentityOp::SiteCancel)&&h.sent().back()->site==site.id,"Leaving while the site opened did not end the sign-in");
 h.Screen(SiteConnectScreen);for(int i=0;i<6;++i)h.answer();
 h.Choose("st-connect");h.answer();
 const auto cancels=h.count(IdentityOp::SiteCancel);
 h.Screen("identity");for(int i=0;i<6;++i)h.answer();
 Check(h.count(IdentityOp::SiteCancel)==cancels,"Leaving a sign-in that waits for the browser ended it");
 h.Screen(SiteConnectScreen);for(int i=0;i<6;++i)h.answer();
 Check(h.row("st-waiting")!=nullptr,"Coming back no longer showed the sign-in waiting");
 id.siteUser="1001";id.siteName="kate";h.Frame(0,300);
 Check(h.until(IdentityOp::SiteStatus),"The waiting sign-in was not read again");
 h.answer();
 Check(h.status==Words("site.done",site,"kate"),"The waiting sign-in did not finish");
}

// Sign-in off with an account kept: it can be unlinked, none connected. A
// failed read offers Try again. Without Ember's own service there is nothing
// to connect.
void SiteOffAndRetry(const Case& site){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 OpenSite(h,site,false);
 id.siteUser.clear();id.siteName.clear();h.answer();
 Check(h.row("st-off")&&h.row("st-off")->label==Words("site.off",site)&&!h.row("st-connect"),"Connect is offered where sign-in is off");
 id.siteUser="1001";id.siteName="kate";
 h.Screen("home");h.Screen(SiteConnectScreen);Profile(h,{site},false);h.answer();
 Check(h.row("st-connected")&&h.row("st-remove"),"An account kept without sign-in cannot be unlinked");
 h.Screen("home");h.Screen(SiteConnectScreen);Profile(h,{site});
 Check(ReadsSite(h,site),"The account was not read");
 h.answer(false,"service_unavailable");
 Check(h.row("st-retry")&&h.row("st-connect")&&!h.row("st-connect")->enabled,"A failed read does not offer Try again");
 h.Choose("st-retry");Profile(h,{site});h.answer();
 Check(h.row("st-connected")&&!h.row("st-retry"),"Try again did not read the account");
 id.bridges.clear();h.Screen("home");h.Screen(SiteConnectScreen);for(int i=0;i<4;++i)h.answer();
 Check(h.row("st-no-service")&&h.row("linked-accounts")&&!h.row("st-connect")&&!h.row("st-off"),"Without Ember's own service the screen does not say to set it up");
}

// Over its request limits a site says so in its own words; Discord's words
// for the same code stay the general ones.
void SiteBusy(const Case& site){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 OpenSite(h,site);h.answer();
 h.Choose("st-connect");
 Check(h.sent().back()->op==IdentityOp::SiteConnect,"Connect did not ask the helper to open the site");
 h.answer(false,"sign_in_busy");
 Check(h.status==Words("site.busy",site),"A busy sign-in was not said in the site's words");
 Check(h.row("st-connect")&&!h.row("st-waiting")&&!h.row("st-cancel"),"A busy sign-in still waits");
 h.Screen("linked-accounts");
 Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not read the service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"Linked accounts did not read Discord's account");
 id.discordUser.clear();id.discordName.clear();h.answer();for(int i=0;i<4;++i)h.answer();
 h.Choose("id-discord-connect");
 Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Connect did not open Discord");
 h.answer(false,"sign_in_busy");
 Check(h.status==loc::Tf("identity.failure.other","sign_in_busy"),"Discord's words for a refusal changed");
}

void SiteWaiting(Journey& h,const Case& site);
// A site its service stops listing while the player uses its screen: the
// account read before goes, and the screen says sign-in is off, still in the
// site's own name; a sign-in waiting on it ends on the service. A Connect the
// service no longer knows says so, in the site's words.
void SiteGone(const Case& site){
 using namespace sf4e;using netplay::IdentityOp;
 {
  Journey h;Ready(h);auto& id=h.view.identity;
  OpenSite(h,site);
  id.siteUser="1001";id.siteName="kate";h.answer();
  Check(h.row("st-connected")&&h.row("st-remove"),"The site's account was not shown");
  h.Screen("home");h.Screen(SiteConnectScreen);Profile(h,{});for(int i=0;i<4;++i)h.answer();
  Check(!h.row("st-connected")&&!h.row("st-remove"),"A site the service no longer lists still showed its account");
  Check(h.row("st-off")&&h.row("st-off")->label==Words("site.off",site),"A site the service no longer lists was not said to be off by name");
 }
 {
  Journey h;Ready(h);
  SiteWaiting(h,site);
  h.Screen("home");h.Screen(SiteConnectScreen);Profile(h,{});h.Frame(0,2);
  Check(h.sent().back()->op==IdentityOp::SiteCancel&&h.sent().back()->site==site.id,"A sign-in on a site no longer listed did not end");
  h.answer();
  Check(!h.row("st-waiting")&&!h.row("st-cancel")&&h.row("st-off"),"A sign-in on a site no longer listed still waits");
  const auto reads=h.count(IdentityOp::SiteStatus);h.Frame(0,300);
  Check(h.count(IdentityOp::SiteStatus)==reads,"A site no longer listed was still read");
 }
 {
  Journey h;Ready(h);
  OpenSite(h,site);h.answer();
  h.Choose("st-connect");
  Check(h.sent().back()->op==IdentityOp::SiteConnect,"Connect did not ask the helper to open the site");
  h.answer(false,"not_found");
  Check(h.status==Words("site.gone",site),"A site the service no longer has was not said so");
 }
 {
  // Its sign-in switched off since the list was read: said so, and shown off.
  Journey h;Ready(h);
  OpenSite(h,site);h.answer();
  h.Choose("st-connect");
  Check(h.sent().back()->op==IdentityOp::SiteConnect,"Connect did not ask the helper to open the site");
  h.answer(false,"sign_in_off");
  Check(h.status==Words("site.off_detail",site),"A site whose sign-in is off was not said so");
  Check(h.row("st-off")&&!h.row("st-connect"),"A site whose sign-in is off still offers Connect");
 }
}

// Every site a service lists, up to the most kept, has its account read on
// the Ember ID screen.
void ManySites(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 h.Screen("identity");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"Ember's own service was not read");
 id.inspected=id.bridges[0];id.signInSites.clear();
 for(std::size_t n=0;n<netplay::MaxSignInSites;++n)id.signInSites.push_back({"site-"+std::to_string(n),"Site "+std::to_string(n),true});
 h.answer();
 std::set<std::string> read;
 for(int i=0;i<40;++i){const auto* r=h.sent().back();if(r->op==IdentityOp::SiteStatus)read.insert(r->site);h.answer();}
 Check(read.size()==netplay::MaxSignInSites,"Not every listed site's account was read");
 for(std::size_t n=0;n<netplay::MaxSignInSites;++n){
  const auto row=h.row((std::string(SiteRowPrefix)+"site-"+std::to_string(n)).c_str());
  Check(row&&row->value==loc::Tf("site.none","Site "+std::to_string(n)),"A listed site's row did not show its account");
 }
}

// A site's name comes from its service, so the labels carrying it are
// elided when long, as a player's own text is, rather than checked to fit.
void LongSiteName(){
 using namespace sf4e;
 const Case site{"long-name","A tournament site whose name runs as long as a service may make it"};
 Journey h;Ready(h);auto& id=h.view.identity;
 OpenSite(h,site);id.siteUser.clear();id.siteName.clear();h.answer();
 Check(h.row("st-connect")&&h.row("st-connect")->userText&&h.row("st-connect")->label==Words("site.title",site),
  "A label with the site's name is checked to fit");
 h.Screen("home");OpenSite(h,site,false);h.answer();
 Check(h.row("st-off")&&h.row("st-off")->userText,"The off label with the site's name is checked to fit");
}

// Answers what is asked, each site's read with that site's account, until
// nothing new is asked.
using Accounts=std::map<std::string,std::pair<std::string,std::string>>;
void Settle(Journey& h,const Accounts& accounts){
 auto& id=h.view.identity;
 for(int i=0;i<16;++i){
  const auto* r=h.sent().back();
  if(r->op==sf4e::netplay::IdentityOp::SiteStatus){
   const auto it=accounts.find(r->site);
   id.siteUser=it==accounts.end()?std::string():it->second.first;id.siteName=it==accounts.end()?std::string():it->second.second;
  }
  h.answer();
 }
}

// Two sites on one service: both rows, each its own account, and one
// sign-in at a time between them.
void TwoSites(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);
 List(h,{Challonge,Startgg},true);
 Accounts accounts{{Challonge.id,{"1001","Kate_99"}},{Startgg.id,{"2002","kate.gg"}}};
 h.Screen("identity");Settle(h,accounts);
 Check(h.count(IdentityOp::SiteStatus)>=2,"The two sites' accounts were not read");
 Check(h.row(RowId(Challonge).c_str())&&h.row(RowId(Challonge).c_str())->value=="Kate_99"&&
  h.row(RowId(Startgg).c_str())&&h.row(RowId(Startgg).c_str())->value=="kate.gg"&&
  h.index(RowId(Startgg).c_str())==h.index(RowId(Challonge).c_str())+1,"Two sites' rows do not show their own accounts");
 // Unlinking one leaves the other.
 h.Choose(RowId(Startgg).c_str());Settle(h,accounts);
 h.Choose("st-remove");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.sent().back()->op==IdentityOp::SiteRemove&&h.sent().back()->site==Startgg.id,"Unlink did not name its site");
 accounts.erase(Startgg.id);Settle(h,accounts);
 h.Screen("identity");Settle(h,accounts);
 Check(h.row(RowId(Challonge).c_str())&&h.row(RowId(Challonge).c_str())->value=="Kate_99"&&
  h.row(RowId(Startgg).c_str())&&h.row(RowId(Startgg).c_str())->value==loc::Tf("site.none",Startgg.name),
  "Unlinking one site changed the other's account");
 // One sign-in at a time: start.gg's Connect ends Challonge's waiting sign-in first.
 accounts.erase(Challonge.id);
 h.Choose(RowId(Challonge).c_str());Settle(h,accounts);
 h.Choose("st-connect");h.answer();
 Check(h.row("st-waiting")!=nullptr,"Challonge is not waited for");
 h.Screen("identity");Settle(h,accounts);
 h.Choose(RowId(Startgg).c_str());Settle(h,accounts);
 const auto before=h.sent().size();
 h.Choose("st-connect");
 Check(h.sent().size()==before+1&&h.sent().back()->op==IdentityOp::SiteCancel&&h.sent().back()->site==Challonge.id,
  "Connecting start.gg did not end Challonge's sign-in first");
 h.answer();
 Check(h.sent().back()->op==IdentityOp::SiteConnect&&h.sent().back()->site==Startgg.id,"start.gg was not opened after Challonge's sign-in ended");
}

// On a service that offers Discord and a site, Discord's requests and rows
// come first and as before, and a tournament site's connect link (BluMint's
// one-link onboarding) asks nothing of the site on its way to Discord's page.
void Both(Journey& h){
 auto& id=h.view.identity;List(h,{Challonge},true);id.inspectedDiscord=id.inspectedDiscordAccounts=true;
}
void DiscordFirst(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 h.Screen("identity");
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==ember,"The Ember ID screen did not read Ember's service");
 Both(h);h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.count(IdentityOp::SiteStatus)==0,"Discord's account was not read first");
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(h.row("discord-connect")&&h.row("discord-connect")->value=="kate","Discord's account waited for the site's");
 Check(ReadsSite(h,Challonge),"The site's account was not read after Discord's");
 id.siteUser="1001";id.siteName="Kate_99";h.answer();
 Check(h.row(RowId(Challonge).c_str())&&h.row(RowId(Challonge).c_str())->value=="Kate_99","The site's account is not shown");
 id.discordUser.clear();id.discordName.clear();
 const auto siteReads=h.count(IdentityOp::SiteStatus);
 h.Screen("home");h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
 Check(h.until(IdentityOp::BridgeInspect),"The connect link did not read its service");
 Both(h);h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge=="brg_1","The connect link did not read Discord's account");
 h.answer();
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge=="brg_1","The connect link did not open Discord");
 Check(h.count(IdentityOp::SiteStatus)==siteReads,"The connect link asked about the site");
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
// A site's sign-in waiting for the browser.
void SiteWaiting(Journey& h,const Case& site){
 using sf4e::netplay::IdentityOp;auto& id=h.view.identity;
 OpenSite(h,site);
 id.siteUser.clear();id.siteName.clear();h.answer();
 h.Choose("st-connect");
 Check(h.sent().back()->op==IdentityOp::SiteConnect,"Connect did not ask the helper to open the site");
 h.answer();
 Check(h.row("st-waiting")!=nullptr,"The site is not waited for");
}

// One sign-in at a time between Discord and a site: starting one ends the
// other's on its service first, and the new one shows as being set up.
void OneSignInAtATime(){
 using namespace sf4e;using netplay::IdentityOp;
 {
  Journey h;Ready(h);h.untilLimit=12;
  DiscordWaiting(h);
  OpenSite(h,Challonge);h.answer();
  const auto before=h.sent().size();
  h.Choose("st-connect");
  Check(h.sent().size()==before+1&&h.sent().back()->op==IdentityOp::DiscordCancel&&h.sent().back()->bridge=="brg_1",
   "Connecting a site did not end Discord's sign-in first");
  Check(h.row("st-progress")&&!h.row("st-waiting"),"The site showed as waiting before its page opened");
  h.answer();
  Check(h.sent().back()->op==IdentityOp::SiteConnect,"The site was not opened after Discord's sign-in ended");
 }
 {
  Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
  SiteWaiting(h,Challonge);
  h.Screen("linked-accounts");
  Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not read the service");
  Both(h);h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"Linked accounts did not read Discord's account");
  id.discordUser.clear();id.discordName.clear();h.answer();for(int i=0;i<4;++i)h.answer();
  const auto before=h.sent().size();
  h.Choose("id-discord-connect");
  Check(h.sent().size()==before+1&&h.sent().back()->op==IdentityOp::SiteCancel&&h.sent().back()->site==Challonge.id,
   "Connecting Discord did not end the site's sign-in first");
  h.answer();
  Check(h.sent().back()->op==IdentityOp::DiscordConnect,"Discord was not opened after the site's sign-in ended");
 }
 {
  // Hiding Ember before the site's page was asked for sends nothing to the site.
  Journey h;Ready(h);h.untilLimit=12;
  DiscordWaiting(h);
  OpenSite(h,Challonge);h.answer();
  h.Choose("st-connect");
  Check(h.sent().back()->op==IdentityOp::DiscordCancel,"Connecting a site did not end Discord's sign-in first");
  Hidden(h);AnswerHidden(h);
  Check(h.count(IdentityOp::SiteConnect)==0&&h.count(IdentityOp::SiteCancel)==0,"Hiding Ember still opened the site");
 }
 {
  // A sign-in asked of the helper and then hidden still ends on the service.
  Journey h;Ready(h);
  OpenSite(h,Challonge);h.answer();
  h.Choose("st-connect");
  Check(h.sent().back()->op==IdentityOp::SiteConnect,"Connect did not ask the helper to open the site");
  Hidden(h);AnswerHidden(h);
  Check(h.sent().back()->op==IdentityOp::SiteCancel&&h.sent().back()->site==Challonge.id,"Hiding Ember lost the site's cancellation");
  Check(h.count(IdentityOp::SiteCancel)==1,"The site's cancellation was sent more than once");
 }
 {
  // A tournament site's link while a site's page is being opened: that
  // sign-in ends on the service, and the link runs as always.
  Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
  OpenSite(h,Challonge);h.answer();
  h.Choose("st-connect");
  Check(h.sent().back()->op==IdentityOp::SiteConnect,"Connect did not ask the helper to open the site");
  h.view.tournament.connect.bridge="brg_1";h.view.tournament.connect.sequence=1;h.Frame(0,2);
  h.answer();
  Check(h.sent().back()->op==IdentityOp::SiteCancel&&h.sent().back()->site==Challonge.id,"A connect link left the site's sign-in open");
  Check(h.until(IdentityOp::BridgeInspect),"The connect link did not read its service");
  Both(h);h.answer();
  Check(h.until(IdentityOp::DiscordStatus),"The connect link did not read Discord's account");
  id.discordUser.clear();id.discordName.clear();h.answer();
  Check(h.sent().back()->op==IdentityOp::DiscordConnect,"The connect link did not open Discord");
  Check(h.count(IdentityOp::SiteConnect)==1,"The site was opened again");
 }
}

// Discord's change of account keeps the wait already running as it always has.
void DiscordKeepsItsWait(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 DiscordWaiting(h);
 h.Screen("linked-accounts");
 Check(h.until(IdentityOp::BridgeInspect),"Linked accounts did not read the service");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;id.signInSites.clear();h.answer();
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

// Only starting another sign-in ends a site's: opening Connect Discord just
// to look leaves it waiting for the browser.
void LookingAtDiscordKeepsSite(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;h.untilLimit=12;
 SiteWaiting(h,Challonge);
 h.Screen("identity");for(int i=0;i<6;++i)h.answer();
 h.Choose(DiscordConnectScreen);
 Check(h.shell.Navigation().Screen()==DiscordConnectScreen,"The Discord row did not open Connect Discord");
 for(int i=0;i<6;++i)h.answer();
 Check(h.count(IdentityOp::SiteCancel)==0,"Opening Connect Discord ended the site's sign-in");
 h.Screen(SiteConnectScreen);for(int i=0;i<6;++i)h.answer();
 Check(h.row("st-waiting")&&h.row("st-cancel"),"The site's sign-in no longer waits after a look at Connect Discord");
 id.siteUser="1001";id.siteName="kate";h.Frame(0,300);
 Check(h.until(IdentityOp::SiteStatus),"The site's sign-in was not read again");
 h.answer();
 Check(h.status==Words("site.done",Challonge,"kate"),"The site's sign-in did not finish");
}

// A failed site read says nothing off its own screen, and Settings > Discord
// reads only Discord's account.
void SiteReadsQuietly(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;Ready(h);auto& id=h.view.identity;
 Tone tone=Tone::Neutral;
 SetMenuStatusProbe([&](const char* s,Tone t){h.status=s;tone=t;});
 h.Screen("identity");
 Check(h.until(IdentityOp::BridgeInspect),"The Ember ID screen did not read Ember's service");
 Both(h);h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"The Ember ID screen did not read Discord's account");
 id.discordUser="274220342558756145";id.discordName="kate";h.answer();
 Check(ReadsSite(h,Challonge),"The Ember ID screen did not read the site's account");
 h.answer(false,"service_unavailable");
 for(int i=0;i<120;++i){h.Frame();Check(tone!=Tone::Error,"A failed site read showed an error on the Ember ID screen");}
 Check(h.status.empty()||h.status==loc::T("identity.working"),"A failed site read left a status on the Ember ID screen");
 Check(h.row("discord-connect")&&h.row("discord-connect")->value=="kate","A failed site read changed Discord's row");
 Check(h.row(RowId(Challonge).c_str())&&h.row(RowId(Challonge).c_str())->value.empty(),"A failed site read still showed an account");
 for(int i=0;i<6;++i){h.answer();Check(tone!=Tone::Error,"A failed site read showed an error on the Ember ID screen");}
 h.Choose(RowId(Challonge).c_str());Profile(h,{Challonge});h.answer(false,"service_unavailable");
 Check(h.status==loc::T("identity.failure.unreachable")&&h.row("st-retry"),"A failed read on the site's Connect screen was not said");
 const auto siteReads=h.count(IdentityOp::SiteStatus);
 h.Screen("discord");
 Check(h.until(IdentityOp::BridgeInspect),"Settings > Discord did not read the service");
 Both(h);h.answer();
 Check(h.until(IdentityOp::DiscordStatus),"Settings > Discord did not read Discord's account");
 h.answer();for(int i=0;i<4;++i)h.answer();
 Check(h.count(IdentityOp::SiteStatus)==siteReads,"Settings > Discord read a site's account");
}
}
void RunSignInSiteJourneys(){
 for(const auto& site:{Challonge,Startgg,Other}){SiteRows(site);SiteConnects(site);SiteStops(site);SiteOffAndRetry(site);SiteBusy(site);SiteGone(site);}
 ManySites();LongSiteName();TwoSites();DiscordFirst();OneSignInAtATime();DiscordKeepsItsWait();LookingAtDiscordKeepsSite();SiteReadsQuietly();
}
