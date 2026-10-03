// The public room journeys: the list a player opens from Online play, a ticket
// for the room they press, the join that follows its admission, a refusal in
// words, and Create on Public. The runtime's answers are the view's fields.
#include "shell_journey_support.hxx"
namespace {
using namespace sf4e;
using netplay::IdentityOp;
using netplay::tournament::Command;
using Op = Command::Op;

// The harness with the menu's rows and status captured, and an Ember ID whose
// service the player trusts, or not.
struct Journey {
 Harness h;
 std::vector<MenuEntry> rows;
 std::string status;
 explicit Journey(bool trusted=true){
  auto& id=h.view.identity;id.known=true;id.state=trusted?"ready":"disabled";
  if(trusted){id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
   id.bridges={{"brg_1","https://bridge.example","Example"}};}
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  h.Frame();
 }
 ~Journey(){SetMenuEntriesProbe({});SetMenuStatusProbe({});}
 const MenuEntry* row(const std::string& name) const{
  const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;
 }
 std::vector<const netplay::IdentityRequest*> identity() const{
  std::vector<const netplay::IdentityRequest*> out;for(const auto& a:h.actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;
 }
 // The public room requests sent so far.
 std::vector<const Command*> requests() const{
  std::vector<const Command*> out;
  for(const auto& a:h.actions)if(a.tournament.op==Op::RoomList||a.tournament.op==Op::RoomCreate||a.tournament.op==Op::RoomTicket)out.push_back(&a.tournament);
  return out;
 }
 // Private rooms hosted: ShellAction's kind starts as HostRoom, so the other
 // requests it can carry say it was not one.
 std::size_t hosts() const{
  return static_cast<std::size_t>(std::count_if(h.actions.begin(),h.actions.end(),[](const ShellAction& a){
   return a.command.kind==Kind::HostRoom&&a.identity.op==IdentityOp::None&&a.tournament.op==Op::None&&a.inputAction==input::Action::None&&
    a.service==platform::ServiceAction::None&&a.discordAction==discord::InviteAction::None&&!a.shortInvitation&&a.previewSoundVolume<0&&
    a.roomAction.kind==room::Action().kind&&a.selectedDelay<0;}));
 }
 std::size_t joins() const{
  return static_cast<std::size_t>(std::count_if(h.actions.begin(),h.actions.end(),[](const ShellAction& a){return a.command.kind==Kind::JoinInvite;}));
 }
 // The helper answers the newest Ember ID request.
 void answerIdentity(){
  const auto sent=identity();Check(!sent.empty(),"No Ember ID request to answer");
  auto& id=h.view.identity;h.view.identityTicket=sent.back()->ticket;h.view.identityRequest=id.requestId=h.view.identityTicket+100;
  id.ok=true;id.failure.clear();h.Frame(0,2);
 }
 void answerIdentityUntilIdle(){
  for(int i=0;i<8;++i)answerIdentity();
 }
 // Opens Public rooms from Online play and lets the Ember ID answer.
 void OpenList(){
  h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
  Check(h.shell.Navigation().Screen()=="public-rooms","Online play does not open Public rooms");
  answerIdentityUntilIdle();
 }
 // The runtime has the list: the rooms in the order received.
 void List(const std::vector<netplay::publicrooms::Room>& rooms){
  auto& list=h.view.publicRooms;list.bridge="brg_1";list.rooms=rooms;list.loading=false;list.error.clear();++list.listed;h.Frame(0,2);
 }
 // The identity of the newest create or ticket request sent, 0 when none.
 std::uint64_t lastAdmission() const{
  std::uint64_t id=0;
  for(const auto& a:h.actions)if(a.tournament.op==Op::RoomCreate||a.tournament.op==Op::RoomTicket)id=a.tournament.request;
  return id;
 }
 // The runtime has answered the create or ticket request with that identity.
 void AdmitAs(std::uint64_t request,const char* failure="",const char* roomId="b",const char* invitation="sf4e3:host-invitation",
  const char* ticket="{\"ticket\":{\"room_id\":\"b\"},\"kid\":\"k1\",\"signature\":\"sig\"}"){
  auto& list=h.view.publicRooms;list.failure=failure;
  list.admission=*failure?netplay::publicrooms::Admission{}:netplay::publicrooms::Admission{{roomId,"Open Mic","use1",3,8,0},invitation,ticket};
  list.admitting=0;list.answered=request;h.Frame(0,2);
 }
 // ... the newest one sent.
 void Admit(const char* failure="",const char* roomId="b"){AdmitAs(lastAdmission(),failure,roomId);}
};

netplay::publicrooms::Room MakeRoom(const char* id,const char* name,unsigned members,unsigned capacity,unsigned playing=0){
 netplay::publicrooms::Room room;room.id=id;room.name=name;room.members=members;room.capacity=capacity;room.playing=playing;room.region="use1";return room;
}

// Online play offers Public rooms; opening it waits for the Ember ID's services
// and then asks the service for the list; rooms show their name, players and
// whether a match is on; an empty list and a failed one say so.
void ListJourney(){
 Journey j;auto& h=j.h;
 h.Screen("online");Check(j.row("public-rooms")&&j.row("public-rooms")->enabled,"Online play does not offer Public rooms");
 Check(j.row("relay")&&j.row("relay")->info&&j.row("network")&&j.row("network")->info,"The route rows lost their information role");
 h.Choose("public-rooms");h.Frame(0,2);
 // Home already asks for the trusted services in the background; Public rooms
 // waits for that answer rather than asking again.
 Check(j.identity().size()==1&&j.identity().back()->op==IdentityOp::BridgeList,"Opening Public rooms did not wait for the Ember ID's services");
 Check(j.row("pr-checking")&&!j.row("pr-create"),"A list was offered before the Ember ID answered");
 Check(j.requests().empty(),"The list was asked for before a service was known");
 j.answerIdentityUntilIdle();
 Check(j.requests().size()==1&&j.requests().back()->op==Op::RoomList&&j.requests().back()->bridgeId=="brg_1",
  "Opening Public rooms did not ask the trusted service for the list");
 h.view.publicRooms.bridge="brg_1";h.view.publicRooms.loading=true;h.Frame();
 Check(j.status==loc::T("public.loading"),"A refresh in flight is not shown");
 Check(j.row("pr-refresh")&&!j.row("pr-refresh")->enabled,"Refresh is offered while a refresh is in flight");
 j.List({MakeRoom("a","Friday Night",3,8),MakeRoom("b","金曜ルーム",2,4,1),MakeRoom("c","Full House",4,4)});
 Check(j.row("pr-create")&&j.row("pr-refresh")&&j.row("pr-refresh")->enabled,"The list lacks Create or Refresh");
 // Create and Refresh first, then the rooms as received.
 std::vector<std::string> order;for(const auto& r:j.rows)order.push_back(r.id);
 Check(order==std::vector<std::string>({"pr-create","pr-refresh","pr-room:a","pr-room:b","pr-room:c"}),"The list is not Create, Refresh and then the rooms as received");
 const auto* friday=j.row("pr-room:a");const auto* match=j.row("pr-room:b");
 Check(friday->label=="Friday Night"&&friday->userText&&friday->value=="3/8"&&friday->hint==loc::T("online.join"),"A room does not show its name, players and Join");
 Check(match->label=="金曜ルーム"&&match->userText,"A room name is not drawn as player text");
 Check(match->value==loc::Tf("public.room_playing","2/4")&&match->detail==loc::Tf("public.room_detail_playing","金曜ルーム",2u,4u,"use1",1u)&&
  match->detailText==DetailText::Name,"A room with a match on does not say so");
 Check(friday->detail==loc::Tf("public.room_detail","Friday Night",3u,8u,"use1"),"A room's detail does not name it");
 // Refresh asks again; a failed refresh is said once.
 const auto before=j.requests().size();
 h.Choose("pr-refresh");
 Check(j.requests().size()==before+1&&j.requests().back()->op==Op::RoomList,"Refresh did not ask for the list");
 h.view.publicRooms.loading=true;h.Frame();h.view.publicRooms.loading=false;h.view.publicRooms.error="bridge_unreachable";++h.view.publicRooms.listed;h.Frame(0,2);
 Check(j.status==loc::T("identity.failure.unreachable"),"A failed refresh was not put in words");
 // No rooms: the empty state, not a bare list.
 h.view.publicRooms.error.clear();h.view.publicRooms.rooms.clear();++h.view.publicRooms.listed;h.Frame(0,2);
 Check(j.row("pr-none")&&j.row("pr-none")->info&&!j.row("pr-room:a"),"An empty list has no empty state");
 // Back leaves the list without sending anything.
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="online","Back did not leave Public rooms");
}

// Pressing a room asks for its ticket; the admission that answers joins with
// the room host's invitation and the ticket as received; a refusal is a
// sentence and joins nothing; a hidden overlay does not join later.
void JoinJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({MakeRoom("a","Friday Night",3,8),MakeRoom("b","Open Mic",2,4)});
 h.Choose("pr-room:b");
 Check(j.requests().back()->op==Op::RoomTicket&&j.requests().back()->roomId=="b"&&j.requests().back()->bridgeId=="brg_1",
  "Pressing a room did not ask for its ticket");
 Check(j.status==loc::T("room.joining_status")&&j.row("pr-room:b")->value==loc::T("public.joining_value")&&!j.row("pr-room:a")->enabled&&!j.row("pr-create")->enabled,
  "A room being joined is not shown as pending");
 const auto asked=j.requests().size();h.Choose("pr-room:a");
 Check(j.requests().size()==asked,"A second room was asked for while one is pending");
 Check(j.joins()==0,"A room was joined before its admission arrived");
 j.Admit();
 Check(j.joins()==1&&h.actions.back().command.kind==Kind::JoinInvite&&h.actions.back().command.invitation=="sf4e3:host-invitation"&&
  h.actions.back().publicTicket=="{\"ticket\":{\"room_id\":\"b\"},\"kid\":\"k1\",\"signature\":\"sig\"}"&&
  h.actions.back().command.generation==h.view.session.generation,"The admission did not join with the invitation and the ticket");
 const auto joined=j.joins();h.Frame(0,5);Check(j.joins()==joined,"An admission joined twice");
 // The room opens: the controller's own opening state takes over the screen.
 h.view.session.room=netplay::RoomState::Opening;h.view.session.generation.room=1;h.Frame(0,2);
 Check(j.row("cancel-open")&&j.status==loc::T("room.joining_status"),"A room being joined has no Stop row");
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.Frame(0,2);
 // Each refusal, in words, and none joins.
 const std::pair<const char*,const char*> refusals[]={
  {"room_full","public.failure.room_full"},{"banned","public.failure.banned"},{"room_not_found","public.failure.room_not_found"},{"room_not_open","public.failure.room_not_open"},
  {"room_limit","public.failure.room_limit"},{"unsupported_build","public.failure.unsupported_build"},{"invalid_name","public.failure.invalid_name"},{"rooms_unavailable","public.failure.rooms_unavailable"}};
 for(const auto& refusal:refusals){
  h.Choose("pr-room:b");j.Admit(refusal.first);
  Check(j.status==loc::T(refusal.second),"A refused ticket did not say why");
  Check(!j.row("pr-room:b")||j.row("pr-room:b")->enabled,"A refusal left the list blocked");
 }
 h.Choose("pr-room:b");j.Admit("bridge_unreachable");Check(j.status==loc::T("identity.failure.unreachable"),"A failure from the helper was not put in words");
 h.Choose("pr-room:b");j.Admit("something_new");Check(j.status==loc::Tf("identity.failure.other","something_new"),"An unknown refusal has no fallback sentence");
 Check(j.joins()==joined,"A refusal joined a room");
 // The overlay is hidden while the ticket is asked for: it joins nothing when it returns.
 h.Choose("pr-room:b");h.shell.Conceal();j.Admit();
 Check(j.joins()==joined,"An admission that arrived while Ember was hidden joined later");
}

