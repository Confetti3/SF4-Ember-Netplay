#include "../ui/OverlayPresentation.hxx"
#include <cstdlib>
#include <iostream>
#include "test_support.hxx"
int main() {
    using sf4e::netplay::MatchState;
    using sf4e::ui::OverlayPresentation;
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
