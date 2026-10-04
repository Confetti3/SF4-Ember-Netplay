// The public room journeys that set a player up or bring them in by a link:
// no Ember ID, the one-press setup (also while the Ember ID screens ask the
// helper for their own reads), room links from a bot, a site or the clipboard,
// and the room's own link copied inside it. Run from the public room journey
// test's main.
#include "public_rooms_journey_support.hxx"
namespace {
// Without a trusted Ember ID the screen says so and leads to the Ember ID
// screen; nothing is asked of the service.
void NoIdentityJourney(){
 {
  Journey j(false);auto& h=j.h;j.OpenList();
  Check(j.row("pr-setup")&&j.row("pr-setup")->confirm&&j.row("pr-setup")->wide&&j.row("pr-setup")->height==150&&j.row("pr-setup")->label==loc::T("public.setup")&&
   j.row("pr-setup")->detail==loc::T("public.setup_detail")&&j.row("identity")&&!j.row("pr-create")&&!j.row("pr-quick")&&!j.row("pr-needs-id")&&!j.row("pr-refresh"),
   "Public rooms did not offer to set up");
  Check(j.requests().empty(),"A service was asked for rooms without an Ember ID");
  // Setup asks first, and Cancel is the answer it starts on (SetupJourney runs the confirmed one).
  h.Choose("pr-setup");Check(h.shell.Navigation().Confirming()&&h.shell.Navigation().Screen()=="public-rooms","Set up public rooms did not ask first");
  h.Press(MenuInput::Select);
  Check(!h.shell.Navigation().Confirming()&&h.shell.Navigation().Screen()=="public-rooms"&&j.requests().empty(),"Cancelling the setup did something");
  h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","The Ember ID row did not open the Ember ID screen");
 }
 {
  // A ready ID without a trusted service offers the same setup.
  Journey k;k.h.view.identity.bridges.clear();k.OpenList();
  Check(k.row("pr-setup")&&k.requests().empty(),"An ID without a trusted service was offered the list");
 }
 {
  // A locked ID, or one behind a passphrase, must be unlocked on its own screen first.
  Journey locked(false);locked.h.view.identity.state="locked";locked.OpenList();
  Check(locked.row("pr-setup-id")&&!locked.row("pr-setup-id")->confirm&&locked.row("pr-setup-id")->label==loc::T("public.setup_needs_id")&&
   locked.row("pr-setup-id")->detail==loc::T("public.setup_needs_id_detail")&&!locked.row("pr-setup")&&locked.row("identity"),"A locked ID was offered the one-press setup");
  locked.h.Choose("pr-setup-id");Check(locked.h.shell.Navigation().Screen()=="identity","Unlock your Ember ID did not open the Ember ID screen");
 }
 Journey passphrase(false);passphrase.h.view.identity.passphraseRequired=true;passphrase.OpenList();
 Check(passphrase.row("pr-setup-id")&&!passphrase.row("pr-setup"),"A disabled ID with a passphrase was offered the one-press setup");
}

// The one-press setup: a confirmed press (Cancel is where the dialog starts) reads the
// Ember ID, creates it when nothing must be typed, finds Ember's own service and
// trusts it, then the list is asked for. Each step is the card's line, a failure its
// text with Try again, and an ID that needs the player goes to its own screen.
const char* const EmberOrigin="https://bridge.embernetplay.link";
// The helper refuses the newest request outright.
void RefuseIdentity(Journey& j,const char* reason){
 auto& h=j.h;h.view.identityTicket=j.identity().back()->ticket;h.view.identityRequest=0;h.view.identityRefusal=reason;h.Frame(0,2);
}
// The helper answers the newest request with a failure code.
void FailIdentity(Journey& j,const char* code){
 auto& h=j.h;auto& id=h.view.identity;h.view.identityTicket=j.identity().back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;
 id.ok=false;id.failure=code;h.Frame(0,2);id.ok=true;id.failure.clear();
}
std::size_t Asked(const Journey& j,IdentityOp op,std::size_t from=0){
 std::size_t n=0;const auto all=j.identity();for(std::size_t i=from;i<all.size();++i)n+=all[i]->op==op;return n;
}
// Presses the setup card and answers its dialog with Confirm.
void ConfirmSetup(Journey& j){
 auto& h=j.h;h.Choose("pr-setup");
 Check(h.shell.Navigation().Confirming()&&h.shell.Navigation().Screen()=="public-rooms","Set up public rooms did not ask first");
 h.Press(MenuInput::Right);h.Press(MenuInput::Select);
}
void SetupJourney(){
 {
  // No Ember ID: Cancel is the default answer, then the whole sequence from one confirmed press.
  Journey j(false);auto& h=j.h;auto& id=h.view.identity;j.OpenList();
  const auto asked=j.identity().size();
  h.Choose("pr-setup");Check(h.shell.Navigation().Confirming(),"Set up public rooms did not ask first");
  h.Press(MenuInput::Select);
  Check(!h.shell.Navigation().Confirming()&&j.identity().size()==asked&&j.row("pr-setup")&&j.row("pr-setup")->enabled&&j.row("pr-setup")->value.empty(),
   "Select on the dialog's first answer started the setup");
  ConfirmSetup(j);
  Check(j.identity().size()==asked+1&&j.identity().back()->op==IdentityOp::Status,"The confirmed setup did not read the Ember ID");
  Check(j.row("pr-setup")&&!j.row("pr-setup")->enabled&&j.row("pr-setup")->value==loc::T("public.setup_step.checking")&&!j.row("pr-checking"),"The setup does not show its first step");
  // While it runs the card cannot be pressed again.
  h.FocusOn("pr-setup");h.Press(MenuInput::Select);
  Check(!h.shell.Navigation().Confirming()&&j.identity().size()==asked+1,"A running setup could be started again");
  j.answerIdentity();
  Check(j.identity().back()->op==IdentityOp::Enable&&j.row("pr-setup")->value==loc::T("public.setup_step.creating"),"A disabled ID was not created");
  id.state="ready";h.view.identity.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
  j.answerIdentity();
  Check(j.identity().back()->op==IdentityOp::BridgeList&&j.row("pr-setup")->value==loc::T("public.setup_step.finding"),"The created ID's services were not read");
  j.answerIdentity();
  Check(j.identity().back()->op==IdentityOp::BridgeInspect&&j.identity().back()->origin==EmberOrigin,"Ember's own service was not looked up");
  Check(j.row("pr-setup")->value==loc::T("public.setup_step.finding"),"The look-up is not shown as finding");
  id.inspected={"brg_9",EmberOrigin,"Ember"};j.answerIdentity();
  Check(j.identity().back()->op==IdentityOp::BridgeApprove&&j.identity().back()->origin==EmberOrigin&&j.identity().back()->bridge=="brg_9"&&
   j.row("pr-setup")->value==loc::T("public.setup_step.trusting"),"Ember's own service was not trusted");
  j.answerIdentity();
  Check(j.identity().back()->op==IdentityOp::BridgeList,"The trusted service was not read back");
  id.bridges={{"brg_9",EmberOrigin,"Ember"}};
  const auto lists=j.lists();
  j.answerIdentity();
  Check(j.status==loc::T("public.setup_done")&&!j.row("pr-setup")&&j.row("pr-quick")&&j.lists()==lists+1&&j.requests().back()->op==Op::RoomList&&j.requests().back()->bridgeId=="brg_9",
   "A finished setup did not say so and ask for the list");
  Check(Asked(j,IdentityOp::Enable)==1&&Asked(j,IdentityOp::BridgeApprove)==1,"The setup asked for more than it needed");
 }
 {
  // An ID that is ready skips creating it, and Ember's service already trusted skips the look-up.
  Journey j;auto& h=j.h;auto& id=h.view.identity;id.bridges.clear();j.OpenList();
  Check(j.row("pr-setup"),"A ready ID without a trusted service was not offered the setup");
  const auto from=j.identity().size();
  ConfirmSetup(j);j.answerIdentity();
  Check(j.identity().back()->op==IdentityOp::BridgeList&&Asked(j,IdentityOp::Enable,from)==0,"A ready ID was asked to be created");
  id.bridges={{"brg_9",EmberOrigin,"Ember"}};j.answerIdentity();
  Check(j.status==loc::T("public.setup_done")&&Asked(j,IdentityOp::BridgeInspect,from)==0&&Asked(j,IdentityOp::BridgeApprove,from)==0&&j.row("pr-quick"),
   "A trusted Ember service was looked up again");
 }
 {
  // A passphrase (Wine and Proton) or a locked ID is the player's to type on the Ember ID screen.
  Journey j(false);auto& h=j.h;j.OpenList();
  const auto from=j.identity().size();
  ConfirmSetup(j);h.view.identity.passphraseRequired=true;j.answerIdentity();
  Check(j.row("pr-setup-id")&&!j.row("pr-setup")&&Asked(j,IdentityOp::Enable,from)==0,"An ID that needs a passphrase was created by the setup");
  h.Choose("pr-setup-id");
  Check(h.shell.Navigation().Screen()=="identity","The unlock card did not open the Ember ID screen");
 }
 {
  Journey k(false);k.h.view.identity.state="locked";k.OpenList();
  Check(k.row("pr-setup-id")&&!k.row("pr-setup"),"A locked ID was offered the one-press setup");
 }
 {
  // A service that answers as something other than Ember's own is not trusted.
  Journey j;auto& h=j.h;auto& id=h.view.identity;id.bridges.clear();j.OpenList();
  const auto from=j.identity().size();
  ConfirmSetup(j);j.answerIdentity();j.answerIdentity();
  Check(j.identity().back()->op==IdentityOp::BridgeInspect,"No look-up was asked");
  id.inspected={"brg_7","https://other.example","Other"};j.answerIdentity();
  Check(Asked(j,IdentityOp::BridgeApprove,from)==0,"A service at another address was trusted by the setup");
  Check(j.row("pr-setup")&&j.row("pr-setup")->enabled&&!j.row("pr-setup")->confirm&&j.row("pr-setup")->label==loc::T("public.setup_retry")&&
   j.row("pr-setup")->value==loc::T("public.failure.rooms_unavailable"),"A wrong look-up did not fail with its words on the card");
  // Try again starts over, without asking a second time, and a refusal from the helper is the card's text too.
  const auto asked=j.identity().size();
  h.Choose("pr-setup");
  Check(!h.shell.Navigation().Confirming()&&j.identity().size()==asked+1&&j.identity().back()->op==IdentityOp::Status&&
   j.row("pr-setup")->value==loc::T("public.setup_step.checking")&&!j.row("pr-setup")->enabled,"Try again did not restart the setup");
  RefuseIdentity(j,"identity.refused.match");
  Check(j.row("pr-setup")->enabled&&j.row("pr-setup")->value==loc::T("identity.refused.match")&&j.row("pr-setup")->label==loc::T("public.setup_retry"),"A refused request did not show on the card");
  h.Choose("pr-setup");FailIdentity(j,"bridge_unreachable");
  Check(j.row("pr-setup")->value==loc::T("public.failure.unavailable")&&j.row("pr-setup")->enabled,"A helper failure did not show on the card");
  // An answer that never comes fails the setup too.
  h.Choose("pr-setup");h.Wait(130);h.Frame(0,2);
  Check(j.row("pr-setup")&&j.row("pr-setup")->enabled&&j.row("pr-setup")->value==loc::T("identity.failure.timeout"),"A setup that is never answered did not time out");
  // Leaving the screen drops the failure: the next visit starts fresh.
  h.Screen("home");h.Frame(0,2);h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);j.answerIdentityUntilIdle();
  Check(j.row("pr-setup")&&j.row("pr-setup")->confirm&&j.row("pr-setup")->value.empty(),"A failed setup came back on the next visit");
 }
 {
  // Hiding Ember mid-setup ends it: the answer still arrives but starts nothing.
  Journey j(false);auto& h=j.h;j.OpenList();
  const auto from=j.identity().size();
  ConfirmSetup(j);Check(Asked(j,IdentityOp::Status,from)==1,"The setup did not start");
  h.shell.Conceal();h.Frame(0,2);
  j.answerIdentityUntilIdle();
  Check(Asked(j,IdentityOp::Enable,from)==0,"A setup that was hidden went on to create the ID");
  Check(j.row("pr-setup")&&j.row("pr-setup")->enabled&&j.row("pr-setup")->value.empty(),"A hidden setup did not reset");
 }
}

