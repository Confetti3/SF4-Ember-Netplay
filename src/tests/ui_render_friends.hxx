#pragma once
// The Friends shots of ui_render_test.cxx: the tab in each state the lab has (a full list, requests only, an empty
// list, no Discord account, a service without friends, the first read in flight, the last sync failed and appearing
// offline), then Add a friend, Sent, Blocked, More open on a friend and the question a Join asks. The friends are the
// UI lab's (src/ui/lab/UiLabSocial.cxx), so the shots and the lab show the same people. `draw` and `page` are the
// test's own.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/lab/UiLabSocial.hxx"
#include <string>
namespace {
template<class Draw,class Page>
void ShootFriends(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page){
    using namespace sf4e;using namespace sf4e::ui;
    // The lab's service and player stand in for the shots' own, and are given back.
    const auto identity=view.identity;const auto name=view.preferences.displayName;const auto clock=view.unixNow;
    for(const char* state:{"full","requests","empty","noaccount","unavailable","loading","unreachable","offline"}){
        Require(uilab::Friends(view,state),"The lab has no such friends state");
        page("friends");draw(nullptr,0,6);draw((std::string("friends-")+state).c_str());
        Require(shell.Navigation().Screen()=="friends","Friends did not open");
    }
    Require(uilab::Friends(view,"full"),"The lab has no full friends state");
    for(const char* screen:{"friends-add","friends-sent","friends-blocked"}){
        page(screen);draw(nullptr,0,6);draw(screen);
        Require(shell.Navigation().Screen()==screen,"A page over Friends did not open");
    }
    // More on a friend who is fighting in a room that can be joined.
    page("friends");shell.Navigation().Prefer("friend:fr_3");draw(nullptr,0,6);
    draw(nullptr,MenuInput::Options,1);draw(nullptr,0,4);draw("friends-more");
    Require(shell.Navigation().Choosing(),"More did not open on a friend");
    draw(nullptr,MenuInput::Back,1);draw(nullptr,0,3);
    // A Join that asks first, over the tab.
    auto& visit=view.friends.visit;
    visit.active=visit.confirm=true;visit.revision=1;visit.name="Player 3";
    draw(nullptr,0,6);draw("friends-visit");
    Require(shell.Navigation().Screen()=="friends-visit","A visit that asks first did not open its question");
    visit={};draw(nullptr,0,6);
    view.friends={};view.identity=identity;view.preferences.displayName=name;view.unixNow=clock;
    shell.Navigation().Home();draw(nullptr,0,4);
}
}
