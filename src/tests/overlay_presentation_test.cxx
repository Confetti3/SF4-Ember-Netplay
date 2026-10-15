#include "../ui/OverlayPresentation.hxx"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "test_support.hxx"
// A reader of the hold, on the game thread, while the drawing thread answers
// opens: from the moment an open is asked until the menu is hidden again, the
// pads are held at every read, through the handoff from "asked" to "shown".
static void CheckHandoff() {
    using sf4e::ui::MenuPads;
    MenuPads pads; pads.Place(sf4e::input::MenuContext::MainMenu, false);
    std::atomic<bool> running{true}, hide{false};
    std::thread drawer([&] {
        bool shown = false;
        while (running.load()) {
            const auto taken = pads.Take();
            if (taken.kind != MenuPads::Kind::None) shown = true;
            else if (hide.load()) shown = false;
            pads.Publish(shown, taken);
        }
    });
    bool held = true;
    for (int round = 0; round < 20000 && held; ++round) {
        hide = false;
        pads.Request(MenuPads::Kind::Controls);
        for (int read = 0; read < 50 && held; ++read) held = pads.Holds();
        hide = true;
        while (pads.Holds()) std::this_thread::yield();
    }
    running = false; drawer.join();
    CHECK(held);
}
int main() {
    using sf4e::netplay::MatchState;
    using sf4e::input::MenuContext;
    using sf4e::ui::OverlayPresentation;
    for (unsigned flags = 0; flags < 16; ++flags) {
        CHECK(sf4e::NativePauseMenuOpen(flags, -1) == false);
        CHECK(sf4e::NativePauseMenuOpen(flags, 0) == ((flags & 1) != 0));
        CHECK(sf4e::NativePauseMenuOpen(flags, 1) == ((flags & 1) != 0));
    }
    // Training load-state hold, replay Back freeze, and native online wait
    // must leave the passive overlays visible without a pause-menu player.
    CHECK(!sf4e::NativePauseMenuOpen(1, -1));
    CHECK(!sf4e::NativePauseMenuOpen(8, -1));
    CHECK(!sf4e::NativePauseMenuOpen(4, -1));
    for (int shell = 0; shell < 2; ++shell)
        for (int controls = 0; controls < 2; ++controls)
            for (int paused = 0; paused < 2; ++paused)
                CHECK(sf4e::ui::PassiveOverlayShown(shell != 0, controls != 0, paused != 0) == (!shell && !controls && !paused));
    sf4e::SetNativePauseForTest(true); CHECK(sf4e::battlePause.Paused());
    sf4e::battlePause.CloseBattle(); CHECK(!sf4e::battlePause.Paused());
    sf4e::SetNativePauseForTest(true);
    sf4e::battlePause.StartBattle(); CHECK(!sf4e::battlePause.Paused());
    OverlayPresentation menu;
    menu.Update(MenuContext::Unavailable, MatchState::None, false, true);
    menu.Open(); CHECK(!menu.Visible());
    menu.Update(MenuContext::MainMenu, MatchState::None, false, true); CHECK(menu.Visible());
    menu.Toggle(); CHECK(!menu.Visible()); menu.Open(); CHECK(menu.Visible());
    // Alt-tab keeps the shell on screen, and an unfocused window cannot toggle it.
    menu.Update(MenuContext::MainMenu, MatchState::None, false, false); CHECK(menu.Visible());
    menu.Toggle(); CHECK(menu.Visible());
    // The pad needs no focus: Start opens the shell and the shell closes itself.
    menu.Close(); CHECK(!menu.Visible());
    menu.Update(MenuContext::MainMenu, MatchState::None, false, false); menu.Open(); CHECK(menu.Visible());
    menu.Close(); CHECK(!menu.Visible()); menu.Open(); CHECK(menu.Visible());
    menu.Update(MenuContext::MainMenu, MatchState::None, false, true); CHECK(menu.Visible());
    menu.Update(MenuContext::MainMenu, MatchState::Preparing, false, true);
    menu.Open(); menu.Toggle(); CHECK(!menu.Visible() && !menu.Available());
    menu.Update(MenuContext::Unavailable, MatchState::Playing, false, true); CHECK(!menu.Visible());
    menu.Update(MenuContext::Unavailable, MatchState::PostMatch, false, true); CHECK(!menu.Visible());
    menu.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true); CHECK(menu.Visible() && menu.Reopened());
    // A transient foreground drop after the fight must not hide the shell or
    // reopen it a second time; a sustained absence still hides it.
    for (unsigned i = 0; i + 1 < OverlayPresentation::AwayFrames; ++i) {
        menu.Update(MenuContext::Unavailable, MatchState::PostMatch, false, true); CHECK(menu.Visible() && !menu.Reopened());
    }
    menu.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true); CHECK(menu.Visible() && !menu.Reopened());
    for (unsigned i = 0; i < OverlayPresentation::AwayFrames; ++i) menu.Update(MenuContext::Unavailable, MatchState::PostMatch, false, true);
    CHECK(!menu.Visible() && !menu.Available());
    menu.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true); CHECK(menu.Visible() && !menu.Reopened());
    menu.Close(); menu.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true); CHECK(!menu.Visible() && !menu.Reopened());
    menu.Update(MenuContext::MainMenu, MatchState::None, true, true); CHECK(!menu.Visible());
    menu.Open(); CHECK(menu.Visible());
    for (unsigned i = 0; i < OverlayPresentation::AwayFrames; ++i) menu.Update(MenuContext::Unavailable, MatchState::None, true, true);
    CHECK(!menu.Visible());
    menu.Update(MenuContext::MainMenu, MatchState::None, true, true); CHECK(menu.Visible());
    // A grant withdrawn before the fight (a late spectator link) goes straight
    // from Preparing to PostMatch, and the shell must come back all the same.
    OverlayPresentation late;
    late.Update(MenuContext::MainMenu, MatchState::Preparing, false, true); CHECK(!late.Visible() && !late.Available());
    late.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true); CHECK(late.Visible() && late.Reopened());
    // A Practice match this PC fights is a place for the menu: once its fight
    // runs, Start or F10 opens it there. Any other fight, and the practice
    // fight's own end, still hide it at once; the game's end brings the board
    // back as after any game.
    {
        OverlayPresentation practice;
        practice.Update(MenuContext::MainMenu, MatchState::None, false, true);
        practice.Update(MenuContext::MainMenu, MatchState::Preparing, false, true);
        CHECK(!practice.Available() && !practice.Visible());
        practice.Update(MenuContext::Unavailable, MatchState::Playing, false, true);
        practice.Open(); CHECK(!practice.Available() && !practice.Visible());
        practice.Update(MenuContext::PracticeMatch, MatchState::Playing, false, true);
        CHECK(practice.Available() && !practice.Visible());
        practice.Open(); CHECK(practice.Visible());
        practice.Toggle(); CHECK(!practice.Visible());
        practice.Toggle(); CHECK(practice.Visible());
        practice.Update(MenuContext::PracticeMatch, MatchState::Playing, false, false); CHECK(practice.Visible());
        practice.Update(MenuContext::Unavailable, MatchState::Playing, false, true);
        CHECK(!practice.Visible() && !practice.Available());
        practice.Open(); CHECK(!practice.Visible());
        practice.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true);
        CHECK(practice.Visible() && practice.Reopened());
    }
    // Game-thread open requests wait for the drawing thread and coalesce, Play first.
    {
        using Kind = sf4e::ui::MenuPads::Kind;
        sf4e::ui::MenuPads requests;
        CHECK(requests.Take().kind == Kind::None);
        requests.Request(Kind::Controls); requests.Request(Kind::Play);
        CHECK(requests.Take().kind == Kind::Play && requests.Take().kind == Kind::None);
        requests.Request(Kind::Play); requests.Request(Kind::Controls);
        CHECK(requests.Take().kind == Kind::Play);
        requests.Request(Kind::Controls);
        CHECK(requests.Take().kind == Kind::Controls);
    }
    {
        // The menu's hold on the pads, as the overlay and the pad update use it.
        using sf4e::input::MenuContext;
        using sf4e::ui::MenuPads;
        OverlayPresentation shell; shell.Update(MenuContext::MainMenu, MatchState::None, false, false); shell.Close();
        // Start arrives after a frame took the opens and before that frame
        // publishes the menu still hidden: the request holds the pads until
        // the next frame has shown the menu.
        MenuPads pads; pads.Place(MenuContext::MainMenu, false);
        CHECK(!pads.Holds() && pads.Takes() && pads.StartOpens());
        auto taken = pads.Take();
        pads.Request(MenuPads::Kind::Controls);          // the game thread, mid-frame
        CHECK(pads.Holds());
        pads.Publish(shell.Visible(), taken);           // that frame: still hidden
        CHECK(!shell.Visible() && pads.Holds());
        taken = pads.Take();
        CHECK(taken.kind == MenuPads::Kind::Controls);
        shell.Open();
        CHECK(pads.Holds());                            // taken, not yet published
        pads.Publish(shell.Visible(), taken);
        CHECK(shell.Visible() && pads.Holds());
        // A request the menu cannot answer lets the pads go once answered.
        MenuPads refused; refused.Place(MenuContext::MainMenu, false);
        OverlayPresentation fight; fight.Update(MenuContext::MainMenu, MatchState::Preparing, false, true);
        refused.Request(MenuPads::Kind::Controls);
        CHECK(refused.Holds());
        const auto none = refused.Take(); if (none.kind != MenuPads::Kind::None && fight.Available()) fight.Open();
        refused.Publish(fight.Visible(), none);
        CHECK(!refused.Holds());
        // Drawing stops with the menu shown, so its last word stays "shown":
        // a match being prepared takes the pads from it at once, and they stay
        // the game's after the match until the main menu is back. Start opens
        // nothing while the match is prepared, so the pads are not even taken.
        CHECK(pads.Holds());
        pads.Place(MenuContext::MainMenu, true); CHECK(!pads.Holds() && !pads.Takes() && !pads.StartOpens());
        pads.Place(MenuContext::Unavailable, true); CHECK(!pads.Holds());
        pads.Place(MenuContext::Unavailable, false); CHECK(!pads.Holds() && !pads.Takes());
        pads.Place(MenuContext::MainMenu, false); CHECK(pads.Holds());
        // A brief drop of the native menu is ridden out, as the menu's own
        // availability rides it; Training past that takes the pads back.
        for (unsigned i = 0; i + 1 < OverlayPresentation::AwayFrames; ++i) { pads.Place(MenuContext::Unavailable, false); CHECK(pads.Holds()); }
        pads.Place(MenuContext::MainMenu, false);
        for (unsigned i = 0; i + 1 < OverlayPresentation::AwayFrames; ++i) pads.Place(MenuContext::OfflineTraining, false);
        CHECK(pads.Holds());
        pads.Place(MenuContext::OfflineTraining, false); CHECK(!pads.Holds() && !pads.Takes());
        // Over an export the menu holds them again, once no match is being
        // prepared; Start is the game's pause there, so only a shown menu takes them.
        pads.Place(MenuContext::ReplayExport, true); CHECK(!pads.Holds());
        pads.Place(MenuContext::ReplayExport, false); CHECK(pads.Holds() && !pads.StartOpens());
        // Over a Practice match this PC fights, Start opens the menu and its
        // request holds the pads at once, so neither the press nor anything
        // after it reaches the fight while the menu is up; the fighter gives
        // the game neutral input. The fight's end gives the pads back at once.
        MenuPads practice; practice.Place(MenuContext::PracticeMatch, true);
        CHECK(!practice.Holds() && practice.Takes() && practice.StartOpens());
        practice.Request(MenuPads::Kind::Controls); CHECK(practice.Holds());
        practice.Publish(true, practice.Take()); CHECK(practice.Holds());
        practice.Place(MenuContext::PracticeMatch, true); CHECK(practice.Holds());
        practice.Place(MenuContext::Unavailable, true); CHECK(!practice.Holds() && !practice.Takes() && !practice.StartOpens());
        practice.Place(MenuContext::PracticeMatch, true); practice.Publish(false, practice.Take()); CHECK(!practice.Holds());
        // A display reset drops the opens waiting and what was shown, until a frame says again.
        MenuPads reset; reset.Place(MenuContext::MainMenu, false);
        reset.Request(MenuPads::Kind::Play); reset.Publish(true, MenuPads::Taken{});
        CHECK(reset.Holds());
        reset.Reset();
        CHECK(!reset.Holds() && reset.Take().kind == MenuPads::Kind::None && reset.Takes());
    }
    CheckHandoff();
    std::cout << "Safe-menu, focus, offline, loading, fight and rematch visibility passed\n";
}
