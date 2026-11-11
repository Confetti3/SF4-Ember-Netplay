#pragma once
// The room shots of fighter points (PLAYER_CARD.md, "Levels on the room board"):
// a public room whose members have cards (one has played nothing, one has no
// card yet, this PC's own come from the player's own points), the same
// room as a private one, which shows none, and the Members screen. The room,
// the cards and the player's points are put back at the end.
#include "../ui/ApplicationShell.hxx"
#include <map>
#include <memory>
#include <string>
namespace {
template<class Draw,class Page>
void ShootRoomPoints(sf4e::ui::ShellView& view,const Draw& draw,const Page& page) {
    using namespace sf4e;
    const auto room=view.room;const auto levels=view.levels;const auto cards=view.memberCards;
    // The tiers begin at 0, 200, 700, 1500 and 2500 points, as the bridge decides them.
    const std::uint64_t starts[netplay::levels::TierCount]={0,200,700,1500,2500};
    const std::uint64_t spread[]={3480,1986,860,412,230,86,19};
    const auto tierOf=[&](std::uint64_t points){std::uint8_t tier=0;while(tier+1<netplay::levels::TierCount&&points>=starts[tier+1])++tier;return tier;};
    auto map=std::make_shared<netplay::levels::CardMap>();
    for(auto& member:view.room.members){
        if(member.id==view.room.localMember)continue;
        member.card=CardToken::Parsed("c1.player_"+std::to_string(member.id));
        // Player 2 has played nothing and Player 6 has no card yet.
        if(member.id==6)continue;
        netplay::levels::Card card;
        std::map<int,std::uint64_t> played{{member.fighter,spread[member.id%7]},{member.mainFighter,spread[(member.id+3)%7]},{(member.id*5)%44,41}};
        if(member.id==2)played.clear();
        std::uint64_t account=0;
        for(const auto& one:played){card.fighters.push_back({static_cast<std::uint8_t>(one.first),one.second,tierOf(one.second)});account+=one.second;}
        card.account={account,0};
        (*map)[member.card.ToString()]=card;
    }
    view.memberCards=map;
    // This PC's own points: their pick (Ryu) at 1986 and a few others.
    view.levels.loaded=true;view.levels.value.kind=netplay::levels::Own::Kind::Counted;view.levels.value.account.points=2331;view.levels.value.account.games=1500;
    for(const auto& one:std::map<int,std::uint64_t>{{0,1986},{5,412},{12,19}}){
        auto& level=view.levels.value.fighters[one.first];level.points=one.second;level.tier=tierOf(one.second);level.games=one.second/2;
    }
    view.room.serverOwned=true;
    page("room");draw("room-points");
    page("room-members");draw("room-points-members");
    view.room.serverOwned=false;
    page("room");draw("room-points-private");
    view.room=room;view.levels=levels;view.memberCards=cards;
}
}
