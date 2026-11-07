// The shell's product rules (Branding::roomFacesArePicks, answersFromInput,
// rankingClasses), built twice: ProductRules under SF4 Ember's identity, where
// they are off and the shell is as it was, and ProductRulesOn under a product
// that turns them on (product_rules_product.cxx). Each run checks the
// behaviour its product's rules ask for.
#include "shell_journey_support.hxx"
#include "../common/ProductIdentity.hxx"
#include "../ui/MenuProbes.hxx"
#include "../ui/NameMarks.hxx"
#include "../ui/RoomPanelBoard.hxx"
#include <vector>

namespace {
using namespace sf4e;

// A joined Classic room, member 1 (this PC) and member 2 seated at table 0.
void SeatedPair(Harness& h,std::uint64_t epoch) {
 JoinedRoom(h,epoch);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;
 h.view.room.host=1;h.view.room.revision=3;
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";peer.table=0;peer.seat=1;h.view.room.members.push_back(peer);
 auto& t=h.view.room.tables[0];t.p1=1;t.p2=2;t.revision=7;t.matchGeneration=5;t.phase=room::TablePhase::Waiting;
}
std::vector<FighterPicture> Drawn(Harness& h) {
 std::vector<FighterPicture> drawn;SetPortraitProbe([&](const FighterPicture& picture,ImVec2,ImVec2){drawn.push_back(picture);});
 h.Frame();SetPortraitProbe({});
 return drawn;
}
std::vector<int> Fighters(const std::vector<FighterPicture>& drawn) {
 std::vector<int> fighters;for(const auto& picture:drawn)fighters.push_back(picture.fighter);return fighters;
}

// The Members rows and the seats. Off, the rows show each member's main and
// an unpicked seat the main; on, both show the pick shared, else no fighter.
// This PC always holds a pick of its own (a default Pick is fighter 0), which
// its own row shows as its seat does; only the peer is unpicked here.
void RoomFaces() {
 const bool picks=ember::Product().branding.roomFacesArePicks;
 Harness h;SeatedPair(h,21);
 h.view.preferences.mainFighter=8;h.view.room.members[1].mainFighter=9;
 h.view.selectedPick=selection::Pick{};h.view.room.members[1].fighter=-1;
 h.Frame();h.Screen("room-members");
 Check(Fighters(Drawn(h))==(picks?std::vector<int>{0,-1}:std::vector<int>{8,9}),
  picks?"With picks only, an unpicked member's row drew a fighter":"An unpicked member's row did not show their main");
 h.Screen("room");
 {const auto seats=Fighters(Drawn(h));
  const auto has=[&](int fighter){return std::find(seats.begin(),seats.end(),fighter)!=seats.end();};
  Check(picks?(!has(8)&&!has(9)&&has(-1)):(has(8)&&has(9)),picks?"With picks only, an unpicked seat drew a main":"An unpicked seat did not show the main");}
 Check(SeatCaption(SeatLookOf(h.view,h.view.room.members[1]))==loc::T("room.fighter_not_shared"),"An unpicked seat was not captioned as not shared");
 // Both pick: this PC's own pick before Ready shares it, the peer's as the room has it.
 h.view.selectedPick.fighter=0;h.view.selectedPick.costume=1;h.view.selectedPick.color=3;
 h.view.room.members[1].fighter=2;h.view.room.members[1].costume=2;h.view.room.members[1].color=4;
 h.Screen("room-members");
 const auto rows=Drawn(h);
 Check(Fighters(rows)==(picks?std::vector<int>{0,2}:std::vector<int>{8,9}),
  picks?"With picks only, the Members rows did not show the picks":"The Members rows did not keep each member's main over their pick");
 if(picks)Check(rows[0].costume==1&&rows[0].color==3&&rows[1].costume==2&&rows[1].color==4,"A Members row did not show the pick's costume and colour");
 else Check(rows[0].costume==0&&rows[0].color==0&&rows[1].costume==0&&rows[1].color==0,"A main's row was not drawn in the original outfit");
 h.Screen("room");
 {const auto seats=Fighters(Drawn(h));
  Check(std::find(seats.begin(),seats.end(),0)!=seats.end()&&std::find(seats.begin(),seats.end(),2)!=seats.end(),"A picked seat did not show the pick");}
}

// The opponent's answer over a WIN that goes on in place. Off, it reads
// Choosing until both chose; on, their Rematch and their decline show as soon
// as the battle's input carries them, and a decline stands once the battle
// has stopped reporting it.
void OpponentAnswer() {
 const bool fromInput=ember::Product().branding.answersFromInput;
 Harness h;SeatedPair(h,22);
 auto& t=h.view.room.tables[0];t.phase=room::TablePhase::Playing;
 h.Screen("room");
 std::map<std::string,std::string> words;SetMenuCardTextProbe([&](const std::string& id,const std::string& text){words[id]=text;});
 const auto win=[&]{h.view.rematchChoices=h.view.rematchDeclines=0;h.view.matchResult=false;h.Frame();
  h.view.session.match=netplay::MatchState::Playing;h.view.atMainMenu=false;h.view.foughtGame={0,t.matchGeneration,room::MatchResult::P1Win};
  h.view.matchResult=true;h.Frame();};
 const auto backInRoom=[&]{h.view.matchResult=false;h.view.rematchChoices=h.view.rematchDeclines=0;h.view.session.match=netplay::MatchState::PostMatch;
  h.view.atMainMenu=true;t.phase=room::TablePhase::Waiting;h.open=true;h.Frame();};
 const std::string choosing=loc::T("postmatch.state.choosing"),ready=loc::T("postmatch.state.ready"),back=loc::T("postmatch.back");
 win();
 Check(h.shell.Navigation().Screen()=="post-match"&&words["pm-opponent"]==choosing,"The WIN's prompt did not open with the opponent choosing");
 // The opponent (P2, bit 2) chose Rematch; this PC has not answered.
 h.view.rematchChoices=2;h.Frame();
 Check(words["pm-opponent"]==(fromInput?ready:choosing),fromInput?"The opponent's Rematch from the input was not shown":"The opponent's Rematch showed before the room had it");
 Check(words["pm-you"]==choosing,"The opponent's Rematch was shown as this PC's");
 // Then declined: the battle decides on it and stops reporting either.
 h.view.rematchChoices=0;h.view.rematchDeclines=2;h.Frame();
 Check(words["pm-opponent"]==(fromInput?back:choosing),fromInput?"The opponent's decline was not shown":"The opponent's decline showed in SF4 Ember");
 h.view.rematchDeclines=0;h.Frame();
 Check(words["pm-opponent"]==(fromInput?back:choosing),"The opponent's decline was lost when the battle stopped reporting it");
 backInRoom();h.Wait(11);
 Check(h.shell.Navigation().Screen()!="post-match","The prompt with no answer of this PC's did not close");
 // This PC chose Rematch, the opponent declined: in the room the Rematch is
 // this player's Ready, and the prompt has nothing left to wait for.
 win();h.Choose("again");
 Check(h.actions.back().command.kind==Kind::Rematch,"Rematch did not send the room's Ready");
 h.view.rematchChoices=1;h.Frame();
 Check(words["pm-you"]==ready&&words["pm-opponent"]==choosing,"This PC's own Rematch was read as the opponent's");
 h.view.rematchChoices=1;h.view.rematchDeclines=2;h.Frame();
 Check(h.shell.Navigation().Screen()=="post-match"&&words["pm-opponent"]==(fromInput?back:choosing),"The decline over a chosen Rematch was not shown as it should be");
 const auto sent=h.actions.size();
 backInRoom();h.Frame();
 Check((h.shell.Navigation().Screen()=="post-match")!=fromInput,
  fromInput?"The prompt waited on over the room after the opponent's decline":"SF4 Ember's prompt did not wait for the opponent's answer");
 Check(std::none_of(h.actions.begin()+static_cast<std::ptrdiff_t>(sent),h.actions.end(),[](const ShellAction& x){return x.roomAction.kind==room::ActionKind::Unqueue;}),
  "A chosen Rematch gave up the seat");
 SetMenuCardTextProbe({});
}
}

