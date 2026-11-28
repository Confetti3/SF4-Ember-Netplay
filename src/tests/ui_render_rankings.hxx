#pragma once
// The Rankings shots of ui_render_test.cxx: your class in each state (settled,
// placing, faint, classes hidden, not taking part with the day's wait, no
// Discord account, a service without rankings) and the board by class, with
// a name in Japanese, the longest name and your own row. `draw` and `page` are
// the test's own.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include <memory>
#include <string>
#include <vector>
namespace {
template<class Draw,class Page>
void ShootRankings(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page){
    using namespace sf4e;using namespace sf4e::ui;
    using netplay::rankings::Standing;
    auto& id=view.identity;
    id=netplay::IdentityView{};id.requestId=view.identityRequest;id.known=true;id.state="ready";id.fingerprint="j25zrhe6-pmdhvlja";
    id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.bridges={{"brg_00000000-0000-4000-8000-000000000001","https://bridge.example","Example"}};
    auto& rankings=view.rankings;rankings=netplay::rankings::Status{};rankings.bridge=id.bridges[0].id;
    const auto stand=[&](Standing standing,const char* shot){
        rankings.standing.value=standing;rankings.standing.loaded=true;++rankings.standing.finished;++rankings.standing.revision;
        page("rankings");draw(nullptr,0,6);draw(shot);
        Require(shell.Navigation().Screen()=="rankings","Rankings did not open");};
    const auto ranked=[](std::uint8_t band,bool settled){
        Standing s;s.kind=Standing::Kind::Ranked;s.classesShown=true;s.badge=band?SkillBadge::OfClass(band,settled):SkillBadge::Placing();
        s.listed=true;s.listing={"Player 1",CountryCode::Parsed("GB")};return s;};
    // The shot of the screen with the row `row` focused, as the pad leaves it.
    const auto focus=[&](const char* row,const char* shot){
        shell.Navigation().Prefer(row);draw(nullptr,0,2);
        Require(shell.Navigation().Focus()==row,"A Rankings row did not take the focus");
        draw(shot);};
    stand(ranked(9,true),"rankings-class");
    stand(ranked(0,false),"rankings-placing");
    Standing developer=ranked(0,false);developer.badge=SkillBadge::Developer();stand(developer,"rankings-developer");
    stand(ranked(4,false),"rankings-faint");
    Standing hidden=ranked(5,true);hidden.classesShown=false;hidden.listed=false;stand(hidden,"rankings-hidden");
    Standing out;out.kind=Standing::Kind::OptedOut;out.changeIn=20*3600;stand(out,"rankings-opted-out");
    stand(Standing{},"rankings-no-account");
    rankings.standing=netplay::Answered<Standing>{};rankings.standing.error="ranking_unavailable";++rankings.standing.finished;
    page("rankings");draw(nullptr,0,6);draw("rankings-unavailable");
    // Your rating under your class, and the points board's row.
    netplay::rankings::OwnRating own;own.takesPart=own.settled=true;own.rating=1612.4;own.deviation=61.2;own.volatility=.06;own.band=10;
    own.downBelow=1550.;own.faintInDays=12;own.playedAt=1700000000;
    rankings.rating.value.rating=own;rankings.rating.loaded=true;++rankings.rating.finished;++rankings.rating.revision;
    stand(ranked(10,true),"rankings-grand-master");
    // Your rating's own pane: where the class moves and what the next game would move.
    own.band=7;own.rating=1484.6;own.upAt=1525.;own.downBelow=1440.;
    own.examples={{1702.,10,{.4,9.8},{.4,-3.1}},{1490.,7,{.5,6.2},{.5,-6.4}},{1300.,0,{.3,2.9},{.3,-11.7}}};
    rankings.rating.value.rating=own;++rankings.rating.revision;
    stand(ranked(7,true),"rankings-class-pane");
    focus("rk-rating","rankings-rating");
    // The points board: your line with its card, a name in Japanese, the longest name and every tier.
    auto points=std::make_shared<netplay::rankings::PointsBoard>();points->total=812;
    points->lines.push_back({1,{"Player 2",CountryCode::Parsed("JP")},10,48210,4,"card-2"});
    points->lines.push_back({2,{"Player 1",CountryCode::Parsed("GB")},10,31877,3,"card-1"});
    points->lines.push_back({3,{"プレイヤー3",CountryCode::Parsed("JP")},7,12040,2,""});
    points->lines.push_back({4,{std::string(32,'W'),CountryCode::Parsed("BR")},7,5300,1,""});
    points->lines.push_back({5,{"Player 5",CountryCode()},0,120,0,""});
    rankings.points.value=points;rankings.points.loaded=true;++rankings.points.finished;++rankings.points.revision;
    shell.Navigation().Push("rankings-points");draw(nullptr,0,6);draw("rankings-points");
    focus("rkp-p:1","rankings-points-player");
    page("rankings");draw(nullptr,0,4);
    // The board: your class, 7, among others, the crowns at the top.
    auto board=std::make_shared<std::vector<netplay::rankings::BoardEntry>>();
    board->push_back({{"Player 2",CountryCode::Parsed("JP")},10,false});
    board->push_back({{"Player 6",CountryCode::Parsed("US")},10,false});
    board->push_back({{"Player 7",CountryCode::Parsed("KR")},9,false});
    board->push_back({{"プレイヤー3",CountryCode::Parsed("JP")},7,false});
    board->push_back({{"Player 1",CountryCode::Parsed("GB")},7,true});
    board->push_back({{std::string(32,'W'),CountryCode::Parsed("BR")},7,false});
    board->push_back({{"Player 8",CountryCode::Parsed("FR")},7,false});
    board->push_back({{"Player 9",CountryCode::Parsed("DE")},5,false});
    board->push_back({{"Player 5",CountryCode()},1,false});
    rankings.board.value=board;rankings.board.loaded=true;++rankings.board.finished;++rankings.board.revision;
    shell.Navigation().Push("rankings-board");draw(nullptr,0,6);draw("rankings-board");
    // In the class's players, on your own row; then the crowns' class from the side.
    focus("rkb-p:4","rankings-board-player");
    focus("rkb-c:10","rankings-board-crowns");
    rankings.board.value=std::make_shared<std::vector<netplay::rankings::BoardEntry>>();++rankings.board.finished;++rankings.board.revision;
    draw(nullptr,0,4);draw("rankings-board-empty");
    rankings=netplay::rankings::Status{};
}
}
