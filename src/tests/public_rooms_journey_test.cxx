// The public room journeys: the list a player opens from Online play, a ticket
// for the room they press, the join that follows its admission, a refusal in
// words, Create on Public, the order, the filter, Quick join and the refresh by
// itself. The runtime's answers are the view's fields. The setup and room link
// journeys are in public_rooms_setup_journey_test.cxx.
#include "public_rooms_journey_support.hxx"
namespace {
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
 // Nothing is listed yet: the toolbar waits, and the loading card stands in for the rooms.
 Check(j.row("pr-loading")&&j.row("pr-loading")->wide&&j.row("pr-quick")&&!j.row("pr-quick")->enabled&&!j.row("pr-room:a"),"The first load has no loading card");
 auto friday=MakeRoom("a","Friday Night",3,8);friday.hostName="Ken";friday.createdAt=100;
 auto match=MakeRoom("b","金曜ルーム",2,4,1);match.createdAt=200;
 j.List({friday,match,MakeRoom("c","Full House",4,4)});
 // The toolbar is four grid cells: Quick join, Create room, the filter, Paste room link; the rooms follow, full width.
 std::vector<std::string> order;for(const auto& r:j.rows)order.push_back(r.id);
 Check(order==std::vector<std::string>({"pr-quick","pr-create","pr-filter","pr-paste","pr-room:a","pr-room:b","pr-room:c"}),"The list is not Quick join, Create room, the filter, Paste room link and then the rooms");
 Check(!j.row("pr-refresh")&&!j.row("pr-needs-id"),"The list kept a Refresh or needs-ID row");
 for(const char* cell:{"pr-quick","pr-create","pr-filter","pr-paste"})Check(j.row(cell)&&!j.row(cell)->wide&&j.row(cell)->enabled&&!j.row(cell)->hint.empty(),"A toolbar cell is missing, wide or disabled");
 // Friday Night came from a bridge without room details, so its card is the short one.
 Check(j.row("pr-room:a")->wide&&j.row("pr-room:a")->height==72,"A room without details is not the short full-width card");
 for(const char* room:{"pr-room:b","pr-room:c"})Check(j.row(room)->wide&&j.row(room)->height==100,"A room is not a full-width card");
 Check(j.row("pr-quick")->label==loc::T("public.quick")&&j.row("pr-create")->label==loc::T("public.create_room")&&j.row("pr-paste")->label==loc::T("public.paste_link"),"A toolbar cell has the wrong label");
 Check(j.row("pr-filter")->label==loc::Tf("public.filter_label",loc::T("public.filter.all"))&&j.row("pr-filter")->choices.size()==3&&j.row("pr-filter")->chosen=="all","The filter does not start on all rooms");
 const auto* first=j.row("pr-room:a");const auto* second=j.row("pr-room:b");
 Check(first->label=="Friday Night"&&first->userText&&first->value.empty()&&first->hint==loc::T("online.join"),"A room does not show its name and Join");
 Check(second->label=="金曜ルーム"&&second->userText,"A room name is not drawn as player text");
 // With details the row names the host, region and seats; without them it keeps the plain sentence.
 Check(first->detail==loc::Tf("public.room_detail2","Ken",loc::T("network.region_use1"),3u,8u)&&first->detailText==DetailText::Name,"A room's detail does not name its host and seats");
 Check(second->detail==loc::Tf("public.room_detail_playing","金曜ルーム",2u,4u,"use1",1u)&&second->detailText==DetailText::Name,"A room with a match on and no details does not say so");
 Check(j.row("pr-room:c")->detail==loc::Tf("public.room_detail","Full House",4u,4u,"use1")&&!j.row("pr-room:c")->enabled,"A full room is not shown disabled");
 // Options refreshes by hand: Updating... while it is in flight; a failed refresh is said once.
 const auto before=j.lists();
 h.Press(MenuInput::Options);
 Check(j.lists()==before+1&&j.requests().back()->op==Op::RoomList,"Options did not ask for the list");
 h.view.publicRooms.loading=true;h.Frame();
 Check(j.status==loc::T("public.updating"),"A refresh the player asked for does not say Updating");
 Check(j.row("pr-room:a"),"A refresh emptied the list");
 h.view.publicRooms.loading=false;h.view.publicRooms.error="bridge_unreachable";++h.view.publicRooms.listed;h.Frame(0,2);
 Check(j.status==loc::T("public.failure.unavailable"),"A failed refresh was not put in words");
 Check(j.row("pr-room:a")&&!j.row("pr-error"),"A failed refresh took the listed rooms away");
 // No rooms: the empty state, not a bare list; its action is Create room.
 h.view.publicRooms.error.clear();h.view.publicRooms.rooms.clear();++h.view.publicRooms.listed;h.Frame(0,2);
 Check(j.row("pr-none")&&!j.row("pr-none")->info&&j.row("pr-none")->wide&&j.row("pr-none")->label==loc::T("public.none_all")&&j.row("pr-none")->hint==loc::T("public.create_room")&&!j.row("pr-room:a"),"An empty list has no empty state");
 // Nothing listed and the refresh failed: the failure is the card, with its action Try again, and no notice besides.
 h.Wait(7); // the earlier failure's notice has run out
 h.view.publicRooms.error="bridge_unreachable";++h.view.publicRooms.listed;h.Frame(0,2);
 Check(j.row("pr-error")&&j.row("pr-error")->label==loc::T("public.failure.unavailable")&&j.row("pr-error")->hint==loc::T("public.try_again")&&!j.row("pr-none"),"A failed refresh with nothing listed has no error card");
 Check(j.status!=loc::T("public.failure.unavailable"),"A failure with an error card was also said as a notice");
 const auto asked=j.lists();h.Choose("pr-error");
 Check(j.lists()==asked+1,"The error card did not ask for the list again");
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
 Check(j.status==loc::T("room.joining_status")&&j.row("pr-room:b")->value==loc::T("public.joining_value")&&!j.row("pr-room:a")->enabled&&!j.row("pr-create")->enabled&&!j.row("pr-quick")->enabled,
  "A room being joined is not shown as pending");
 // Stop joining is the first wide row; it drops the request, so its answer joins nothing.
 {std::vector<std::string> wide;for(const auto& r:j.rows)if(r.wide)wide.push_back(r.id);
  Check(!wide.empty()&&wide.front()=="pr-stop","Stop joining is not the first wide row while a room is asked for");}
 h.Choose("pr-stop");
 Check(!j.row("pr-stop")&&j.row("pr-room:b")&&j.row("pr-room:b")->enabled&&j.row("pr-room:b")->value.empty(),"Stop joining left the list blocked");
 j.Admit();Check(j.joins()==0,"A stopped request joined");
 h.Choose("pr-room:b");
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
 Check(j.row("pr-opening")&&j.row("pr-opening")->label=="Open Mic"&&j.row("pr-opening")->value==loc::T("public.connecting")&&j.rows.back().id=="cancel-open"&&!j.row("pr-quick"),
  "A room being joined has no card of its own");
 Check(h.shell.Navigation().Focus()=="cancel-open","The cursor does not wait on Stop joining");
 // Back from Home returns to this screen, not the Join screen: a public room is joined from here.
 h.Screen("home");h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="public-rooms","An opening public room did not return to Public rooms");
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.Frame(0,2);
 // Each refusal, in words, and none joins.
 const std::pair<const char*,const char*> refusals[]={
  {"room_full","public.failure.room_full"},{"banned","public.failure.banned"},{"room_not_found","public.failure.room_not_found"},{"room_not_open","public.failure.room_not_open"},
  {"room_limit","public.failure.room_limit"},{"unsupported_build","public.failure.unsupported_build"},{"invalid_name","public.failure.invalid_name"},{"rooms_unavailable","public.failure.rooms_unavailable"},
  {"room_locked","public.failure.room_locked"},{"unavailable","public.failure.unavailable"},{"untrusted_signature","public.failure.untrusted"},{"binding_mismatch","public.failure.untrusted"},
  {"internal","public.failure.internal"},{"helper_unavailable","public.failure.helper"}};
 for(const auto& refusal:refusals){
  h.Choose("pr-room:b");j.Admit(refusal.first);
  Check(j.status==loc::T(refusal.second),"A refused ticket did not say why");
  Check(!j.row("pr-room:b")||j.row("pr-room:b")->enabled,"A refusal left the list blocked");
 }
 h.Choose("pr-room:b");j.Admit("bridge_unreachable");Check(j.status==loc::T("public.failure.unavailable"),"A failure from the helper was not put in words");
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

