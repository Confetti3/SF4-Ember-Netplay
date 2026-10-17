// The tournament services built into this version of Ember: trusted from
// the start, dormant until the player uses one. Run from the identity
// journey test's main.
#include "identity_journey_support.hxx"
namespace {
const std::string staging="https://bridge-staging.embernetplay.link",staged="brg_00000000-0000-4000-8000-0000000000aa";
// A service built into this version of Ember arrives trusted in the helper's
// list but dormant: with only it, Online play, Home and the matches look as
// they do with no service, and nothing is read from it. Its connect link runs
// up to Discord's page with nothing trusted on the way, and that use makes it
// one of the player's services. The screens say it is built in.
void BuiltInServiceConnects(){
 using namespace sf4e;using netplay::IdentityOp;
 {
 // Beside Ember's own, a dormant one is not offered as a service.
 Journey h;auto& id=h.view.identity;id.known=true;h.ready();
 id.bridges={{"brg_1","https://bridge.embernetplay.link","Ember"},{staged,staging,"Ember bridge (staging)",true,false}};
 h.Screen("linked-accounts");for(int i=0;i<6;++i)h.answer();
 Check(h.row("id-bridge")&&h.row("id-bridge")->value=="Ember"&&h.row("id-bridge")->choices.size()==1,"A dormant built-in service is offered as one of the player's");
 }
 Journey h;auto& id=h.view.identity;id.known=true;h.ready();
 id.bridges={{staged,staging,"Ember bridge (staging)",true,false}};
 h.Screen("home");h.Frame(0,2);
 Check(h.until(IdentityOp::BridgeList),"Home did not learn the services");
 h.answer();h.Frame(0,300);
 Check(h.played().empty(),"Home read the matches of a service the player never used");
 h.Screen("online");
 Check(h.row("identity")&&h.row("identity")->detail==loc::T("home.identity_start"),"A dormant built-in service changed the Ember ID guide");
 h.Screen("tournament-matches");for(int i=0;i<6;++i)h.answer();
 Check(h.row("tm-no-service")&&!h.row("id-bridge"),"A dormant built-in service became the matches' service");
 Check(h.played().empty(),"The matches were read from a service the player never used");
 h.Screen("home");
 const auto start=h.sent().size();
 h.view.tournament.connect.bridge=staged;h.view.tournament.connect.sequence=1;h.Frame(0,2);
 Check(h.until(IdentityOp::BridgeInspect)&&h.sent().back()->origin==staging,"A connect link for a built-in service did not read it");
 id.inspected=id.bridges[0];id.inspectedDiscord=true;h.answer();
 Check(h.until(IdentityOp::DiscordStatus)&&h.sent().back()->bridge==staged,"A connect link for a built-in service did not read the account");
 Check(h.row("dc-service")&&h.row("dc-service")->detail==loc::Tf("identity.service_builtin",staging),"Connect Discord does not say the service is built in");
 id.discordUser.clear();id.discordName.clear();h.answer();
 Check(h.sent().back()->op==IdentityOp::DiscordConnect&&h.sent().back()->bridge==staged,"A connect link for a built-in service did not open Discord");
 Check(h.count(IdentityOp::BridgeApprove,start)==0&&!h.row("id-approve"),"A built-in service was trusted again");
 // The helper's answers made it active; the services are read again to say so.
 h.answer();
 Check(h.until(IdentityOp::BridgeList),"Using a built-in service did not read the services again");
 id.bridges[0].active=true;h.answer();
 h.Screen("linked-accounts");for(int i=0;i<8;++i)h.answer();
 Check(h.row("id-bridge")&&h.row("id-bridge")->detail==loc::Tf("identity.service_builtin",staging),"Linked accounts does not say the service is built in");
 h.Screen("online");
 Check(h.row("identity")&&h.row("identity")->detail==loc::T("home.identity_detail"),"A used built-in service is not one of the player's");
}
// A match link naming a dormant built-in service is a first use too. Here
// its links fail to load, so the match list read from it is the first answer,
// and that reads the services again.
void BuiltInServiceMatchLink(){
 using namespace sf4e;using netplay::IdentityOp;
 Journey h;auto& id=h.view.identity;id.known=true;h.ready();
 id.bridges={{staged,staging,"Ember bridge (staging)",true,false}};
 h.Screen("home");h.Frame(0,2);
 Check(h.until(IdentityOp::BridgeList),"Home did not learn the services");
 h.answer();
 auto& t=h.view.tournament;
 t.link.bridge=staged;t.link.match="emt_1";t.link.sequence=1;h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="tournament-matches","A match link for a built-in service did not open the matches");
 Check(h.until(IdentityOp::LinkList)&&h.sent().back()->bridge==staged,"A match link did not select its built-in service");
 h.answer(false,"rate_limited");
 Check(!h.played().empty()&&h.played().back()->bridgeId==staged,"A match link did not read the built-in service's matches");
 const auto before=h.sent().size();
 t.list.bridge=staged;t.list.loading=true;h.Frame(0,2);
 t.list.loading=false;h.Frame(0,2);
 Check(h.sent().size()>before&&h.sent().back()->op==IdentityOp::BridgeList,"A built-in service's first match list did not read the services again");
 id.bridges[0].active=true;h.answer();
 h.Screen("online");
 Check(h.row("identity")&&h.row("identity")->detail==loc::T("home.identity_detail"),"A built-in service used by a match link is not one of the player's");
}
}
void RunBuiltInServiceJourneys(){
 BuiltInServiceConnects();BuiltInServiceMatchLink();
}