// The setup's requests and answers are its own. The player opens Linked accounts
// while its look-up is out: that screen's own status and service list go first, and
// its list, still without Ember's service, answers before the setup's trust. The
// setup still finishes, and the screen offers nothing of the setup's look-up.
void SetupBesideScreensJourney(){
 Journey j;auto& h=j.h;auto& id=h.view.identity;id.bridges.clear();j.OpenList();
 const auto from=j.identity().size();
 ConfirmSetup(j);j.answerIdentity();
 Check(j.identity().back()->op==IdentityOp::BridgeList,"The setup did not read the services");
 j.answerIdentity();
 Check(j.identity().back()->op==IdentityOp::BridgeInspect&&j.identity().back()->origin==EmberOrigin,"Ember's own service was not looked up");
 h.Screen("identity");h.Choose("linked-accounts");
 Check(h.shell.Navigation().Screen()=="linked-accounts","Linked accounts did not open");
 id.inspected={"brg_9",EmberOrigin,"Ember"};j.answerIdentity();
 std::size_t lists=0;
 for(int i=0;i<8&&j.identity().back()->op!=IdentityOp::BridgeApprove;++i){lists+=j.identity().back()->op==IdentityOp::BridgeList;j.answerIdentity();}
 Check(j.identity().back()->op==IdentityOp::BridgeApprove&&j.identity().back()->origin==EmberOrigin&&j.identity().back()->bridge=="brg_9",
  "The setup's trust was not asked");
 Check(lists>0,"Linked accounts' service list did not answer before the setup's trust");
 Check(!j.row("id-approve"),"Linked accounts offered the setup's look-up to trust");
 j.answerIdentity();
 Check(j.identity().back()->op==IdentityOp::BridgeList,"The trusted service was not read back");
 id.bridges={{"brg_9",EmberOrigin,"Ember"}};j.answerIdentity();
 Check(Asked(j,IdentityOp::BridgeApprove,from)==1,"The setup asked to trust more than once");
 // Back on Public rooms the service is set up, and the list is asked of it.
 const auto asked=j.lists();
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 Check(!j.row("pr-setup")&&j.row("pr-quick")&&j.lists()==asked+1&&j.requests().back()->op==Op::RoomList&&j.requests().back()->bridgeId=="brg_9",
  "A setup that ran beside Linked accounts did not finish");
}

