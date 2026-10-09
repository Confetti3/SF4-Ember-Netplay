#include "../ui/OverlayPresentation.hxx"
#include "../netplay/MatchLoading.hxx"
#include "../common/MenuInputCapture.hxx"
#include <array>
#include <cstdlib>
#include <iostream>
#include "test_support.hxx"
int main() {
    using sf4e::netplay::MatchState;
    using sf4e::ui::OverlayPresentation;
    // Match entry covers the native pipeline, including rematches/spectators,
    // but never traps a player who needs native Training/Options to return.
    using sf4e::netplay::MatchLoading;
    CHECK(!MatchLoading(MatchState::None, true, false));
    CHECK(!MatchLoading(MatchState::Preparing, false, false));
    CHECK(MatchLoading(MatchState::Preparing, true, false));
    CHECK(MatchLoading(MatchState::Preparing, false, true));
    CHECK(!MatchLoading(MatchState::Playing, false, true));
    CHECK(!MatchLoading(MatchState::PostMatch, true, true));
    CHECK(!MatchLoading(MatchState::None, true, true));
    // Input suppression does not depend on a render frame or window focus.
    // Both players' native caches clear; unrelated provider bytes stay intact.
    std::array<unsigned char, 0x100> native;
    native.fill(0xa5);
    sf4e::input::MenuInputCapture loadingGate;
    CHECK(loadingGate.Update(MatchLoading(MatchState::Preparing, false, true), sf4e::input::NativeMenuHeld(native.data())));
    sf4e::input::ClearNativeMenuInputs(native.data());
    for (unsigned i = 0; i < native.size(); ++i) {
        const bool cache = (i >= 0x18 && i < 0x2c) || (i >= 0x68 && i < 0x7c);
        CHECK(native[i] == (cache ? 0 : 0xa5));
    }
    // A held Confirm/Start cannot leak through on cancellation or completion.
    CHECK(loadingGate.Update(MatchLoading(MatchState::PostMatch, true, false), 0x1000));
    CHECK(!loadingGate.Update(false, 0));
    CHECK(!loadingGate.Update(false, 0x1000));
    CHECK(loadingGate.Update(MatchLoading(MatchState::Preparing, true, false), 0));
    CHECK(!loadingGate.Update(MatchLoading(MatchState::Playing, false, true), 0));
    OverlayPresentation menu;
    menu.Update(false, MatchState::None, false, true);
    menu.Open(); CHECK(!menu.Visible());
    menu.Update(true, MatchState::None, false, true); CHECK(menu.Visible());
    menu.Toggle(); CHECK(!menu.Visible()); menu.Open(); CHECK(menu.Visible());
    // Alt-tab keeps the shell on screen, and an unfocused window cannot toggle it.
    menu.Update(true, MatchState::None, false, false); CHECK(menu.Visible());
    menu.Toggle(); CHECK(menu.Visible());
    menu.Update(true, MatchState::None, false, true); CHECK(menu.Visible());
    menu.Update(true, MatchState::Preparing, false, true);
    menu.Open(); menu.Toggle(); CHECK(!menu.Visible() && !menu.Available());
    menu.Update(false, MatchState::Playing, false, true); CHECK(!menu.Visible());
    menu.Update(false, MatchState::PostMatch, false, true); CHECK(!menu.Visible());
    menu.Update(true, MatchState::PostMatch, false, true); CHECK(menu.Visible() && menu.Reopened());
    // A transient foreground drop after the fight must not hide the shell or
    // reopen it a second time; a sustained absence still hides it.
    for (unsigned i = 0; i + 1 < OverlayPresentation::AwayFrames; ++i) {
        menu.Update(false, MatchState::PostMatch, false, true); CHECK(menu.Visible() && !menu.Reopened());
    }
    menu.Update(true, MatchState::PostMatch, false, true); CHECK(menu.Visible() && !menu.Reopened());
    for (unsigned i = 0; i < OverlayPresentation::AwayFrames; ++i) menu.Update(false, MatchState::PostMatch, false, true);
    CHECK(!menu.Visible() && !menu.Available());
    menu.Update(true, MatchState::PostMatch, false, true); CHECK(menu.Visible() && !menu.Reopened());
    menu.Close(); menu.Update(true, MatchState::PostMatch, false, true); CHECK(!menu.Visible() && !menu.Reopened());
    menu.Update(true, MatchState::None, true, true); CHECK(!menu.Visible());
    menu.Open(); CHECK(menu.Visible());
    for (unsigned i = 0; i < OverlayPresentation::AwayFrames; ++i) menu.Update(false, MatchState::None, true, true);
    CHECK(!menu.Visible());
    menu.Update(true, MatchState::None, true, true); CHECK(menu.Visible());
    // A grant withdrawn before the fight (a late spectator link) goes straight
    // from Preparing to PostMatch, and the shell must come back all the same.
    OverlayPresentation late;
    late.Update(true, MatchState::Preparing, false, true); CHECK(!late.Visible() && !late.Available());
    late.Update(true, MatchState::PostMatch, false, true); CHECK(late.Visible() && late.Reopened());
    // Game-thread open requests wait for the drawing thread and coalesce, Play first.
    using Kind = sf4e::ui::OpenRequests::Kind;
    sf4e::ui::OpenRequests requests;
    CHECK(requests.Take() == Kind::None);
    requests.Post(Kind::Controls); requests.Post(Kind::Play);
    CHECK(requests.Take() == Kind::Play && requests.Take() == Kind::None);
    requests.Post(Kind::Play); requests.Post(Kind::Controls);
    CHECK(requests.Take() == Kind::Play);
    requests.Post(Kind::Controls);
    CHECK(requests.Take() == Kind::Controls);
    std::cout << "Safe-menu, focus, offline, loading, fight and rematch visibility passed\n";
}