// Create on Public sends room_create with the room's name and capacity, never
// a private host; its rules are offered, apart from a private room's; the
// admission joins; Private is unchanged.
void CreateJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({MakeRoom("a","Friday Night",3,8)});
 h.view.preferences.roomName="Open Mic";h.view.preferences.roomCapacity=6;h.Frame(0,2);
 h.Choose("pr-create");
 Check(h.shell.Navigation().Screen()=="create"&&j.row("visibility")&&j.row("visibility")->value==loc::T("public.visibility.public"),
  "Create public room did not open Create on Public");
 Check(j.row("set-length")&&j.row("set-length")->value==SetLengthText(room::SetFormat::Ft2)&&j.row("rotation")&&
  j.row("rotation")->value==RotationText(room::RotationMode::WinnerStays)&&j.row("rounds")&&j.row("room-name")&&j.row("capacity"),
  "Create on Public did not offer the rules at the public default");
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

// Create on Public with rules of the player's own: they are saved apart from a
// private room's, and once the creator is in the new room as its host, every
// table gets them by the ordinary host action, which the room accepts.
void CreateWithRulesJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({});
 h.view.preferences.roomName="Open Mic";h.Frame(0,2);
 h.Choose("pr-none");
 Check(h.shell.Navigation().Screen()=="create","Create room did not open Create");
 h.FocusOn("set-length");h.Press(MenuInput::Right);
 h.FocusOn("rotation");h.Press(MenuInput::Right);
 Check(j.row("set-length")->value==SetLengthText(room::SetFormat::Ft3)&&j.row("rotation")->value==RotationText(room::RotationMode::LoserStays),
  "The public rules did not step");
 // Each save is acknowledged, as the runtime does, so the next one follows.
 bool savedAny=false;
 for(int i=0;i<4;++i){
  h.Frame(0,40);
  const auto last=std::find_if(h.actions.rbegin(),h.actions.rend(),[](const ShellAction& a){return a.command.kind==Kind::SavePreferences;});
  if(last!=h.actions.rend()){savedAny=true;h.view.preferences=last->preferences;}
 }
 const auto& saved=h.view.preferences;
 Check(savedAny&&saved.publicTableRules.format==room::SetFormat::Ft3&&
  saved.publicTableRules.rotation==room::RotationMode::LoserStays&&saved.tableRules==room::Rules(),
  "The public rules were not saved apart from a private room's");
 h.Frame(0,2);
 h.Choose("host");j.Admit("","b");
 Check(j.joins()==1,"The new room's admission did not join");
 // The service opened the room at the public default; the creator joins first and moderates.
 room::RoomAuthority authority("Open Mic",8,9,room::PublicRoomRules());
 Check(authority.SetServerOwned(),"The room is not server owned");
 room::MemberProfile profile;profile.account="emb1-creator";
 const auto joined=authority.Join("Player",room::ConnectionRef{"host","1"},false,profile);
 Check(joined.accepted,"The creator could not join");
 const auto creator=joined.snapshot.members.back().id;
 auto& s=h.view.session;s.room=netplay::RoomState::Joined;s.control=netplay::Health::Healthy;s.recovery=netplay::Recovery::None;
 h.view.room=authority.SnapshotFor(creator);
 const auto before=h.actions.size();h.Frame(0,4);
 std::vector<room::Action> sent;
 for(std::size_t i=before;i<h.actions.size();++i)if(h.actions[i].command.kind==Kind::RoomAction&&h.actions[i].roomAction.kind==room::ActionKind::SetRules)sent.push_back(h.actions[i].roomAction);
 Check(sent.size()==room::TableCount,"The chosen rules were not sent for every table");
 for(const auto& action:sent)Check(authority.Apply(creator,action).accepted,"The room refused the creator's rules");
 for(const auto& table:authority.SnapshotView().tables)
  Check(table.rules==h.view.preferences.publicTableRules,"A table did not end up with the chosen rules");
 // Sent once: a later snapshot sends nothing more.
 h.view.room=authority.SnapshotFor(creator);const auto after=h.actions.size();h.Frame(0,4);
 Check(std::none_of(h.actions.begin()+after,h.actions.end(),[](const ShellAction& a){return a.roomAction.kind==room::ActionKind::SetRules&&a.command.kind==Kind::RoomAction;}),
  "The chosen rules were sent again");
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

