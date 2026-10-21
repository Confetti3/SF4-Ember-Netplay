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
    stand(ranked(9,true),"rankings-class");
    stand(ranked(0,false),"rankings-placing");
    Standing developer=ranked(0,false);developer.badge=SkillBadge::Developer();stand(developer,"rankings-developer");
    stand(ranked(4,false),"rankings-faint");
    Standing hidden=ranked(5,true);hidden.classesShown=false;hidden.listed=false;stand(hidden,"rankings-hidden");
    Standing out;out.kind=Standing::Kind::OptedOut;out.changeIn=20*3600;stand(out,"rankings-opted-out");
    stand(Standing{},"rankings-no-account");
    rankings.standing=netplay::rankings::Answered<Standing>{};rankings.standing.error="ranking_unavailable";++rankings.standing.finished;
    page("rankings");draw(nullptr,0,6);draw("rankings-unavailable");
    // The board: your row, two classes, a name in Japanese and the longest name.
    stand(ranked(10,true),"rankings-grand-master");
    auto board=std::make_shared<std::vector<netplay::rankings::BoardEntry>>();
    board->push_back({{"Player 2",CountryCode::Parsed("JP")},10,false});
    board->push_back({{"Player 1",CountryCode::Parsed("GB")},10,true});
    board->push_back({{"プレイヤー3",CountryCode::Parsed("JP")},7,false});
    board->push_back({{std::string(32,'W'),CountryCode::Parsed("BR")},7,false});
    board->push_back({{"Player 5",CountryCode()},1,false});
    rankings.board.value=board;rankings.board.loaded=true;++rankings.board.finished;++rankings.board.revision;
    shell.Navigation().Push("rankings-board");draw(nullptr,0,6);draw("rankings-board");
    rankings.board.value=std::make_shared<std::vector<netplay::rankings::BoardEntry>>();++rankings.board.finished;++rankings.board.revision;
    draw(nullptr,0,4);draw("rankings-board-empty");
    rankings=netplay::rankings::Status{};
}
}
