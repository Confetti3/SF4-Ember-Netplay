#include "../ui/OverlayPresentation.hxx"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
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
    // A Practice match this PC fights, and a game it only watches, are places
    // for the menu: once the fight runs, Start or F10 opens it there. Any other
    // fight, and that fight's own end (a spectator's leave retires its stream),
    // still hide it at once; the game's end brings the board back as after any game.
    for (const auto context : {MenuContext::PracticeMatch, MenuContext::WatchedMatch}) {
        OverlayPresentation over;
        over.Update(MenuContext::MainMenu, MatchState::None, false, true);
        over.Update(MenuContext::MainMenu, MatchState::Preparing, false, true);
        CHECK(!over.Available() && !over.Visible());
        over.Update(MenuContext::Unavailable, MatchState::Playing, false, true);
        over.Open(); CHECK(!over.Available() && !over.Visible());
        over.Update(context, MatchState::Playing, false, true);
        CHECK(over.Available() && !over.Visible());
        over.Open(); CHECK(over.Visible());
        over.Toggle(); CHECK(!over.Visible());
        over.Toggle(); CHECK(over.Visible());
        over.Update(context, MatchState::Playing, false, false); CHECK(over.Visible());
        over.Update(MenuContext::Unavailable, MatchState::Playing, false, true);
        CHECK(!over.Visible() && !over.Available());
        over.Open(); CHECK(!over.Visible());
        over.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true);
        CHECK(over.Visible() && over.Reopened());
    }
    // The log says why the shell showed or hid.
    {
        OverlayPresentation why;
        why.Update(MenuContext::MainMenu, MatchState::None, false, true);
        CHECK(std::string(why.Why()) == "the main menu is in front");
        why.Toggle(); CHECK(!why.Visible() && std::string(why.Why()) == "F10");
        why.Open("Start"); CHECK(why.Visible() && std::string(why.Why()) == "Start");
        why.Close("Back on Ember's Home"); CHECK(!why.Visible() && std::string(why.Why()) == "Back on Ember's Home");
        why.Open(); CHECK(std::string(why.Why()) == "opened");
        why.Update(MenuContext::MainMenu, MatchState::Preparing, false, true);
        CHECK(!why.Visible() && std::string(why.Why()) == "a game is starting or running");
        why.Update(MenuContext::MainMenu, MatchState::PostMatch, false, true);
        CHECK(why.Visible() && std::string(why.Why()) == "a match ended");
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
        // Over a Practice match this PC fights, and over a game it only
        // watches, Start opens the menu and its request holds the pads at
        // once, so neither the press nor anything after it reaches the fight
        // while the menu is up; the fighter gives the game neutral input. The
        // fight's end gives the pads back at once.
        for (const auto context : {MenuContext::PracticeMatch, MenuContext::WatchedMatch}) {
            MenuPads over; over.Place(context, true);
            CHECK(!over.Holds() && over.Takes() && over.StartOpens());
            over.Request(MenuPads::Kind::Controls); CHECK(over.Holds());
            over.Publish(true, over.Take()); CHECK(over.Holds());
            over.Place(context, true); CHECK(over.Holds());
            over.Place(MenuContext::Unavailable, true); CHECK(!over.Holds() && !over.Takes() && !over.StartOpens());
            over.Place(context, true); over.Publish(false, over.Take()); CHECK(!over.Holds());
        }
        // A display reset drops the opens waiting and what was shown, until a frame says again.
        MenuPads reset; reset.Place(MenuContext::MainMenu, false);
        reset.Request(MenuPads::Kind::Play); reset.Publish(true, MenuPads::Taken{});
        CHECK(reset.Holds());
        reset.Reset();
        CHECK(!reset.Holds() && reset.Take().kind == MenuPads::Kind::None && reset.Takes());
    }
    {
        // A watched game's hint: for its first ten seconds, whatever the clock read when it began,
        // not again while it lasts, and again for the next game.
        sf4e::ui::TimedHint hint(10);
        CHECK(!hint.Due(false, 0));
        CHECK(hint.Due(true, 5) && hint.Due(true, 14.9) && !hint.Due(true, 15) && !hint.Due(true, 60));
        CHECK(!hint.Due(false, 61));
        CHECK(hint.Due(true, 62) && !hint.Due(true, 72));
        sf4e::ui::TimedHint first(10);
        CHECK(first.Due(true, 0) && first.Due(true, 9.9) && !first.Due(true, 10));
        // On a clock that keeps going (the process clock, not ImGui's, which a DX9
        // reset restarts), the same watched game stays expired however long it runs.
        sf4e::ui::TimedHint resumed(10);
        CHECK(resumed.Due(true, 1000) && !resumed.Due(true, 1011) && !resumed.Due(true, 1012) && !resumed.Due(true, 5000));
    }
    // A result offer is mandatory, including the explicit offline prototype:
    // no rendering delay, F10 or Start may release the native Continue input.
    {
        OverlayPresentation result;
        result.Update(MenuContext::Unavailable,MatchState::Playing,true,true);
        result.Update(MenuContext::QuickRematch,MatchState::Playing,true,true);
        CHECK(result.Available()&&result.Visible());
        result.Close();result.Toggle();CHECK(result.Visible());
        result.Update(MenuContext::QuickRematch,MatchState::Playing,true,false);CHECK(result.Visible());
        result.Update(MenuContext::Unavailable,MatchState::None,true,true);
        CHECK(!result.Visible()&&!result.Available()); // no half-second shell grace over the resumed offline fight
        sf4e::ui::MenuPads owner;
        owner.Place(MenuContext::QuickRematch,true);
        CHECK(owner.Holds()&&owner.Takes()&&!owner.StartOpens()); // before first rendered frame
        owner.Publish(false,owner.Take());CHECK(owner.Holds());
        owner.Reset();CHECK(owner.Holds()); // device recreation does not advance a waiting result
        owner.Place(MenuContext::Unavailable,true);CHECK(!owner.Holds()&&!owner.Takes());
        owner.Place(MenuContext::QuickRematch,false);CHECK(owner.Holds());
        owner.Place(MenuContext::Unavailable,false);CHECK(!owner.Holds()&&!owner.Takes());
    }
    CheckHandoff();
    std::cout << "Safe-menu, focus, offline, loading, fight and rematch visibility passed\n";
}