// The room links a bot or site hands out (ember://room/open, or the /r page's
// own link). A link names a service and a room, and asks for that room's ticket
// through the request a pressed room sends, so the admission, the refusals and
// the join are the list's own.

// A link at the menus opens Public rooms on the link's service, waits for the
// Ember ID to answer, asks that service for that room's ticket, and the
// admission joins; the list that follows is the link's service's.
void LinkJourney(){
 Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"},{OtherBridge,"https://other.example","Other"}};
 h.Frame(0,2);h.Screen("home");
 OpenLink(j,OtherBridge,LinkRoom);
 Check(h.shell.Navigation().Screen()=="public-rooms","A room link did not open Public rooms");
 Check(tickets(j)==0,"A room link asked for its ticket before the Ember ID answered");
 j.answerIdentityUntilIdle();
 Check(tickets(j)==1&&lastTicketIs(j,OtherBridge,LinkRoom),"A room link did not ask its service for its room's ticket");
 Check(j.status==loc::T("room.joining_status")&&j.joins()==0,"A room link joined before its admission arrived");
 j.Admit("",LinkRoom);
 Check(j.joins()==1&&h.actions.back().command.invitation=="sf4e3:host-invitation"&&!h.actions.back().publicTicket.empty()&&
  h.actions.back().command.generation==h.view.session.generation,"The admission of a linked room did not join with its invitation and ticket");
 h.Frame(0,5);Check(j.joins()==1&&tickets(j)==1,"A room link was followed twice");
 // The list on screen is the link's service's, not the selected one's.
 const auto listed=[&]{for(const auto* r:j.requests())if(r->op==Op::RoomList)return r->bridgeId;return std::string();};
 Check(listed()==OtherBridge,"Public rooms listed the selected service after a link named another");
 // Pressing Refresh keeps asking the link's service.
 h.Press(MenuInput::Options);Check(j.requests().back()->op==Op::RoomList&&j.requests().back()->bridgeId==OtherBridge,"Refresh left the link's service");
 // Leaving Public rooms returns to the selected service.
 h.Screen("home");h.Choose("online");h.Choose("public-rooms");h.Frame(0,2);
 Check(j.requests().back()->op==Op::RoomList&&j.requests().back()->bridgeId==LinkBridge,"The link's service stayed selected after leaving Public rooms");
}

