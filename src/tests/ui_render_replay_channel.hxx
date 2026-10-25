#pragma once
// The Replay channel shots of ui_render_test.cxx: the channel with sharing on
// (classes, the developer mark, no class, a name in Japanese and the longest
// name, Load more), sharing off, no Discord account with an
// empty channel, and a service without the channel. `draw` and `page` are
// the test's own.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include <memory>
#include <string>
#include <vector>
namespace {
template<class Draw,class Page>
void ShootReplayChannel(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page){
    using namespace sf4e;using namespace sf4e::ui;
    using netplay::channel::Entry;using netplay::channel::Page;using netplay::channel::Sharing;
    auto& id=view.identity;
    id=netplay::IdentityView{};id.requestId=view.identityRequest;id.known=true;id.state="ready";id.fingerprint="j25zrhe6-pmdhvlja";
    id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.bridges={{"brg_00000000-0000-4000-8000-000000000001","https://bridge.example","Example"}};
    auto& channel=view.replayChannel;channel=netplay::channel::Status{};channel.bridge=id.bridges[0].id;
    const auto game=[](char tag,std::uint64_t at,const char* one,int f1,SkillBadge b1,const char* country1,const std::string& two,int f2,SkillBadge b2,int winner,bool publicRoom){
        Entry e;e.id=std::string(32,tag);e.playedAt=at;e.publicRoom=publicRoom;e.winner=winner;e.size=9000;
        e.sides[0].name=one;e.sides[0].fighter=f1;e.sides[0].badge=b1;e.sides[0].country=CountryCode::Parsed(country1);
        e.sides[1].name=two;e.sides[1].fighter=f2;e.sides[1].badge=b2;return e;};
    auto full=std::make_shared<Page>();
    full->entries={
        game('f',1761402645,"Player 1",23,SkillBadge::OfClass(10,true),"GB","Player 2",39,SkillBadge::OfClass(9,true),1,true),
        game('e',1761401000,"プレイヤー3",1,SkillBadge::OfClass(7,true),"JP",std::string(32,'W'),2,SkillBadge{},0,true),
        game('d',1761399000,"Player 5",31,SkillBadge::Developer(),"US","Player 6",12,SkillBadge::Placing(),-1,false),
        game('c',1761397000,"Player 7",0,SkillBadge::OfClass(3,false),"BR","Player 8",43,SkillBadge::OfClass(1,true),0,true)};
    full->more=true;
    const auto show=[&](Sharing sharing,std::shared_ptr<const Page> shown,int fighter,const char* shot){
        channel.sharing.value=sharing;channel.sharing.loaded=true;++channel.sharing.finished;++channel.sharing.revision;
        channel.page.value=shown;channel.page.loaded=true;channel.page.error.clear();channel.fighter=fighter;++channel.page.finished;++channel.page.revision;
        page("replay-channel");draw(nullptr,0,6);draw(shot);
        Require(shell.Navigation().Screen()=="replay-channel","The Replay channel did not open");};
    view.replays.ready=true;
    channel.queued=3;
    show(Sharing::On,full,-1,"replay-channel");
    channel.queued=0;
    show(Sharing::Off,full,-1,"replay-channel-off");
    show(Sharing::NoAccount,std::make_shared<Page>(),-1,"replay-channel-no-account");
    // A page that failed: the service has no channel.
    channel.page=netplay::rankings::Answered<std::shared_ptr<const Page>>{};channel.page.error="replay_channel_unavailable";++channel.page.finished;
    page("replay-channel");draw(nullptr,0,6);draw("replay-channel-unavailable");
    channel=netplay::channel::Status{};
}
}