// A room placed in `region`, named by its ID, with its seats, when it opened and whether it is locked.
netplay::publicrooms::Room Placed(const char* id,const char* region,unsigned members,unsigned capacity,std::uint64_t createdAt=0,bool locked=false){
 auto room=MakeRoom(id,id,members,capacity);room.region=region;room.createdAt=createdAt;room.locked=locked;return room;
}
// Whether the status is the list's own count of `rooms` open, with whatever age it gives.
bool CountsRooms(const std::string& status,std::size_t rooms){
 const std::string sample=loc::Tf("public.status_listed",rooms,std::string(""));
 const auto cut=sample.find('');
 return cut!=std::string::npos&&status.size()>cut&&status.compare(0,cut,sample,0,cut)==0;
}
// The rooms on screen, in the order shown, as their IDs run together.
std::string Shown(const Journey& j){
 std::string out;for(const auto& r:j.rows)if(r.id.compare(0,8,"pr-room:")==0)out+=r.id.substr(8);return out;
}

// Rooms that can be joined come first, near ones before far ones once the region
// is known, then the most free seats, the most players and the oldest; locked and
// full rooms are last and cannot be pressed. Without a region nothing is near.
void SortJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({Placed("x","euc1",1,8,5),Placed("f","use1",4,4,6),Placed("l","use1",1,8,7,true),Placed("n","use1",2,8,20),Placed("m","use1",5,8,9),Placed("p","use1",2,8,10)});
 Check(Shown(j)=="xpnmlf",(std::string("Rooms are not sorted by free seats when no region is known: ")+Shown(j)).c_str());
 Check(j.row("pr-room:p")->enabled&&!j.row("pr-room:l")->enabled&&!j.row("pr-room:f")->enabled,"A locked or full room can be pressed");
 h.view.netReport.relay="other";h.Frame(0,2);
 Check(Shown(j)=="xpnmlf","The other region was taken for a region that is near");
 h.view.netReport.relay="use1";h.Frame(0,2);
 Check(Shown(j)=="pnmxlf",(std::string("Near rooms do not come before far ones: ")+Shown(j)).c_str());
 // The order does not wobble from one refresh to the next.
 j.List({Placed("p","use1",2,8,10),Placed("n","use1",2,8,20),Placed("x","euc1",1,8,5),Placed("m","use1",5,8,9),Placed("f","use1",4,4,6),Placed("l","use1",1,8,7,true)});
 Check(Shown(j)=="pnmxlf","The same rooms in another bridge order were sorted differently");
}