// A refused room is said in words and joins nothing, and the list stays usable;
// the same link can be followed again.
void LinkRefusalJourney(){
 Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};h.Frame(0,2);
 OpenLink(j,LinkBridge,LinkRoom);j.answerIdentityUntilIdle();
 Check(tickets(j)==1,"A room link did not ask for its ticket");
 const std::pair<const char*,const char*> refusals[]={
  {"room_full","public.failure.room_full"},{"banned","public.failure.banned"},{"room_not_found","public.failure.room_not_found"},
  {"room_not_open","public.failure.room_not_open"},{"unsupported_build","public.failure.unsupported_build"}};
 for(const auto& refusal:refusals){
  j.Admit(refusal.first);
  Check(j.status==loc::T(refusal.second),"A refused room link did not say why");
  Check(j.joins()==0&&j.row("pr-create")&&j.row("pr-create")->enabled,"A refused room link left Public rooms blocked");
  OpenLink(j,LinkBridge,LinkRoom);j.answerIdentityUntilIdle();
 }
 Check(j.joins()==0,"A refused room link joined");
 j.Admit();Check(j.joins()==1,"A link followed again did not join");
}

// A link for a service the player does not trust says so and sends nothing; a
// link never makes a service trusted. Without an Ember ID it says what is needed.
void LinkUntrustedJourney(){
 {
  Journey j;auto& h=j.h;
  OpenLink(j,OtherBridge,LinkRoom);j.answerIdentityUntilIdle();
  Check(h.shell.Navigation().Screen()=="public-rooms"&&j.status==loc::T("public.failure.link_service"),"A link to an untrusted service did not say so");
  Check(tickets(j)==0&&j.joins()==0,"A link to an untrusted service asked for a ticket or joined");
  h.Frame(0,10);Check(tickets(j)==0&&j.joins()==0,"A link to an untrusted service went on later");
 }
 Journey k(false);
 OpenLink(k,LinkBridge,LinkRoom);k.answerIdentityUntilIdle();
 Check(k.status==loc::T("public.needs_id_detail")&&k.requests().empty()&&k.joins()==0,"A link without an Ember ID did not say what is needed");
}

