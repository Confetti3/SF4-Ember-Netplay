#pragma once
// Ember's shell over the runtime: each frame the published runtime snapshot
// becomes the shell's view, and what the player does in it becomes runtime
// commands. The host that draws the overlay (SF4's sf4e__Overlay.cxx) owns
// the graphics device and the frame; this owns the shell and everything
// between it and the runtime that is not the game's.
//
// The player's pick and the selection screen are still the game's (ShellGame):
// they are SF4's native pick type until the catalog reaches the shell
// (EMBER_LIBRARY.md, step 5).

#include "../ui/ApplicationShell.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "sf4e__NetplayFacade.hxx"

namespace ember {

// What only the game knows of the shell's frame.
class ShellGame {
public:
    using RuntimeSnapshot = sf4e::NetplayFacade::RuntimeSnapshot;
    // The game's part of the view, filled after the runtime's: the player's
    // pick and what the shell says of it, whether that pick can be readied,
    // the watched game's picture, the display advice and the build.
    // view.canReady arrives holding the runtime's answer: the game ANDs its
    // own into it (canReady = canReady && ...) and never sets it.
    virtual void FillView(const RuntimeSnapshot& snapshot, sf4e::ui::ShellView& view) = 0;
    // The table page's Ultra and Appearance rows step the pick; nothing is
    // sent. False when the pick cannot change now.
    virtual bool StepSelection(const sf4e::ui::ShellAction::SelectionStep& step, const RuntimeSnapshot& snapshot) = 0;
    // Training select's Start, at the idle main menu: the game enters
    // Training with the player's pick and the dummy.
    virtual void StartTraining(const RuntimeSnapshot& snapshot) = 0;
    // The pick a room command carries: fighter, stage and random pool.
    virtual void AddPick(sf4e::NetplayFacade::RuntimeCommand& command) = 0;
    // The selection screen the shell shows inside its own page: the dummy's
    // on Training select, else the player's.
    virtual void DrawSelection(bool dummy, const RuntimeSnapshot& snapshot) = 0;
    // Training select's Hitboxes row (Capabilities::hitboxes): shows the
    // fighters' boxes over the game's Training, or not, and keeps the choice;
    // the view's hitboxesShown is the game's to fill (FillView). False when
    // it was not kept. A game without the viewer is never asked.
    virtual bool ShowHitboxes(bool) { return false; }
protected:
    ~ShellGame() = default;
};

class ShellHost {
public:
    sf4e::ui::ApplicationShell& Shell() { return shell_; }

    // The shown shell, one frame. `developer` draws the developer page (empty
    // in a player's build). Returns why the menu closes after this frame, or
    // null while it stays open.
    const char* Draw(const ShellGame::RuntimeSnapshot& snapshot, const sf4e::NetplayStatus& status,
        const sf4e::ui::ControllerNavigation& navigation, ShellGame& game,
        const sf4e::ui::ApplicationShell::DrawSelection& developer);
    // Hidden, the shell still takes identity answers, ends a cancelled Discord
    // sign-in on its service, keeps the room chat and follows the room.
    void Background(const ShellGame::RuntimeSnapshot& snapshot);

private:
    sf4e::ui::ApplicationShell shell_;
    // The room the drawn shell shows, kept across frames (Draw).
    sf4e::ui::RoomCarry roomCarry_;
};

} // namespace ember