// Quick join asks for the first room that can be joined, in the player's region when
// it is known (not a far room with more seats), skips the rooms that refused this
// visit, and with none left offers to create one.
void QuickJoinJourney(){
 {
 Journey j;auto& h=j.h;j.OpenList();
 h.view.netReport.relay="use1";
 j.List({Placed("x","euc1",1,8,5),Placed("f","use1",4,4,6),Placed("l","use1",1,8,7,true),Placed("n","use1",2,8,20),Placed("m","use1",5,8,9)});
 Check(j.row("pr-quick")&&j.row("pr-quick")->enabled,"Quick join is not offered on a list");
 h.Choose("pr-quick");
 Check(tickets(j)==1&&lastTicketIs(j,"brg_1","n"),"Quick join did not ask for the near room with the most free seats");
 Check(h.shell.Navigation().Focus()=="pr-room:n"&&j.row("pr-room:n")->value==loc::T("public.joining_value"),"Quick join did not show the room it picked");
 // A refusal that says the room is not available is remembered: the next pick is another room.
 j.Admit("room_full");
 h.Choose("pr-quick");
 Check(tickets(j)==2&&lastTicketIs(j,"brg_1","m"),"Quick join asked again for a room that was full");
 j.Admit("room_locked");
 // Only a far room, a locked one and a full one are left: nothing near has a seat.
 h.Choose("pr-quick");
 Check(tickets(j)==2,"Quick join asked for a room none of which qualified");
 Check(j.noticeOpen(true),"Quick join with no room did not offer to create one");
 h.Press(MenuInput::Select);h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="public-rooms"&&!j.noticeOpen(),"OK on the notice left Public rooms or kept the notice");
 // Leaving the screen ends the visit: the refused room is a candidate again.
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 h.Choose("pr-quick");
 Check(tickets(j)==3&&lastTicketIs(j,"brg_1","n"),"A new visit did not forget the rooms that refused");
 j.Admit("banned");h.Choose("pr-quick");
 Check(lastTicketIs(j,"brg_1","m")&&tickets(j)==4,"A room that banned the player was asked for again");
 // With nothing to join the notice's other button opens Create on Public.
 j.Admit("room_full");h.Choose("pr-quick");
 Check(j.noticeOpen(true),"The notice with no room does not carry the Create button");
 h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,4);
 Check(h.shell.Navigation().Screen()=="create"&&j.row("visibility")&&j.row("visibility")->value==loc::T("public.visibility.public"),"Create room on the notice did not open Create on Public");
 }
 {
  // Without a known region any room with a seat will do: the one with the most free seats.
  Journey k;k.OpenList();k.List({Placed("x","euc1",1,8,5),Placed("n","use1",2,8,20),Placed("f","use1",4,4,6)});
  k.h.Choose("pr-quick");
  Check(tickets(k)==1&&lastTicketIs(k,"brg_1","x"),"Quick join without a region did not ask for the room with the most seats");
 }
 // An empty list also offers to create.
 Journey e;e.OpenList();e.List({});
 e.h.Choose("pr-quick");Check(tickets(e)==0&&e.noticeOpen(true),"Quick join on an empty list did not offer to create a room");
}