// A link that arrives in a room or a game moves no one: it says so, and waits as
// a row on Public rooms for the player's choice.
void LinkWaitsJourney(){
 Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};h.Frame(0,2);
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;h.Screen("home");
 OpenLink(j,LinkBridge,LinkRoom,false);
 Check(h.shell.Navigation().Screen()!="public-rooms","A room link moved a player who is in a room");
 Check(j.status==loc::T("public.link_waiting"),"A room link in a room did not say it waits");
 Check(tickets(j)==0&&j.joins()==0,"A room link in a room asked for a ticket");
 h.Frame(0,10);Check(h.shell.Navigation().Screen()!="public-rooms"&&tickets(j)==0,"A waiting room link acted by itself");
 // In a game with no room, too.
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.view.session.match=netplay::MatchState::Playing;
 OpenLink(j,LinkBridge,LinkRoom,false);
 Check(h.shell.Navigation().Screen()!="public-rooms"&&j.status==loc::T("public.link_waiting")&&tickets(j)==0,"A room link in a game moved the player or asked for a ticket");
 // Free again: Public rooms offers the link as a row, and choosing it asks.
 h.view.session.match=netplay::MatchState::None;h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);j.answerIdentityUntilIdle();
 Check(tickets(j)==0,"A waiting room link asked for a ticket when Public rooms opened");
 Check(j.row("pr-link")&&j.row("pr-link")->enabled,"A waiting room link is not offered on Public rooms");
 h.Choose("pr-link");h.Frame(0,2);
 Check(tickets(j)==1&&lastTicketIs(j,LinkBridge,LinkRoom),"Choosing the waiting link did not ask for its room");
 j.Admit("",LinkRoom);Check(j.joins()==1,"The waiting link's admission did not join");
}

