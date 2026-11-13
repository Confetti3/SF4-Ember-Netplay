#pragma once
// The Profile shots of ui_render_test.cxx: the Card and Fighters tabs with
// points (twelve fighters, the main at 1986), with the main in the top tier, with nothing
// played yet, without a Discord account, on a service that does not count
// points yet, while the points are read and when the service cannot be reached;
// then Edit card and Home's card with points. `draw` and `page` are the test's own.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include <string>
namespace {
using ProfileOwn=sf4e::netplay::levels::Own;
// The tiers begin at 0, 200, 700, 1500 and 2500 points, as the bridge decides
// them; a point is a game, and a win is another.
inline void ProfilePlay(ProfileOwn& own,int fighter,std::uint64_t points){
    const std::uint64_t starts[]={0,200,700,1500,2500};
    auto& l=own.fighters[fighter];
    l.points=points;l.tier=0;
    while(l.tier+1<sf4e::netplay::levels::TierCount&&points>=starts[l.tier+1])++l.tier;
    l.floor=starts[l.tier];
    l.next=l.tier+1<sf4e::netplay::levels::TierCount?starts[l.tier+1]:0;
    l.games=static_cast<std::uint64_t>(points/1.5);l.wins=points-l.games;l.since=1700000000+static_cast<std::uint64_t>(fighter)*86400;
}
// The account's frame from its points, as the bridge decides it.
inline void ProfileAccount(ProfileOwn& own){
    const std::uint64_t starts[]={0,500,2000,5000,10000};
    auto& l=own.account;
    l.tier=0;
    while(l.tier+1<sf4e::netplay::levels::TierCount&&l.points>=starts[l.tier+1])++l.tier;
    l.floor=starts[l.tier];
    l.next=l.tier+1<sf4e::netplay::levels::TierCount?starts[l.tier+1]:0;
    l.since=1700000000;
}
inline ProfileOwn ProfilePlayed(std::uint64_t mainPoints){
    ProfileOwn own;own.kind=ProfileOwn::Kind::Counted;
    ProfilePlay(own,0,mainPoints);
    const std::uint64_t others[][2]={{5,3480},{12,860},{7,412},{20,230},{3,86},{30,41},{40,19},{17,7},{9,3},{26,2},{33,1}};
    for(const auto& pair:others)ProfilePlay(own,static_cast<int>(pair[0]),pair[1]);
    // The account's points are every game of every fighter, on its own tiers (500, 2000, 5000, 10000).
    for(const auto& level:own.fighters){own.account.points+=level.points;own.account.games+=level.games;own.account.wins+=level.wins;}
    // The award counters and the opponent played most, which the awards and the Most played part read.
    own.awards.games=own.account.games;own.awards.wins=own.account.wins;own.awards.bestStreak=7;own.awards.peakClass=6;own.awards.rival=40;own.awards.opponents=60;
    own.hasMostPlayed=true;
    own.mostPlayed.card="c1.lab_rival";own.mostPlayed.name="Player 7";own.mostPlayed.country=sf4e::CountryCode::Parsed("US");
    own.mostPlayed.badge=sf4e::SkillBadge::OfClass(8,true);own.mostPlayed.won=41;own.mostPlayed.lost=33;
    return own;
}
template<class Draw,class Page>
void ShootProfile(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page){
    using namespace sf4e;using namespace sf4e::ui;
    view.preferences.mainFighter=0;
    // A trusted service and the card's data: the sets are asked of it by the player's name, how the player plays is shown.
    view.identity.known=true;view.identity.state="ready";view.identity.emberId="emb1_lab";view.identity.bridges={{"brg_1","https://bridge.example","Example"}};
    view.preferences.displayName="Player 1";view.preferences.cardDevice=2;view.preferences.cardLooking=netplay::profiles::LookingLongSets|netplay::profiles::LookingTeach;
    view.preferences.wornTitle="wins";view.preferences.wornIcon="matches";view.ownLink=NetworkLink::Wired;view.replays.ready=true;
    view.profileSets.bridge="brg_1";view.profileSets.filter=netplay::archive::Filter::Named("Player 1");
    view.profileSets.state=netplay::archive::Status::State::Loading;
    const auto shoot=[&](const char* name,const ProfileOwn& own,bool loaded,bool loading){
        view.levels=netplay::Answered<ProfileOwn>{};view.levels.value=own;view.levels.loaded=loaded;view.levels.loading=loading;
        // Not loaded and not loading is a read that failed with nothing before it.
        if(!loaded&&!loading)view.levels.error="bridge_unreachable";
        ++view.levels.revision;++view.levels.finished;
        for(const char* screen:{"card","fighters"}){
            page(screen);draw(nullptr,0,6);draw((std::string(screen)+"-"+name).c_str());
            Require(shell.Navigation().Screen()==screen,"Profile did not open");
        }
    };
    shoot("played",ProfilePlayed(1986),true,false);
    // Without a most played opponent, and the recent sets in each state the part has (the rows themselves are the lab's --sets full).
    auto noMost=ProfilePlayed(1986);noMost.hasMostPlayed=false;shoot("no-most",noMost,true,false);
    {
        namespace ar=netplay::archive;
        for(const auto state:{ar::Status::State::Failed,ar::Status::State::Unsupported,ar::Status::State::Listed}){
            view.profileSets.state=state;++view.profileSets.revision;
            if(state==ar::Status::State::Listed)view.profileSets.listing=std::make_shared<const ar::Listing>();
            shoot(state==ar::Status::State::Failed?"sets-failed":state==ar::Status::State::Unsupported?"sets-unsupported":"sets-none",ProfilePlayed(1986),true,false);
        }
        view.profileSets.state=ar::Status::State::Loading;view.profileSets.listing.reset();
    }
    // The Panel in the awards tab's preview, and the Full card at the lab's other sizes come from the same code.
    page("awards");draw(nullptr,0,6);draw("awards-played");
    Require(shell.Navigation().Screen()=="awards","Awards did not open");
    // Edit card with the player's card as others see it, and Home's card with its points lines.
    page("card-edit");draw(nullptr,0,6);draw("card-edit-played");
    Require(shell.Navigation().Screen()=="card-edit","Edit card did not open");
    page("home");draw(nullptr,0,6);draw("home-profile-played");
    shoot("sixty",ProfilePlayed(12840),true,false);
    ProfileOwn fresh;fresh.kind=ProfileOwn::Kind::Counted;ProfileAccount(fresh);shoot("fresh",fresh,true,false);
    ProfileOwn none;none.kind=ProfileOwn::Kind::NoAccount;shoot("no-account",none,true,false);
    none.kind=ProfileOwn::Kind::Unavailable;shoot("unavailable",none,true,false);
    shoot("loading",ProfileOwn{},false,true);
    shoot("unreachable",ProfileOwn{},false,false);
    view.levels=netplay::Answered<ProfileOwn>{};
}
}