// Every create and ticket request has its own identity, and only its own answer
// acts. A request abandoned by hiding the overlay is not joined when it
// completes, even after the player has chosen another room; the later choice
// is accepted and joins with its own invitation and ticket.
void AbandonedRequestJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({MakeRoom("a","Friday Night",3,8),MakeRoom("b","Open Mic",2,4)});
 h.Choose("pr-room:a");
 const auto first=j.lastAdmission();
 Check(first!=0&&j.requests().back()->roomId=="a","Pressing a room did not send a request with an identity");
 h.shell.Conceal();
 // The overlay returns: the player is not waiting any more, and picks the other room.
 h.Frame(0,2);
 Check(j.status!=loc::T("room.joining_status")&&j.row("pr-room:b")&&j.row("pr-room:b")->enabled,"Reopening left the abandoned request blocking the list");
 h.Choose("pr-room:b");
 const auto second=j.lastAdmission();
 Check(second>first&&j.requests().back()->roomId=="b","The room chosen after reopening was not sent as a newer request");
 Check(j.status==loc::T("room.joining_status")&&j.row("pr-room:b")->value==loc::T("public.joining_value"),"The new choice is not shown as pending");
 // The abandoned request completes first: nothing joins, and the new choice still waits.
 j.AdmitAs(first,"","a","sf4e3:room-a-invitation","{\"ticket\":{\"room_id\":\"a\"},\"kid\":\"k1\",\"signature\":\"a\"}");
 Check(j.joins()==0,"An abandoned admission was joined");
 Check(j.status==loc::T("room.joining_status"),"A stale answer ended the wait for the room chosen later");
 h.Frame(0,5);Check(j.joins()==0,"An abandoned admission joined on a later frame");
 // The chosen room's own answer joins with its invitation and ticket.
 j.AdmitAs(second,"","b","sf4e3:room-b-invitation","{\"ticket\":{\"room_id\":\"b\"},\"kid\":\"k1\",\"signature\":\"b\"}");
 Check(j.joins()==1&&h.actions.back().command.invitation=="sf4e3:room-b-invitation"&&
  h.actions.back().publicTicket=="{\"ticket\":{\"room_id\":\"b\"},\"kid\":\"k1\",\"signature\":\"b\"}","The chosen room was not joined with its own admission");
 // A request that was never abandoned still joins once, and an old answer replayed does not join again.
 h.Frame(0,5);Check(j.joins()==1,"An admission joined twice");
 j.AdmitAs(first);Check(j.joins()==1,"A replayed answer to an old request joined");
}

