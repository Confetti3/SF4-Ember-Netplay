#pragma once
// The Profile shots of ui_render_test.cxx: the Card and Fighters tabs with
// levels (twelve fighters, the main at 42), with the main at 60, with nothing
// played yet, with levels hidden from others, without a Discord account, on a
// service that does not count levels yet, and while the levels are read.
// `draw` and `page` are the test's own.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include <string>
namespace {
using ProfileOwn=sf4e::netplay::levels::Own;
// A level L begins at (L - 1)(L + 3) points and the next at L(L + 4); a point
// is a game, and a win is another.
inline void ProfilePlay(ProfileOwn& own,int fighter,int level,double part){
    auto& l=own.fighters[fighter];
    l.level=static_cast<std::uint8_t>(level);
    l.floor=static_cast<std::uint64_t>((level-1)*(level+3));
    l.next=level==sf4e::netplay::levels::MaxLevel?0:static_cast<std::uint64_t>(level*(level+4));
    const std::uint64_t points=l.floor+(l.next?static_cast<std::uint64_t>((l.next-l.floor)*part):0);
    l.games=static_cast<std::uint64_t>(points/1.5);l.wins=points-l.games;l.since=1700000000+static_cast<std::uint64_t>(fighter)*86400;
}
inline ProfileOwn ProfilePlayed(int mainLevel){
    ProfileOwn own;own.kind=ProfileOwn::Kind::Counted;own.shown=true;
    ProfilePlay(own,0,mainLevel,.7);
    const int others[][2]={{5,25},{12,18},{7,13},{20,9},{3,6},{30,3},{40,2},{17,1},{9,1},{26,1},{33,1}};
    for(const auto& pair:others)ProfilePlay(own,pair[0],pair[1],.35);
    return own;
}
template<class Draw,class Page>
void ShootProfile(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page){
    using namespace sf4e;using namespace sf4e::ui;
    view.preferences.mainFighter=0;
    const auto shoot=[&](const char* name,const ProfileOwn& own,bool loaded,bool loading){
        view.levels=netplay::Answered<ProfileOwn>{};view.levels.value=own;view.levels.loaded=loaded;view.levels.loading=loading;
        ++view.levels.revision;++view.levels.finished;
        for(const char* screen:{"card","fighters"}){
            page(screen);draw(nullptr,0,6);draw((std::string(screen)+"-"+name).c_str());
            Require(shell.Navigation().Screen()==screen,"Profile did not open");
        }
    };
    shoot("played",ProfilePlayed(42),true,false);
    shoot("sixty",ProfilePlayed(netplay::levels::MaxLevel),true,false);
    ProfileOwn hidden=ProfilePlayed(42);hidden.shown=false;shoot("hidden",hidden,true,false);
    ProfileOwn fresh;fresh.kind=ProfileOwn::Kind::Counted;fresh.shown=true;shoot("fresh",fresh,true,false);
    ProfileOwn none;none.kind=ProfileOwn::Kind::NoAccount;shoot("no-account",none,true,false);
    none.kind=ProfileOwn::Kind::Unavailable;shoot("unavailable",none,true,false);
    shoot("loading",ProfileOwn{},false,true);
    view.levels=netplay::Answered<ProfileOwn>{};
}
}
