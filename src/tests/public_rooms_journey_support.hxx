#pragma once
// What the public room journeys share: a Harness that records the menu rows,
// the status line and the cards the shell draws, lists the Ember ID and public
// room requests it has submitted, and answers them; the rooms it lists, and the
// room links a bot or site hands out. The probes capture the object, so make
// one per scenario and never two at once.
#include "shell_journey_support.hxx"
#include "../common/TournamentLink.hxx"
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
 // The cards the last frame drew, for the notice and dialog buttons.
 std::set<std::string> cards;
 explicit Journey(bool trusted=true){
  auto& id=h.view.identity;id.known=true;id.state=trusted?"ready":"disabled";
  if(trusted){id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
   id.bridges={{"brg_1","https://bridge.example","Example"}};}
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
  SetMenuCardProbe([&](const char* id,ImVec2,ImVec2){cards.insert(id);});
  h.Frame();
 }
 ~Journey(){SetMenuEntriesProbe({});SetMenuStatusProbe({});SetMenuCardProbe({});}
 // Whether a notice is open on the frame just drawn: its OK button is a card, and so is its alternative.
 bool noticeOpen(bool alternative=false){cards.clear();h.Frame();return cards.count(alternative?"notice/1":"notice/0")>0;}
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
 // The list requests sent so far.
 std::size_t lists() const{
  return static_cast<std::size_t>(std::count_if(h.actions.begin(),h.actions.end(),[](const ShellAction& a){return a.tournament.op==Op::RoomList;}));
 }
 // The service has answered the newest list request: the rooms, or a failure.
 void Answer(const std::vector<netplay::publicrooms::Room>& rooms,const char* error=""){
  auto& list=h.view.publicRooms;list.bridge="brg_1";list.rooms=rooms;list.loading=false;list.error=error;++list.listed;h.Frame(0,2);
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

inline netplay::publicrooms::Room MakeRoom(const char* id,const char* name,unsigned members,unsigned capacity,unsigned playing=0){
 netplay::publicrooms::Room room;room.id=id;room.name=name;room.members=members;room.capacity=capacity;room.playing=playing;room.region="use1";return room;
}

// A service and room a room link names, and a service the player does not trust.
const char* const LinkBridge="brg_0dbc0598-2312-4ce3-9df8-e160330565e6";
const char* const LinkRoom="0123456789abcdef0123456789abcdef";
const char* const OtherBridge="brg_5d1f3c0a-7b2e-4c11-8a3d-1f2e3d4c5b6a";

// Delivers a link the way the runtime does: the status's room link, a new sequence.
inline void OpenLink(Journey& j,const char* bridge,const char* room,bool free=true){
 auto& link=j.h.view.tournament.roomLink;link.bridge=bridge;link.room=room;link.free=free;++link.sequence;j.h.Frame(0,2);
}
// The clipboard the player pastes from.
std::string g_clipboard;
inline void Clipboard(const std::string& text){
 g_clipboard=text;ImGui::GetPlatformIO().Platform_GetClipboardTextFn=[](ImGuiContext*){return g_clipboard.c_str();};
}
// Ticket requests for a room, as sent.
inline std::vector<const Command*> ticketRequests(const Journey& j){
 std::vector<const Command*> out;for(const auto* c:j.requests())if(c->op==Op::RoomTicket)out.push_back(c);return out;
}
inline std::size_t tickets(const Journey& j){return ticketRequests(j).size();}
// Whether the newest ticket request asked `bridge` for `room`.
inline bool lastTicketIs(const Journey& j,const char* bridge,const char* room){
 const auto sent=ticketRequests(j);return !sent.empty()&&sent.back()->bridgeId==bridge&&sent.back()->roomId==room;
}
}
// The setup and room link scenarios, in public_rooms_setup_journey_test.cxx.
void RunPublicRoomsSetupJourneys();