// The same for a created room: hiding the overlay abandons it. The room it
// made is never entered, and the service closes a room nobody enters; the next
// create is a new request that joins with its own admission.
void AbandonedCreateJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({MakeRoom("a","Friday Night",3,8)});
 h.view.preferences.roomName="Open Mic";h.view.preferences.roomCapacity=6;h.Frame(0,2);
 h.Choose("pr-create");h.Frame(0,40);
 const auto saved=std::find_if(h.actions.rbegin(),h.actions.rend(),[](const ShellAction& a){return a.command.kind==Kind::SavePreferences;});
 Check(saved!=h.actions.rend(),"The visibility was not saved");
 h.view.preferences=saved->preferences;h.Frame(0,2);
 h.Choose("host");
 const auto first=j.lastAdmission();
 Check(first!=0&&j.requests().back()->op==Op::RoomCreate&&j.status==loc::T("room.creating_status"),"Create did not send a request");
 h.shell.Conceal();h.Frame(0,2);
 Check(j.status!=loc::T("room.creating_status")&&j.row("host")&&j.row("host")->enabled,"Reopening left the abandoned create pending");
 j.AdmitAs(first,"","b","sf4e3:abandoned-room");
 Check(j.joins()==0,"A room created before the overlay was hidden was joined");
 h.Choose("host");
 const auto second=j.lastAdmission();
 Check(second>first,"A create after reopening was not a newer request");
 j.AdmitAs(first,"","b","sf4e3:abandoned-room");
 Check(j.joins()==0,"The abandoned create's answer joined while the new one waited");
 j.AdmitAs(second,"","c","sf4e3:second-room");
 Check(j.joins()==1&&h.actions.back().command.invitation=="sf4e3:second-room","The new create did not join with its own admission");
}