// Hiding Ember abandons a ticket request in flight, but a link nobody has
// followed yet stays the player's: a link that waited for them, one that waits
// behind a dialog, and one still waiting for the Ember ID all come back.
void LinkSurvivesHidingJourney(){
 {
  Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};h.Frame(0,2);
  h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;h.Screen("home");
  OpenLink(j,LinkBridge,LinkRoom,false);
  Check(j.status==loc::T("public.link_waiting"),"A room link in a room did not say it waits");
  h.shell.Conceal();h.Frame(0,2);h.shell.Conceal();
  h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;
  h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);j.answerIdentityUntilIdle();
  Check(tickets(j)==0&&j.row("pr-link")&&j.row("pr-link")->enabled,"Hiding Ember lost a room link that waited for the player");
  h.Choose("pr-link");h.Frame(0,2);
  Check(tickets(j)==1&&lastTicketIs(j,LinkBridge,LinkRoom),"The kept room link did not ask for its room");
  // A request in flight is abandoned by hiding, though: its admission does not join.
  h.shell.Conceal();j.Admit("",LinkRoom);
  Check(j.joins()==0,"An admission that arrived while Ember was hidden joined");
 }
 {
  // Behind a dialog: the link goes on once it is closed, though Ember was hidden meanwhile.
  Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};h.Frame(0,2);h.Screen("home");
  h.view.readyFailure="Something to read";++h.view.readyFailureSequence;h.Frame(0,2);
  OpenLink(j,LinkBridge,LinkRoom);
  Check(h.shell.Navigation().Screen()=="home"&&tickets(j)==0,"A room link moved the player over a dialog");
  h.shell.Conceal();h.Frame(0,2);h.shell.Conceal();h.Frame(0,2);
  Check(h.shell.Navigation().Screen()=="home"&&tickets(j)==0,"A room link went on over a dialog after Ember was hidden");
  h.view.readyFailure.clear();h.Press(MenuInput::Select);h.Frame(0,4);j.answerIdentityUntilIdle();
  Check(h.shell.Navigation().Screen()=="public-rooms"&&tickets(j)==1&&lastTicketIs(j,LinkBridge,LinkRoom),("Hiding Ember lost a room link that waited behind a dialog "+h.shell.Navigation().Screen()+std::to_string(tickets(j))).c_str());
 }
 {
  // Hidden while the Ember ID was still being asked: it is asked again and the link goes on.
  Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};h.Frame(0,2);h.Screen("home");
  OpenLink(j,LinkBridge,LinkRoom);
  Check(h.shell.Navigation().Screen()=="public-rooms"&&tickets(j)==0,"A room link did not wait for the Ember ID");
  h.shell.Conceal();h.Frame(0,2);h.shell.Conceal();
  h.Frame(0,4);j.answerIdentityUntilIdle();
  Check(tickets(j)==1&&lastTicketIs(j,LinkBridge,LinkRoom),"Hiding Ember lost a room link that waited for the Ember ID");
  }
  {
  // Busy by the time Ember returns: it waits as a row and moves no one.
  Journey k;auto& g=k.h;g.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};g.Frame(0,2);g.Screen("home");
  g.view.session.room=netplay::RoomState::Joined;g.view.session.generation.room=1;g.Frame(0,2);
  g.view.session.room=netplay::RoomState::Idle;g.view.session.generation.room=0;g.Frame(0,2);
  OpenLink(k,LinkBridge,LinkRoom);g.shell.Conceal();g.view.session.room=netplay::RoomState::Joined;g.view.session.generation.room=1;
  g.Frame(0,4);
  Check(tickets(k)==0,"A room link asked for a ticket for a player who became busy while Ember was hidden");
 }
}

// Pasting a room link: either form, with spaces or quotes around it; something
// else says it is no room link, and a service not trusted is refused.
void PasteJourney(){
 Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};h.Frame(0,2);
 j.OpenList();
 Check(j.row("pr-paste")&&j.row("pr-paste")->enabled&&j.row("pr-paste")->hint==loc::T("menu.hint.paste"),"Public rooms has no Paste room link row");
 Clipboard("hello");h.Choose("pr-paste");h.Frame(0,2);
 Check(j.status==loc::T("public.failure.link")&&tickets(j)==0,"Pasting something else did not say it is no room link");
 Clipboard(std::string("ember://tournament/open?bridge=")+LinkBridge+"&match=emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12");
 h.Choose("pr-paste");h.Frame(0,2);
 Check(j.status==loc::T("public.failure.link")&&tickets(j)==0,"A match link was taken for a room link");
 Clipboard(std::string("  \"https://embernetplay.link/r#")+LinkBridge+"/"+LinkRoom+"\" ");
 h.Choose("pr-paste");h.Frame(0,2);
 Check(tickets(j)==1&&lastTicketIs(j,LinkBridge,LinkRoom),"The pasted page link did not ask for its room");
 j.Admit("room_full");
 Check(j.status==loc::T("public.failure.room_full")&&j.joins()==0,"A pasted link's refusal was not said");
 Clipboard(std::string("ember://room/open?room=")+LinkRoom+"&bridge="+LinkBridge);
 h.Choose("pr-paste");h.Frame(0,2);
 Check(tickets(j)==2&&lastTicketIs(j,LinkBridge,LinkRoom),"The pasted ember: link did not ask for its room");
 j.Admit();Check(j.joins()==1&&h.actions.back().command.invitation=="sf4e3:host-invitation","The pasted link's admission did not join");
 // An untrusted service, pasted: said, and nothing asked.
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 Clipboard(std::string("ember://room/open?bridge=")+OtherBridge+"&room="+LinkRoom);
 const auto asked=tickets(j);h.Choose("pr-paste");h.Frame(0,2);
 Check(j.status==loc::T("public.failure.link_service")&&tickets(j)==asked,"A pasted link to an untrusted service asked for a ticket");
 ImGui::GetPlatformIO().Platform_GetClipboardTextFn=nullptr;
}

