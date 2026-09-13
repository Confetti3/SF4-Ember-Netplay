#include "../ui/OverlayPresentation.hxx"
#include <cstdlib>
#include <iostream>
#define CHECK(c) do { if (!(c)) { std::cerr << "Failure at " << __LINE__ << '\n'; return 1; } } while (false)
int main() {
    using sf4e::netplay::MatchState;
    sf4e::ui::OverlayPresentation menu;
    menu.Update(false, MatchState::None, false, true);
    menu.Open(); CHECK(!menu.Visible());
    menu.Update(true, MatchState::None, false, true); CHECK(menu.Visible());
    menu.Toggle(); CHECK(!menu.Visible()); menu.Open(); CHECK(menu.Visible());
    menu.Update(true, MatchState::None, false, false); CHECK(!menu.Visible());
    menu.Toggle(); menu.Update(true, MatchState::None, false, true); CHECK(menu.Visible());
    menu.Update(true, MatchState::Preparing, false, true);
    menu.Open(); menu.Toggle(); CHECK(!menu.Visible() && !menu.Available());
    menu.Update(false, MatchState::Playing, false, true); CHECK(!menu.Visible());
    menu.Update(false, MatchState::PostMatch, false, true); CHECK(!menu.Visible());
    menu.Update(true, MatchState::PostMatch, false, true); CHECK(menu.Visible() && menu.Reopened());
    menu.Close(); menu.Update(true, MatchState::PostMatch, false, true); CHECK(!menu.Visible() && !menu.Reopened());
    menu.Update(true, MatchState::None, true, true); CHECK(!menu.Visible());
    menu.Open(); CHECK(menu.Visible());
    menu.Update(false, MatchState::None, true, true); CHECK(!menu.Visible());
    menu.Update(true, MatchState::None, true, true); CHECK(menu.Visible());
    std::cout << "Safe-menu, focus, offline, loading, fight and rematch visibility passed\n";
}
