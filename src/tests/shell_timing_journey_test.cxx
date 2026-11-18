#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
// What the shell remembers for a while: the table card's last offer (the
// same-press guard), and how long it remembers it. A device reset recreates
// ImGui, whose clock starts again (Restart); the shell's deadlines keep the
// time they had left.
namespace {
// What a DX9 device reset does to ImGui's clock.
void Restart(){ImGui::GetCurrentContext()->Time=0;}
// This player (member 1) seated at table 1 against member 2 in room `epoch`,
// neither ready, every table at revision 7.
void SeatAtTableOne(Harness& h,std::uint64_t epoch){
 using namespace sf4e;
 JoinedRoom(h,epoch);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;h.view.room.revision=3;
 for(auto& table:h.view.room.tables){table=room::Table{};table.revision=7;}
 for(std::size_t i=0;i<h.view.room.tables.size();++i)h.view.room.tables[i].id=static_cast<std::uint8_t>(i);
 room::Member local;local.id=1;local.name="Local";local.table=1;local.seat=0;local.status=room::MemberStatus::Seated;
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=1;peer.seat=1;peer.status=room::MemberStatus::Seated;
 h.view.room.members={local,peer};
 auto& table=h.view.room.tables[1];
 table.p1=1;table.p2=2;table.phase=room::TablePhase::Waiting;
 h.view.canReady=true;
}
// The board on Room, its focus on the player's own card with nothing on it lit.
void OwnCard(Harness& h){
 auto& nav=h.shell.Navigation();
 if(nav.Screen()!="room")h.Screen("room");
 nav.Prefer("ready");h.Frame();nav.Prefer("table-1");h.Frame(0,2);
 Check(nav.Focus()=="table-1"&&nav.Lit("table-1").empty(),"The own card could not be focused unlit");
}
// A on the own card readies once. A room joined at once after, at the same
// table and revision, starts without that offer: its first A readies too.
void OfferStaysInItsRoom(){
 Harness h;h.Frame();
 SeatAtTableOne(h,26);h.Frame(0,3);OwnCard(h);
 auto sent=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==sent+1&&ReadyIs(h.actions.back()),"A on your own card did not ready");
 SeatAtTableOne(h,27);h.Frame();OwnCard(h);
 sent=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==sent+1&&ReadyIs(h.actions.back()),"The last room's Ready swallowed the first A on the new room's own card");
}
// Ten minutes in, A on the own card readies; the room answers, and a second
// later the interface clock restarts. A again is a new press, not the last
// one pressed again: it unreadies.
void OfferAcrossRestart(){
 Harness h;h.Frame();
 SeatAtTableOne(h,28);h.Wait(600);OwnCard(h);
 auto sent=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==sent+1&&ReadyIs(h.actions.back()),"A on your own card did not ready");
 auto& table=h.view.room.tables[1];table.ready[0]=true;++table.revision;h.Wait(1);
 Restart();
 sent=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==sent+1&&RoomActionIs(h.actions.back(),sf4e::room::ActionKind::Unready),"After the interface clock restarted, A on a readied card was taken for the last press");
}
// A refusal ends five seconds after it was said, the interface clock
// restarting in between or not.
void RefusalAcrossRestart(){
 using namespace sf4e;
 Harness h;h.Frame();
 SeatAtTableOne(h,29);h.Wait(600);OwnCard(h);
 std::string status;SetMenuStatusProbe([&](const char* text,Tone){status=text;});
 h.accept=false;h.Press(MenuInput::Select);h.accept=true;
 const std::string refused=status;
 Check(refused==loc::T("error.queue_failed"),"A Ready the runtime refused went unsaid");
 Restart();h.Wait(1);
 Check(status==refused,"The interface clock restarting ended a refusal early");
 h.Wait(5);h.Frame();
 Check(status!=refused,"A refusal outlived its five seconds across an interface clock restart");
 SetMenuStatusProbe({});
}
// A draft counts as typing for 4 seconds after its last edit, the interface
// clock restarting in between or not (the room is told, NoteChatTyping).
void TypingAcrossRestart(){
 Harness h;h.Frame();
 SeatAtTableOne(h,30);h.Wait(600);
 h.Screen("room-chat");h.Frame(0,3);
 ImGui::GetIO().AddInputCharactersUTF8("brb");h.Frame(0,2);
 Check(h.shell.ChatTyping(),"Writing a draft did not count as typing");
 Restart();h.Frame();
 Check(h.shell.ChatTyping(),"The interface clock restarting ended typing early");
 h.Wait(4.5f);h.Frame();
 Check(!h.shell.ChatTyping(),"A draft left alone across an interface clock restart still counted as typing");
}
}
void TimingJourneys(){
 OfferStaysInItsRoom();
 OfferAcrossRestart();
 RefusalAcrossRestart();
 TypingAcrossRestart();
}
