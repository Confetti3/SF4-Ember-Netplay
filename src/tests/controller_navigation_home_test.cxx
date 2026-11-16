// How a list wraps, Home's aside entry (an entry its menu draws beside the list is a column of its own) and the profile
// record's win and loss counting. Linked into the ControllerNavigation test, whose main runs them in its order.
#include "../ui/MenuRows.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include <stdexcept>
#include <string>
#include <vector>
using namespace sf4e::ui;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}
// A list wraps only on a fresh press: Down held from the top walks to the
// last entry and stays there, however long it is held, and Up held from the
// bottom stays at the first. A grid never wraps. A list's wrap passes over an
// entry that stands beside it, named as another's Left or Right (Home's card).
void ListWrap(){
 std::vector<MenuEntry> rows={Row("a","A",""),Row("b","B",""),Row("c","C",""),Row("d","D","")};
 MenuNavigation nav;double time=0;
 const auto hold=[&](unsigned held,double seconds){for(double end=time+seconds;time<end;){time+=.016;nav.Update({held,time},rows);}time+=.016;nav.Update({0,time},rows);};
 time+=.016;nav.Update({0,time},rows);time+=.016;nav.Update({0,time},rows);
 hold(MenuInput::Down,3);Check(nav.Focus()=="d","A held Down went round the end of a list");
 hold(MenuInput::Up,3);Check(nav.Focus()=="a","A held Up went round the top of a list");
 hold(MenuInput::Up,.05);Check(nav.Focus()=="d","A fresh Up at the top did not go round");
 hold(MenuInput::Down,.05);Check(nav.Focus()=="a","A fresh Down at the bottom did not go round");
 // A grid three across stays at its edges.
 nav.Focus("a",rows);time+=.016;nav.Update({0,time},rows,3);time+=.016;nav.Update({MenuInput::Up,time},rows,3);
 Check(nav.Focus()=="a","A grid went round its top");
 // Home: the rows name the card on their Right; Up from the first goes to the
 // last row, not to the card.
 for(auto& r:rows)r.right="card";
 rows.push_back(Row("card","Card",""));rows.back().left="a";
 nav.SetAside({"card"});
 nav.Focus("a",rows);time+=.016;nav.Update({0,time},rows);hold(MenuInput::Up,.05);
 Check(nav.Focus()=="d","A list's wrap landed on the card beside it");
 nav.Focus("d",rows);hold(MenuInput::Down,.05);
 Check(nav.Focus()=="a","Down from the list's last entry did not go round, over the card beside it, to its first");
 // The narrow window draws the card as the list's last row: the same links, no
 // aside, so a wrap from the top reaches it and the list's wrap is one rule.
 nav.SetAside({});
 nav.Focus("a",rows);time+=.016;nav.Update({0,time},rows);hold(MenuInput::Up,.05);
 Check(nav.Focus()=="card","Up from the top row skipped the card drawn as the list's last row");
 nav.Focus("card",rows);hold(MenuInput::Down,.05);
 Check(nav.Focus()=="a","Down from the card drawn as a row did not go round to the top");
}
// An entry its menu draws beside the list (Home's card in its pane) is a
// column of its own: Up and Down keep to it, the list's Up and Down pass over
// it and go round on a fresh press only, and Left and Right still reach it.
void AsideEntry(){
 std::vector<MenuEntry> rows={Row("a","A",""),Row("b","B",""),Row("card","Card",""),Row("d","D","")};
 for(auto& r:rows)if(r.id=="card")r.left="a";else r.right="card";
 MenuNavigation nav;double time=0;
 const auto hold=[&](unsigned held,double seconds){for(double end=time+seconds;time<end;){time+=.016;nav.Update({held,time},rows);}time+=.016;nav.Update({0,time},rows);};
 time+=.016;nav.Update({0,time},rows);time+=.016;nav.Update({0,time},rows);
 nav.SetAside({"card"});
 nav.Focus("b",rows);hold(MenuInput::Down,.05);Check(nav.Focus()=="d","Down in the list stopped on the entry beside it");
 hold(MenuInput::Up,.05);Check(nav.Focus()=="b","Up in the list stopped on the entry beside it");
 nav.Focus("d",rows);hold(MenuInput::Down,.05);Check(nav.Focus()=="a","A fresh Down at the list's foot did not go round to its top");
 nav.Focus("a",rows);hold(MenuInput::Down,3);Check(nav.Focus()=="d","A held Down went round the list or onto the entry beside it");
 nav.Focus("card",rows);hold(MenuInput::Down,.05);Check(nav.Focus()=="card","Down on the entry beside the list stepped into the list");
 hold(MenuInput::Up,.05);Check(nav.Focus()=="card","Up on the entry beside the list stepped into the list");
 hold(MenuInput::Left,.05);Check(nav.Focus()=="a","Left from the entry beside the list did not go back to it");
 hold(MenuInput::Right,.05);Check(nav.Focus()=="card","Right from the list did not reach the entry beside it");
 // Drawn in the list (narrow), it is one of its entries again.
 nav.SetAside({});nav.Focus("b",rows);hold(MenuInput::Down,.05);Check(nav.Focus()=="card","Down did not reach the card drawn in the list");
}
void ProfileRecords(){
 using namespace sf4e;netplay::ProfileRecord record;netplay::RandomRoomId roomA{};netplay::RandomRoomId roomB{};roomA[0]=1;roomB[0]=2;
 Check(record.Record(roomA,0,1,0,room::MatchResult::P1Win)&&record.wins==1,"Confirmed win not counted");
 Check(!record.Record(roomA,0,1,0,room::MatchResult::P1Win),"Duplicate result counted");
 Check(record.Record(roomB,0,1,0,room::MatchResult::P2Win)&&record.losses==1,"Distinct rooms with the same table/generation collided");
 for(auto result:{room::MatchResult::Draw,room::MatchResult::Abort,room::MatchResult::Cancel})Check(!record.Record(roomA,0,3,0,result),"Non-decision counted");
 Check(!record.Record(roomA,0,3,2,room::MatchResult::P1Win),"Spectator game counted");
 netplay::ProfileRecord restored;Check(netplay::ReadProfileRecord(nlohmann::json::parse(netplay::ProfileRecordJson(record).dump()),restored)&&restored.wins==1&&restored.losses==1,"Record did not round trip");
 Check(!restored.Record(roomA,0,1,0,room::MatchResult::P1Win),"Reload lost deduplication");
 Check(netplay::ReadProfileRecord({{"wins",std::uint64_t(2)},{"losses",std::uint64_t(1)},{"recent",{"1:0:1",netplay::ProfileResultKeyV2(roomB,0,1)}}},restored),"Legacy recent key was not readable");
 Check(!netplay::ReadProfileRecord({{"wins",std::uint64_t(0)},{"losses",std::uint64_t(0)},{"recent",{std::string(65,'x')}}},restored)&&!restored.available,"Recent key bound was not enforced");
 Check(!netplay::ReadProfileRecord({{"wins",-1},{"losses",0},{"recent",nlohmann::json::array()}},restored)&&!restored.available,"Invalid record accepted");
 netplay::ProfileRecord atLimit;atLimit.wins=netplay::ProfileRecord::MaximumGames/2;atLimit.losses=netplay::ProfileRecord::MaximumGames/2-1;
 Check(atLimit.Record(roomA,0,9,0,room::MatchResult::P1Win)&&atLimit.wins+atLimit.losses==netplay::ProfileRecord::MaximumGames,
  "Cap-minus-one record did not reach the exact total limit");
  Check(netplay::ReadProfileRecord(netplay::ProfileRecordJson(atLimit),restored)&&
  restored.wins==netplay::ProfileRecord::MaximumGames/2+1&&restored.losses==netplay::ProfileRecord::MaximumGames/2-1,
  "Exact persisted game limit was not readable");
 Check(!atLimit.Record(roomA,0,10,0,room::MatchResult::P1Win)&&atLimit.wins+atLimit.losses==netplay::ProfileRecord::MaximumGames,
  "Record after the exact total limit was accepted");
}