// Without a trusted Ember ID the screen says so and leads to the Ember ID
// screen; nothing is asked of the service.
void NoIdentityJourney(){
 {
  Journey j(false);auto& h=j.h;j.OpenList();
  Check(j.row("pr-needs-id")&&j.row("pr-needs-id")->info&&j.row("identity")&&!j.row("pr-create")&&!j.row("pr-refresh"),
   "Public rooms did not ask for an Ember ID");
  Check(j.requests().empty(),"A service was asked for rooms without an Ember ID");
  h.Choose("identity");Check(h.shell.Navigation().Screen()=="identity","The Ember ID row did not open the Ember ID screen");
 }
 // A ready ID without a trusted service says the same.
 Journey k;k.h.view.identity.bridges.clear();k.OpenList();
 Check(k.row("pr-needs-id")&&k.requests().empty(),"An ID without a trusted service was offered the list");
}

// Create on Public sends room_create with the room's name and capacity, never
// a private host; its rules are not offered; the admission joins; Private is
// unchanged.
void CreateJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({MakeRoom("a","Friday Night",3,8)});
 h.view.preferences.roomName="Open Mic";h.view.preferences.roomCapacity=6;h.Frame(0,2);
 h.Choose("pr-create");
 Check(h.shell.Navigation().Screen()=="create"&&j.row("visibility")&&j.row("visibility")->value==loc::T("public.visibility.public"),
  "Create public room did not open Create on Public");
 Check(!j.row("rounds")&&!j.row("set-length")&&j.row("room-name")&&j.row("capacity"),"A public room offers rules that cannot be sent");
 Check(j.row("host")&&j.row("host")->label==loc::T("public.create")&&j.row("host")->enabled,"Create is not Create public room");
 // The choice is saved with the other room defaults.
 h.Frame(0,40);
 const auto saved=std::find_if(h.actions.rbegin(),h.actions.rend(),[](const ShellAction& a){return a.command.kind==Kind::SavePreferences;});
 Check(saved!=h.actions.rend()&&saved->preferences.roomPublic,"The visibility was not saved with the room defaults");
 h.view.preferences=saved->preferences;h.Frame(0,2);
 h.Choose("host");
 Check(j.requests().back()->op==Op::RoomCreate&&j.requests().back()->roomName=="Open Mic"&&j.requests().back()->capacity==6&&
  j.requests().back()->bridgeId=="brg_1","Create did not send the name and capacity");
 Check(j.hosts()==0,"A public room opened a private room");
 Check(j.status==loc::T("room.creating_status")&&!j.row("host")->enabled,"A room being created is not shown as pending");
 j.Admit("","b");
 Check(j.joins()==1&&h.actions.back().command.invitation=="sf4e3:host-invitation"&&!h.actions.back().publicTicket.empty(),"The new room's admission did not join");
 // A refusal says why on Create and the player can try again.
 h.Choose("host");j.Admit("room_limit");
 Check(j.status==loc::T("public.failure.room_limit")&&j.row("host")->enabled&&j.joins()==1,"A refused create did not say why");
 // Without a trusted service Create cannot send, and says why.
 h.view.identity.bridges.clear();h.Screen("home");h.Screen("create");j.answerIdentityUntilIdle();
 Check(j.row("host")&&!j.row("host")->enabled&&j.row("host")->detail==loc::T("public.create_needs_id"),"Create public room was offered without an Ember ID");
 h.view.identity.bridges={{"brg_1","https://bridge.example","Example"}};
 // Private again: the rules return and Create hosts as before.
 h.Screen("home");h.Screen("create");j.answerIdentityUntilIdle();
 h.FocusOn("visibility");h.Press(MenuInput::Left);
 Check(j.row("visibility")->value==loc::T("public.visibility.private")&&j.row("rounds")&&j.row("host")->label==loc::T("online.create"),"Private did not bring the rules back");
 const auto asked=j.requests().size();h.Choose("host");
 Check(j.hosts()==1&&j.requests().size()==asked,"A private room did not host as before");
}

