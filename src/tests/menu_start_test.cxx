#include "../ui/OverlayPresentation.hxx"
#include "test_support.hxx"
#include <iostream>
using namespace sf4e;
using input::MenuContext;
using netplay::MatchState;
using ui::MenuRequest;
static void StartEdges(int type) {
    input::MenuStartInput start;
    auto context=MenuContext::WatchedMatch;
    int index=0; bool connected=true,enabled=true;
    const auto poll=[&](bool held) { return start.Update(context,type,index,connected,
        held?input::directinput::Start:0,held?(type==input::PadXInput?input::xinput::Start:0x80000):0,enabled); };
    CHECK(!poll(true)); CHECK(!poll(false)); CHECK(poll(true));
    for(int i=0;i<10;++i)CHECK(!poll(true));
    CHECK(!poll(false)); CHECK(poll(true));
    ++index; CHECK(!poll(true)); CHECK(!poll(false)); CHECK(poll(true));
    connected=false; CHECK(!poll(false)); connected=true; CHECK(!poll(true));
    CHECK(!poll(false)); CHECK(poll(true));
    enabled=false; CHECK(!poll(false)); enabled=true; CHECK(!poll(true));
    CHECK(!poll(false)); CHECK(poll(true));
    context=MenuContext::MainMenu; CHECK(!poll(true)); CHECK(!poll(false)); CHECK(poll(true));
    context=MenuContext::PracticeMatch; CHECK(!poll(true)); CHECK(!poll(false)); CHECK(poll(true));
    for(const auto unavailable:{MenuContext::Unavailable,MenuContext::MatchLoading,MenuContext::OfflineTraining,MenuContext::ReplayExport}) {
        context=unavailable; CHECK(!poll(false)); CHECK(!poll(false)); CHECK(!poll(true));
    }
    CHECK(input::StartHeld(input::PadXInput,0,input::xinput::Start));
    CHECK(!input::StartHeld(input::PadXInput,input::directinput::Start,0));
    CHECK(input::StartHeld(input::PadDirectInput,input::directinput::Start,0x80000));
    CHECK(!input::StartHeld(input::PadDirectInput,0,input::xinput::Start));
}
static void DrawRequests(bool focused) {
    ui::OverlayPresentation menu;
    menu.Update(MenuContext::WatchedMatch,MatchState::Playing,false,focused); menu.Close();
    menu.Apply(MenuRequest::Toggle,MenuContext::WatchedMatch); CHECK(menu.Visible());
    menu.Apply(MenuRequest::Toggle,MenuContext::WatchedMatch); CHECK(!menu.Visible());
    ui::MenuPads pads; pads.Place(MenuContext::WatchedMatch,true);
    // The published copy is stale after F10. Start resolves the current state.
    pads.Publish(false,pads.Take()); menu.Open("F10"); pads.RequestStart();
    menu.Apply(pads.Take().kind,MenuContext::WatchedMatch); CHECK(!menu.Visible());
    pads.Publish(true,pads.Take()); menu.Close("F10"); pads.RequestStart();
    menu.Apply(pads.Take().kind,MenuContext::WatchedMatch); CHECK(menu.Visible());
    pads.RequestStart(); pads.RequestStart();
    menu.Apply(pads.Take().kind,MenuContext::WatchedMatch); CHECK(!menu.Visible());
    CHECK(pads.Take().kind==MenuRequest::None);
    pads.RequestStart(); pads.Request(MenuRequest::Play);
    menu.Apply(pads.Take().kind,MenuContext::WatchedMatch); CHECK(menu.Visible());
    pads.RequestStart(); pads.Reset(); CHECK(pads.Take().kind==MenuRequest::None);
    for(const auto context:{MenuContext::MainMenu,MenuContext::PracticeMatch}) {
        menu.Update(context,context==MenuContext::MainMenu?MatchState::PostMatch:MatchState::Playing,false,focused);
        menu.Apply(MenuRequest::Toggle,context); CHECK(menu.Visible());
        menu.Apply(MenuRequest::Toggle,context); CHECK(menu.Visible());
    }
    menu.Update(MenuContext::MatchLoading,MatchState::Preparing,false,focused);
    menu.Apply(MenuRequest::Toggle,MenuContext::MatchLoading); CHECK(!menu.Visible());
    menu.Update(MenuContext::WatchedMatch,MatchState::Playing,false,focused); CHECK(!menu.Visible());
}
int main() {
    StartEdges(input::PadXInput); StartEdges(input::PadDirectInput);
    DrawRequests(false); DrawRequests(true);
    std::cout << "Start ownership, context and drawing-time requests passed\n";
}