// Inside a public room the board offers the room's page link: copying it puts the
// link on the clipboard and says so. A private room, a public room this PC did not
// join through the list, and a room that has ended offer none.
void CopyLinkJourney(){
 Journey j;auto& h=j.h;h.view.identity.bridges={{LinkBridge,"https://bridge.example","Example"}};h.Frame(0,2);
 auto& platform=ImGui::GetPlatformIO();
 platform.Platform_SetClipboardTextFn=[](ImGuiContext*,const char* text){g_clipboard=text?text:"";};
 platform.Platform_GetClipboardTextFn=[](ImGuiContext*){return g_clipboard.c_str();};
 const std::string link=tournament_link::RoomPageUrl(LinkBridge,LinkRoom);
 Check(!link.empty(),"The test's room has no link");
 j.OpenList();
 {auto& list=h.view.publicRooms;list.bridge=LinkBridge;list.rooms={MakeRoom(LinkRoom,"Open Mic",2,4)};list.loading=false;list.error.clear();++list.listed;h.Frame(0,2);}
 h.Choose((std::string("pr-room:")+LinkRoom).c_str());
 j.Admit("",LinkRoom);
 h.view.session.room=netplay::RoomState::Opening;h.view.session.generation.room=1;h.Frame(0,2);
 h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Open Mic";h.view.room.revision=3;h.view.room.serverOwned=true;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 h.Screen("room");h.Frame(0,2);
 Check(j.row("copy-room-link")&&j.row("copy-room-link")->enabled&&j.row("copy-room-link")->label==loc::T("public.copy_link")&&
  j.row("copy-room-link")->detail==loc::Tf("public.copy_link_detail",link),"A public room joined from the list offers no room link");
 Check(!j.row("copy")&&!j.row("copy-short")&&j.row("leave"),"A public room offered an invitation to copy");
 g_clipboard="before";const auto actions=h.actions.size();
 h.Choose("copy-room-link");
 Check(g_clipboard==link,"Copy room link did not put the page link on the clipboard");
 Check(j.status==loc::T("public.link_copied"),"Copy room link did not say it copied");
 Check(h.actions.size()==actions,"Copy room link sent a command");
 const auto round=tournament_link::ParseRoomPasted(g_clipboard);
 Check(round.bridgeId==LinkBridge&&round.roomId==LinkRoom,"The copied link does not read back as the room");
 // A private room has no such row.
 h.view.room.serverOwned=false;h.Frame(0,2);
 Check(!j.row("copy-room-link")&&j.row("copy"),"A private room offered a room link");
 h.view.room.serverOwned=true;h.Frame(0,2);Check(j.row("copy-room-link"),"The room link did not come back");
 // The room ends: the link goes with it.
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.view.room=room::Snapshot{};h.Frame(0,3);
 h.view.session.room=netplay::RoomState::Joined;h.view.room.roomEpoch=11;h.view.room.localMember=1;h.view.room.host=1;h.view.room.serverOwned=true;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 h.view.room.members={local};h.Screen("room");h.Frame(0,2);
 Check(!j.row("copy-room-link"),"A public room this PC did not join from the list offered a room link");
 platform.Platform_SetClipboardTextFn=nullptr;platform.Platform_GetClipboardTextFn=nullptr;
}
}
void RunPublicRoomsSetupJourneys(){
 NoIdentityJourney();SetupJourney();SetupBesideScreensJourney();LinkJourney();LinkRefusalJourney();LinkUntrustedJourney();LinkWaitsJourney();LinkSurvivesHidingJourney();PasteJourney();CopyLinkJourney();
}