// After a restart with Public saved, going straight to Create waits for the
// Ember ID's services, which Home reads by itself: the host row waits, then
// enables and creates on the approved service. With Private saved nothing more
// is asked, and choosing Public waits for the same read.
void RestoredCreateJourney(){
 {
  Journey j;auto& h=j.h;
  h.view.preferences.roomPublic=true;h.view.preferences.roomName="Open Mic";h.view.preferences.roomCapacity=6;h.Frame(0,2);
  h.Screen("create");h.Frame(0,2);
  Check(j.identity().size()==1&&j.identity().back()->op==IdentityOp::BridgeList,"Create on a saved Public did not wait for the Ember ID's services");
  Check(j.row("host")&&!j.row("host")->enabled&&j.row("host")->detail==loc::T("public.create_needs_id"),"Create public room was offered before the service was known");
  j.answerIdentityUntilIdle();
  Check(h.shell.Navigation().Screen()=="create"&&j.row("host")&&j.row("host")->enabled&&j.row("host")->label==loc::T("public.create"),
   "Create public room stayed disabled after the Ember ID answered");
  const auto asked=j.identity().size();h.Frame(0,10);
  Check(j.identity().size()==asked,"Create asked the Ember ID again while it was selected");
  Check(j.requests().empty(),"Create sent a service request before it was pressed");
  h.Choose("host");
  Check(j.requests().size()==1&&j.requests().back()->op==Op::RoomCreate&&j.requests().back()->bridgeId=="brg_1"&&
   j.requests().back()->roomName=="Open Mic"&&j.requests().back()->capacity==6,"Create did not send room_create to the approved service");
  Check(j.hosts()==0,"A public room opened a private room");
 }
 Journey k;auto& h=k.h;
 k.h.view.preferences.roomPublic=false;h.Frame(0,2);
 h.Screen("create");h.Frame(0,10);
 // Only Home's own read of the services is in flight.
 Check(k.identity().size()==1&&k.requests().empty(),"Create on Private asked the Ember ID or the service");
 // Choosing Public here waits for that read rather than asking again.
 h.FocusOn("visibility");h.Press(MenuInput::Right);h.Frame(0,2);
 Check(k.row("visibility")->value==loc::T("public.visibility.public")&&k.identity().size()==1&&k.identity().back()->op==IdentityOp::BridgeList,
  "Choosing Public did not wait for the Ember ID's services");
 k.answerIdentityUntilIdle();h.Frame(0,10);
 Check(k.row("host")&&k.row("host")->enabled&&k.requests().empty(),"Choosing Public did not select the service");
}