// Show: All rooms, Near me (disabled until the region is known) or Free seats. A
// filter with nothing to show says so and offers every room again; the choice
// lasts for the visit.
void FilterJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 const auto relist=[&]{j.List({Placed("a","use1",3,8,1),Placed("b","euc1",2,4,2),Placed("c","use1",4,4,3)});};
 relist();
 const auto* filter=j.row("pr-filter");
 Check(filter&&filter->choices.size()==3&&filter->choices[0].id=="all"&&filter->choices[1].id=="near"&&filter->choices[2].id=="seats"&&filter->chosen=="all"&&
  filter->choices[0].label==loc::T("public.filter.all")&&filter->choices[1].label==loc::T("public.filter.near")&&filter->choices[2].label==loc::T("public.filter.seats"),"The filter lacks its three choices");
 Check(!filter->choices[1].enabled&&filter->choices[1].detail==loc::T("public.filter_near_unknown")&&filter->choices[0].enabled&&filter->choices[2].enabled,"Near me is not disabled without a region");
 Check(Shown(j)=="abc","The list is filtered before a filter is chosen");
 h.view.netReport.relay="use1";h.Frame(0,2);
 Check(j.row("pr-filter")->choices[1].enabled&&j.row("pr-filter")->choices[1].detail.empty(),"Near me stays disabled with a region");
 // Near me: only the rooms in the player's region; the status says how many of how many.
 h.Choose("pr-filter");Check(h.shell.Navigation().Choosing(),"The filter did not open its choices");
 h.Press(MenuInput::Down);h.Press(MenuInput::Select);
 Check(!h.shell.Navigation().Choosing()&&j.row("pr-filter")->chosen=="near"&&j.row("pr-filter")->label==loc::Tf("public.filter_label",loc::T("public.filter.near")),"Choosing Near me did not set the filter");
 Check(Shown(j)=="ac","Near me lists rooms that are not near");
 relist();
 Check(j.status==loc::Tf("public.status_filtered",std::size_t(2),std::size_t(3),loc::T("public.updated_now")),"The status does not say how many of the rooms are shown");
 // Free seats: rooms that can be joined, near ones first.
 h.Choose("pr-filter");h.Press(MenuInput::Down);h.Press(MenuInput::Select);
 Check(j.row("pr-filter")->chosen=="seats"&&Shown(j)=="ab","Free seats lists a room without one");
 // Back to all rooms, the status then counts them all.
 h.Choose("pr-filter");h.Press(MenuInput::Up);h.Press(MenuInput::Up);h.Press(MenuInput::Select);
 Check(j.row("pr-filter")->chosen=="all"&&Shown(j)=="abc","Choosing all rooms did not list every room");
 relist();
 Check(j.status==loc::Tf("public.status_listed",std::size_t(3),loc::T("public.updated_now")),"The status does not count the rooms listed");
 // A filter with nothing to show: the card says so, and its action lists everything again.
 j.List({Placed("c","use1",4,4,3),Placed("d","use1",2,2,4,true)});
 h.Choose("pr-filter");h.Press(MenuInput::Down);h.Press(MenuInput::Down);h.Press(MenuInput::Select);
 Check(j.row("pr-filter")->chosen=="seats"&&Shown(j).empty()&&j.row("pr-show-all")&&j.row("pr-show-all")->label==loc::T("public.none_filtered")&&
  j.row("pr-show-all")->hint==loc::T("public.show_all")&&j.row("pr-show-all")->wide&&j.row("pr-show-all")->height==110&&!j.row("pr-none")&&!j.row("pr-error"),
  "A filter with nothing to show has no card for it");
 // Every room closes while the filter is on: the card is the empty list's, and one press opens Create on Public.
 j.List({});
 Check(j.row("pr-none")&&j.row("pr-none")->hint==loc::T("public.create_room")&&!j.row("pr-show-all"),"An empty list under a filter offered to show all rooms");
 h.Choose("pr-none");
 Check(h.shell.Navigation().Screen()=="create","The empty list's card under a filter did not open Create in one press");
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 j.List({Placed("c","use1",4,4,3),Placed("d","use1",2,2,4,true)});
 Check(j.row("pr-filter")->chosen=="seats"&&j.row("pr-show-all"),"The filter was not kept across a visit to Create");
 h.Choose("pr-show-all");
 Check(j.row("pr-filter")->chosen=="all"&&Shown(j)=="cd"&&!j.row("pr-show-all"),"Show all rooms did not clear the filter");
 // The choice lasts for the visit, and Near me falls back to all rooms when the region is lost.
 h.Choose("pr-filter");h.Press(MenuInput::Down);h.Press(MenuInput::Select);
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 Check(j.row("pr-filter")->chosen=="near","The filter was forgotten on leaving Public rooms");
 h.view.netReport.relay="";h.Frame(0,2);
 Check(j.row("pr-filter")->chosen=="all"&&!j.row("pr-filter")->choices[1].enabled,"Near me stayed chosen when the region was lost");
 // An empty list under no filter offers Create room instead.
 j.List({});
 Check(j.row("pr-none")&&j.row("pr-none")->label==loc::T("public.none_all"),"An empty list under all rooms has no empty card");
 h.Choose("pr-none");
 Check(h.shell.Navigation().Screen()=="create"&&j.row("visibility")->value==loc::T("public.visibility.public"),"The empty list's card did not open Create on Public");
}

