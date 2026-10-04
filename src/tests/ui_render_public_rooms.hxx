#pragma once
// The Public rooms shots of ui_render_test.cxx, which includes this after its
// Require. `draw` and `page` are the test's own, `retheme` applies the theme
// again with the device objects remade, and the flags are the ones the
// test's submit reads: answer room tickets, hold identity requests, and the
// ticket of the last one held.
#include "../ui/ApplicationShell.hxx"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
namespace {
// Public rooms: the list (a room with a match on and its faces and rules, one named in
// kanji, one with the longest name, a locked one, a full one, one of nine members, one in
// another region, one from a bridge that sends no details), the focused card, joining,
// opening, loading, the error, none, none under a filter, the setup cards, and the
// room's link on the board of a public room joined from the list.
template<class Draw,class Page,class Retheme>
void ShootPublicRooms(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page,const Retheme& retheme,
    bool& answerTickets,bool& holdIdentity,const std::uint64_t& heldTicket){
    using namespace sf4e;using namespace sf4e::ui;
    auto& id=view.identity;
    // Keep the answered request id, or the panel waits on Checking forever.
    id=netplay::IdentityView{};id.requestId=view.identityRequest;id.known=true;id.state="ready";id.fingerprint="j25zrhe6-pmdhvlja";
    id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.bridges={{"brg_00000000-0000-4000-8000-000000000001","https://bridge.example","Example"}};
    view.netReport.reported=true;view.netReport.relay="use1";
    auto& publicRooms=view.publicRooms;publicRooms=netplay::publicrooms::Status{};
    const std::string service=id.bridges[0].id;
    publicRooms.listedAt=100000;
    const auto listed=[](const char* roomId,std::string name,unsigned members,unsigned capacity,unsigned playing,const char* region="use1"){
        netplay::publicrooms::Room room;room.id=roomId;room.name=std::move(name);room.members=members;room.capacity=capacity;room.playing=playing;room.region=region;
        room.createdAt=100000-1500;return room;};
    const auto detailed=[](netplay::publicrooms::Room room,std::string host,std::vector<int> fighters,int format,int rotation){
        room.hasDetails=true;room.hostName=std::move(host);room.fighters=std::move(fighters);room.setFormat=format;room.rotation=rotation;return room;};
    const auto allRooms=[&]{
        std::vector<netplay::publicrooms::Room> rooms;
        rooms.push_back(detailed(listed("a","Friday Night Fights",3,8,1),"Kate",{1,6,11},2,0));
        rooms.push_back(detailed(listed("b","金曜ルーム",2,4,0),"山田",{3,-1},0,0));
        rooms.push_back(detailed(listed("c",std::string(64,'W'),16,16,2),"A moderator with a very long name",{0,5,9,14,20,25},3,1));
        rooms.push_back(detailed(listed("d","Locked Dojo",2,8,0),"Kate",{4,8},5,2));
        rooms.back().locked=true;
        rooms.push_back(detailed(listed("e","Full House",8,8,0),"Gouken",{2,7,12,17,22,27,32,37},1,0));
        rooms.push_back(detailed(listed("f","Big Lobby",9,16,0),"Kate",{1,2,3,4,5,6,7,8,9},0,0));
        rooms.push_back(detailed(listed("g","Euro Night",4,8,0,"euc1"),"Hugo",{10,15,20,25},2,1));
        rooms.push_back(listed("h","Old Bridge Room",2,8,0,"aps1"));
        return rooms;};
    const auto settleList=[&](std::vector<netplay::publicrooms::Room> rooms,const std::string& error=std::string()){
        publicRooms.bridge=service;publicRooms.rooms=std::move(rooms);publicRooms.error=error;publicRooms.loading=false;++publicRooms.listed;};
    // Each look starts from Home, so a request still waiting from the last one is dropped.
    const auto visit=[&]{shell.Navigation().Home();draw(nullptr,0,2);page("public-rooms");};
    // The first look at the service: nothing listed yet.
    publicRooms.loading=true;
    visit();draw(nullptr,0,8);draw("public-rooms-loading");
    Require(shell.Navigation().Screen()=="public-rooms","Public rooms did not open");
    settleList(allRooms());
    draw(nullptr,0,4);retheme();draw(nullptr,0,8);draw("public-rooms-list");
    for(int i=0;i<12&&shell.Navigation().Focus().compare(0,8,"pr-room:")!=0;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
    Require(shell.Navigation().Focus().compare(0,8,"pr-room:")==0,"Down did not reach a public room card");
    draw("public-rooms-focus");
    // Joining: the ticket is asked for and never answered. Leaving the screen drops it.
    draw(nullptr,MenuInput::Select,1);draw(nullptr,0,3);draw("public-rooms-joining");
    // Opening: the ticket is answered, and the session starts opening the room.
    visit();draw(nullptr,0,4);
    for(int i=0;i<12&&shell.Navigation().Focus().compare(0,8,"pr-room:")!=0;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
    answerTickets=true;
    draw(nullptr,MenuInput::Select,1);draw(nullptr,0,6);
    view.session.room=netplay::RoomState::Opening;draw(nullptr,0,3);draw("public-rooms-opening");
    view.session.room=netplay::RoomState::Idle;draw(nullptr,0,3);answerTickets=false;
    // The service answered with a failure, then with no rooms.
    settleList({},"unavailable");visit();draw(nullptr,0,8);draw("public-rooms-error");
    settleList({});visit();draw(nullptr,0,8);draw("public-rooms-none");
    // A filter that hides every room: Near me, with only far rooms listed.
    {
        std::vector<netplay::publicrooms::Room> distant;
        for(auto room:allRooms())if(room.region!="use1")distant.push_back(room);
        settleList(distant);visit();draw(nullptr,0,8);
        const auto filter=[&](int steps){
            for(int i=0;i<24&&shell.Navigation().Focus()!="pr-filter";++i){
                const auto& at=shell.Navigation().Focus();
                draw(nullptr,at=="pr-quick"||at=="pr-create"?(i%2?MenuInput::Down:MenuInput::Right):at=="pr-paste"?MenuInput::Left:MenuInput::Up,1);draw(nullptr,0,1);
            }
            Require(shell.Navigation().Focus()=="pr-filter","The filter cell is unreachable");
            draw(nullptr,MenuInput::Select,1);draw(nullptr,0,1);
            for(int i=0;i<std::abs(steps);++i){draw(nullptr,steps>0?MenuInput::Right:MenuInput::Left,1);draw(nullptr,0,1);}
            draw(nullptr,MenuInput::Select,1);draw(nullptr,0,3);
        };
        filter(1);draw("public-rooms-none-filtered");
        Require(shell.Navigation().Screen()=="public-rooms","Filtering left Public rooms");
        filter(-1);
    }
    // No service: set up (an Ember ID that can be made here), and an ID that must be unlocked first.
    id.bridges.clear();id.state="disabled";visit();draw(nullptr,0,8);draw("public-rooms-setup");
    id.state="locked";id.backend="passphrase";visit();draw(nullptr,0,8);draw("public-rooms-setup-id");
    id.state="disabled";id.backend.clear();
    // The one-press setup at work, and stopped: the helper's answers are held, then given by hand.
    {
        const bool canEdit=view.canEditPreferences;view.canEditPreferences=true;
        const auto answer=[&](bool ok,const char* failure){
            view.identityTicket=heldTicket;view.identityRequest=view.identity.requestId=heldTicket+1000;
            view.identity.ok=ok;view.identity.failure=failure;draw(nullptr,0,4);view.identity.ok=true;view.identity.failure.clear();};
        // The visit's own reads are answered; only the setup's are held.
        visit();draw(nullptr,0,8);holdIdentity=true;
        for(int i=0;i<4&&shell.Navigation().Focus()!="pr-setup";++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
        Require(shell.Navigation().Focus()=="pr-setup","The setup card is unreachable");
        // Cancel is the dialog's first answer; Right then Select confirms.
        draw(nullptr,MenuInput::Select,1);draw(nullptr,0,2);
        Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),"The setup confirmation does not start on Cancel");
        draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Select,1);draw(nullptr,0,6);
        draw("public-rooms-setup-checking");
        // The ID is ready: the service list is read next, and the card says it is finding Ember's service.
        id.state="ready";id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
        answer(true,"");draw(nullptr,0,6);draw("public-rooms-setup-running");
        // The service did not answer: the card says so, and offers to try again.
        answer(false,"bridge_unreachable");draw(nullptr,0,6);draw("public-rooms-setup-failed");
        holdIdentity=false;view.canEditPreferences=canEdit;id.state="disabled";id.emberId.clear();id.fingerprint.clear();
    }
    // The Ember ID has not answered yet.
    id.known=false;visit();draw(nullptr,0,4);draw("public-rooms-checking");id.known=true;
    id.state="ready";id.bridges={{"brg_00000000-0000-4000-8000-000000000001","https://bridge.example","Example"}};
    id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
    {
        // Inside a public room joined from the list, the board offers the room's link to copy.
        settleList({detailed(listed("0123456789abcdef0123456789abcdef","Friday Night Fights",3,8,1),"Kate",{1,6,11},2,0)});
        visit();draw(nullptr,0,8);
        for(int i=0;i<12&&shell.Navigation().Focus().compare(0,8,"pr-room:")!=0;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
        answerTickets=true;draw(nullptr,MenuInput::Select,1);draw(nullptr,0,6);answerTickets=false;
        view.session.room=netplay::RoomState::Opening;draw(nullptr,0,3);
        view.session.room=netplay::RoomState::Joined;view.room.serverOwned=true;
        std::vector<MenuEntry> boardRows;sf4e::ui::SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){boardRows=r;});
        page("room");draw(nullptr,0,4);draw("room-public-link");sf4e::ui::SetMenuEntriesProbe({});
        Require(std::any_of(boardRows.begin(),boardRows.end(),[](const MenuEntry& e){return e.id=="copy-room-link";}),"A public room's board has no room link row");
        view.room.serverOwned=false;view.session.room=netplay::RoomState::Idle;draw(nullptr,0,3);
    }
}
}
