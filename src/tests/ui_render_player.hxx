#pragma once
// The shots of another player's profile for ui_render_test.cxx: the Card tab in each state the lab has (a friend in a joinable
// room, an online friend, a stranger, a request received and sent, a player you blocked, no card, nobody by the name, the card being
// read, the service unreachable and without profiles), the pick list, and the Fighters, Sets and Awards tabs of a card, with
// More open on a friend. The players are the UI lab's (src/ui/lab/UiLabSocial.cxx), so the shots and the lab show the same
// people. `draw` and `page` are the test's own.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/lab/UiLabSocial.hxx"
#include <string>
namespace {
template<class Draw,class Page>
void ShootPlayer(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page){
    using namespace sf4e;using namespace sf4e::ui;
    // The lab's service, friends and players stand in for the shots' own, and are given back.
    const auto identity=view.identity;const auto name=view.preferences.displayName;const auto clock=view.unixNow;
    const auto profile=view.profile;const auto friends=view.friends;const auto service=view.profilesService;
    for(const char* state:{"room","friend","stranger","asked","sent","blocked","nocard","nobody","reading","unreachable","unsupported"}){
        Require(uilab::Player(view,state),"The lab has no such player state");
        page("player-card");draw(nullptr,0,6);draw((std::string("player-card-")+state).c_str());
        Require(shell.Navigation().Screen()=="player-card"||shell.Navigation().Screen()=="player-pick","Their Card tab did not open");
    }
    // The pick list of players that go by one name.
    Require(uilab::Player(view,"several"),"The lab has no several state");
    page("player-pick");draw(nullptr,0,6);draw("player-pick");
    Require(shell.Navigation().Screen()=="player-pick","The pick list did not open");
    // A friend's other tabs, and More open on the Card.
    Require(uilab::Player(view,"room"),"The lab has no room state");
    for(const char* screen:{"player-fighters","player-sets","player-awards"}){
        page(screen);draw(nullptr,0,6);draw(screen);
        Require(shell.Navigation().Screen()==screen,"A tab of their profile did not open");
    }
    page("player-card");draw(nullptr,0,6);
    draw(nullptr,MenuInput::Options,1);draw(nullptr,0,4);draw("player-card-more");
    Require(shell.Navigation().Choosing(),"More did not open on their Card");
    draw(nullptr,MenuInput::Back,1);draw(nullptr,0,3);
    view.profile=profile;view.friends=friends;view.profilesService=service;view.identity=identity;view.preferences.displayName=name;view.unixNow=clock;
    shell.Navigation().Home();draw(nullptr,0,4);
}
}