// The list refreshes by itself every 15 s while the screen is open and nothing else
// is going on, and 30 s after a failed refresh; it says so only when the player
// asked, and states how many rooms are open and how long ago they were heard of.
void AutoRefreshJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 Check(j.lists()==1,"Opening Public rooms did not ask for the list once");
 const std::vector<netplay::publicrooms::Room> rooms={MakeRoom("a","Friday Night",3,8),MakeRoom("b","Open Mic",2,4)};
 j.List(rooms);
 auto count=j.lists();
 h.Wait(14);Check(j.lists()==count,"The list was refreshed before 15 s");
 h.Wait(2);Check(j.lists()==count+1&&j.requests().back()->op==Op::RoomList&&j.requests().back()->bridgeId=="brg_1","The list was not refreshed after 15 s");
 // A refresh by itself is silent: no Updating, and the rooms stay.
 h.view.publicRooms.loading=true;h.Frame();
 Check(j.status!=loc::T("public.updating")&&j.status!=loc::T("public.loading")&&j.row("pr-room:a")&&!j.row("pr-loading"),"A refresh nobody asked for was announced or emptied the list");
 j.Answer(rooms);count=j.lists();
 // The status when nothing else speaks: how many rooms, and how long ago.
 Check(j.status==loc::Tf("public.status_listed",std::size_t(2),loc::T("public.updated_now")),"The list's own status is missing");
 h.Wait(5);Check(j.status==loc::Tf("public.status_listed",std::size_t(2),loc::T("public.updated_now")),"Five seconds old is not just now");
 h.Wait(6);Check(j.status==loc::Tf("public.status_listed",std::size_t(2),loc::Tf("public.updated_seconds",11)),"Eleven seconds old is not said in seconds");
 j.Answer(rooms);count=j.lists();
 h.Wait(50);h.Wait(10);Check(j.status==loc::Tf("public.status_listed",std::size_t(2),loc::Tf("public.updated_minutes",1)),"A minute old is not said in minutes");
 // Paused while a request is waiting: joining a room ...
 j.Answer(rooms);count=j.lists();
 h.Choose("pr-room:a");
 h.Wait(20);h.Wait(20);Check(j.lists()==count,"The list was refreshed while a room was being asked for");
 h.Choose("pr-stop");h.Frame();
 Check(j.lists()==count+1,"The list was not refreshed once the room request was dropped");
 // ... while a room is being opened ...
 j.Answer(rooms);count=j.lists();
 h.view.session.room=netplay::RoomState::Opening;h.view.session.generation.room=1;h.Frame(0,2);
 h.Wait(20);Check(j.lists()==count,"The list was refreshed while a room was opening");
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.Frame(0,2);
 Check(j.lists()==count+1,"The list was not refreshed once the room was gone");
 // ... while a refresh is on its way ...
 j.Answer(rooms);count=j.lists();
 h.view.publicRooms.loading=true;h.Wait(20);Check(j.lists()==count,"A refresh was asked for over one in flight");
 h.view.publicRooms.loading=false;h.Frame();Check(j.lists()==count+1,"The refresh that waited was not asked for");
 // ... and off the screen, and hidden: reopening asks at once.
 j.Answer(rooms);count=j.lists();
 h.Screen("online");h.Wait(20);Check(j.lists()==count,"The list was refreshed from another screen");
 h.Choose("public-rooms");h.Frame(0,2);Check(j.lists()==count+1,"Reopening Public rooms did not refresh at once");
 j.Answer(rooms);count=j.lists();
 h.shell.Conceal();h.Frame(0,2);Check(j.lists()==count+1,"Showing Ember again did not refresh at once");
 // A failed refresh is said once and the next one waits 30 s; the notice outranks the list's own status.
 j.Answer(rooms);count=j.lists();
 h.view.publicRooms.loading=true;h.Frame();j.Answer(rooms,"bridge_unreachable");
 Check(j.status==loc::T("public.failure.unavailable"),"A failed refresh did not outrank the list's status");
 h.Wait(7);Check(CountsRooms(j.status,2),"The list's status did not return after the notice");
 h.Wait(22);Check(j.lists()==count,"The list was refreshed before the failure's 30 s");
 h.Wait(2);Check(j.lists()==count+1,"The list was not refreshed 30 s after a failure");
 // A room link waiting in the list holds the refresh too.
 j.Answer(rooms);count=j.lists();
 OpenLink(j,LinkBridge,LinkRoom,false);
 Check(j.row("pr-link"),"A waiting room link is not a row");
 h.Wait(20);h.Wait(20);Check(j.lists()==count,"The list was refreshed while a room link waited");
}

