#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
// What the shell remembers for a while: the table card's last offer (the
// same-press guard), and how long it remembers it.
namespace {
// This player (member 1) seated at table 1 against member 2 in room `epoch`,
// neither ready, every table at revision 7.
void SeatAtTableOne(Harness& h,std::uint64_t epoch){
 using namespace sf4e;
 JoinedRoom(h,epoch);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;
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
}
void TimingJourneys(){
 OfferStaysInItsRoom();
}