// The classes' names: USF4's letters and crowns, or the product's own ranks
// (Branding::rankingClasses), by the game's name or the catalog's.
void ClassNames() {
 const auto* ranks=ember::Product().branding.rankingClasses;
 if(ranks){
  for(std::uint8_t band=1;band<=SkillBadge::Classes;++band){
   const auto& rank=ranks[band-1];
   Check(ui::ClassName(band)==(rank.name?std::string(rank.name):std::string(loc::T(rank.msgid))),"A product's rank was not named as the product names it");
  }
  Check(ui::ClassName(1)=="Rank 1"&&ui::ClassName(SkillBadge::Classes)==std::string(loc::T("class.grand_master")),"A product's ranks were not its own");
 }else{
  Check(ui::ClassName(1)=="D"&&ui::ClassName(2)=="D+"&&ui::ClassName(8)=="A+","USF4's classes lost their letters");
  Check(ui::ClassName(9)==std::string(loc::T("class.master"))&&ui::ClassName(10)==std::string(loc::T("class.grand_master")),"USF4's crowns lost their names");
 }
 Check(ui::ClassName(0).empty()&&ui::ClassName(SkillBadge::Classes+1).empty(),"A band past the classes was named");
}

int main() {
 try {
  RoomFaces();
  OpponentAnswer();
  ClassNames();
  std::cout<<"Product rules ("<<(ember::Product().branding.roomFacesArePicks?"on":"off")<<") passed.\n";
  return 0;
 } catch (const std::exception& e) {
  std::cerr<<e.what()<<'\n';
  return 1;
 }
}