// A public room that is created says Creating, not Joining, and returns to
// Create; one pressed in the list returns to Public rooms. The default name is
// the host's, only while the name was never chosen.
void CreateOpeningJourney(){
 {
 Journey j;auto& h=j.h;h.view.preferences.displayName="Ken";h.Frame(0,2);j.OpenList();
 j.List({MakeRoom("a","Friday Night",3,8)});
 h.Choose("pr-create");
 Check(h.shell.Navigation().Screen()=="create"&&j.row("room-name")&&j.row("room-name")->value==loc::Tf("public.default_name","Ken"),"A new public room is not named for its host");
 h.Frame(0,40);
 const auto saved=std::find_if(h.actions.rbegin(),h.actions.rend(),[](const ShellAction& a){return a.command.kind==Kind::SavePreferences;});
 Check(saved!=h.actions.rend()&&saved->preferences.roomPublic&&saved->preferences.roomName==loc::Tf("public.default_name","Ken"),"The host's name for the room was not saved with the defaults");
 h.view.preferences=saved->preferences;h.Frame(0,2);
 h.Choose("host");
 Check(j.requests().back()->op==Op::RoomCreate&&j.requests().back()->roomName==loc::Tf("public.default_name","Ken")&&j.status==loc::T("room.creating_status"),"Create did not send the host's name or say Creating");
 j.Admit("","b");
 h.view.session.room=netplay::RoomState::Opening;h.view.session.generation.room=1;h.view.session.isHost=false;h.Frame(0,2);
 Check(j.status==loc::T("room.creating_status")&&j.row("cancel-open")&&j.row("cancel-open")->label==loc::T("room.stop_creating_action"),"A room being created says Joining");
 h.Screen("home");
 Check(j.row("online")&&j.row("online")->detail==loc::T("room.creating_status"),"Home does not say the room is being created");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="create","An opening public create did not return to Create");
 }
 {
  // A name the player chose is kept, here and when Public is chosen on Create.
  Journey k;auto& g=k.h;g.view.preferences.displayName="Ken";g.view.preferences.roomName="Open Mic";g.Frame(0,2);
  g.Screen("create");g.FocusOn("visibility");g.Press(MenuInput::Right);
  Check(k.row("visibility")->value==loc::T("public.visibility.public")&&k.row("room-name")->value=="Open Mic","Choosing Public renamed a room the player named");
 }
 Journey m;auto& f=m.h;f.view.preferences.displayName="Ken";f.Frame(0,2);
 f.Screen("create");Check(m.row("room-name")->value==netplay::PlayerPreferences{}.roomName,"A private room lost its default name");
 f.FocusOn("visibility");f.Press(MenuInput::Right);
 Check(m.row("room-name")->value==loc::Tf("public.default_name","Ken"),"Choosing Public did not name the room for its host");
 f.Press(MenuInput::Left);
 Check(m.row("visibility")->value==loc::T("public.visibility.private")&&m.row("room-name")->value==loc::Tf("public.default_name","Ken"),"Choosing Private again changed the name");
}