// A public room says so on its board and offers no invitation to copy, since
// its admission is by ticket; locked says so too.
void RoomJourney(){
 Journey j;auto& h=j.h;
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Open Mic";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 h.view.invitation="sf4e3:host-invitation";
 h.Screen("room");h.Frame(0,2);
 Check(j.status==loc::T("room.private_status")&&j.row("copy")&&j.row("copy-short"),"A private room lost its invitation rows");
 h.view.room.serverOwned=true;h.Frame(0,2);
 Check(j.status==loc::T("room.public_status")&&!j.row("copy")&&!j.row("copy-short")&&j.row("leave"),"A public room reads like a private one");
 h.view.room.locked=true;h.Frame(0,2);Check(j.status==loc::T("room.public_locked_status"),"A locked public room does not say so");
}

// A room whose control is lost offers a replacement, except a public room: its
// host cannot be replaced, so the room closes instead.
void RoomLostJourney(){
 Journey j;auto& h=j.h;
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Lost;
 h.view.session.recovery=netplay::Recovery::ReplacementOffered;h.view.session.fault=netplay::Fault::ControlRecovering;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Open Mic";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 h.Screen("room");h.Frame(0,2);
 Check(j.row("replace-room"),"A private room lost its offer to replace the room");
 h.view.room.serverOwned=true;h.view.session.recovery=netplay::Recovery::Recovering;h.Frame(0,2);
 Check(j.status==loc::T("room.control_unavailable")||j.status==loc::T("room.control_recovering"),"A public room did not say it was reconnecting");
 h.view.session.recovery=netplay::Recovery::ReplacementOffered;h.Frame(0,2);
 Check(!j.row("replace-room")&&j.row("leave"),"A public room offered to replace its room");
}
}
int main(){try{ListJourney();JoinJourney();NoIdentityJourney();CreateJourney();RestoredCreateJourney();AbandonedRequestJourney();AbandonedCreateJourney();RoomJourney();RoomLostJourney();std::cout<<"Public rooms journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