// The panel remembers the public room it joined (for its link and its words) from
// the admission until the session is Idle again: hiding Ember does not forget it,
// and a join that never starts is forgotten after a few seconds.
void CurrentRoomJourney(){
 PublicRoomsPanel panel;ShellView v;std::vector<ShellAction> sent;
 const auto submit=[&](ShellAction a){sent.push_back(std::move(a));return true;};
 v.identity.known=true;v.identity.state="ready";
 double now=0;
 const auto update=[&](double step=.1){now+=step;panel.Update(v,"public-rooms",LinkBridge,false,submit,now);};
 const auto lastRequest=[&]{std::uint64_t id=0;for(const auto& a:sent)if(a.tournament.op==Op::RoomTicket||a.tournament.op==Op::RoomCreate)id=a.tournament.request;return id;};
 const auto admit=[&](const char* roomId,const char* name){
  auto& list=v.publicRooms;list.failure.clear();
  list.admission=netplay::publicrooms::Admission{{roomId,name,"use1",1,8,0},"sf4e3:inv","{\"ticket\":{}}"};
  list.admitting=0;list.answered=lastRequest();update();
 };
 update();
 Check(!panel.OpeningKind()&&panel.RoomLink().empty(),"A panel in no room has a room link");
 Check(panel.Join(LinkRoom),"The panel did not take a room to join");update();
 Check(!panel.OpeningKind(),"A room not yet admitted is the panel's room");
 admit(LinkRoom,"Open Mic");
 Check(panel.OpeningKind()==PublicRoomsPanel::OpenKind::Join&&panel.RoomLink()==tournament_link::RoomPageUrl(LinkBridge,LinkRoom),"A joined public room has no link");
 const auto link=tournament_link::ParseRoomPasted(panel.RoomLink());
 Check(link.bridgeId==LinkBridge&&link.roomId==LinkRoom,"The room's link does not read back as the room");
 // Idle before the room has opened is the join not having started yet: it stays through Opening, hiding and Joined.
 update();Check(panel.OpeningKind().has_value(),"The room was forgotten before it opened");
 v.session.room=netplay::RoomState::Opening;update();
 panel.Conceal();update();Check(panel.OpeningKind().has_value()&&!panel.RoomLink().empty(),"Hiding Ember forgot the room being opened");
 v.session.room=netplay::RoomState::Joined;update();Check(panel.OpeningKind().has_value()&&!panel.RoomLink().empty(),"The room was forgotten while the player was in it");
 v.session.room=netplay::RoomState::Idle;update();
 Check(!panel.OpeningKind()&&panel.RoomLink().empty(),"The room was kept after the session ended");
 // A created room is the same, of its own kind, and the link names the room the admission gave.
 panel.Create("Open Mic",6);update();admit(LinkRoom,"Open Mic");
 Check(panel.OpeningKind()==PublicRoomsPanel::OpenKind::Create&&panel.RoomLink()==tournament_link::RoomPageUrl(LinkBridge,LinkRoom),"A created room is not the panel's room");
 // A room another way of opening replaces it; a join that never started lapses.
 panel.ForgetCurrent();Check(!panel.OpeningKind(),"The room was kept after another room was opened");
 Check(panel.Join(LinkRoom),"The panel did not take a second room");update();admit(LinkRoom,"Open Mic");
 Check(panel.OpeningKind().has_value(),"The second room was not kept");
 update(3);Check(panel.OpeningKind().has_value(),"A join that was starting was forgotten at once");
 update(3);Check(!panel.OpeningKind(),"A join that never started was kept");
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
int main(){try{ListJourney();JoinJourney();CreateJourney();CreateWithRulesJourney();RestoredCreateJourney();AbandonedRequestJourney();AbandonedCreateJourney();RoomJourney();RoomLostJourney();SortJourney();QuickJoinJourney();FilterJourney();AutoRefreshJourney();CreateOpeningJourney();CurrentRoomJourney();RunPublicRoomsSetupJourneys();std::cout<<"Public rooms journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
