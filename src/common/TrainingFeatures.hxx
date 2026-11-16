#pragma once
// What Ember's Training lab offers over a game's Training battle, by the
// parts only some games can fill. The game's adapter names the ones it has
// (game::Capabilities::trainingFeatures); the lab's controls (F6) leave out
// the pages and rows of the rest, and its HUD the hints and parts they would
// draw. Every flag is off until the game sets it, by name; All() is SF4's,
// and the controls' default, so a view drawn before a game says otherwise
// (the render tests) is SF4's.
//
// What every game's lab has needs no flag: dummy recording (eight slots,
// Record and Play, loop, order, save and load by name), the dummy's reply
// from those recordings, the frame meter's options and the HUD's size and
// parts, and the hitbox switch.

namespace ember {

struct TrainingFeatures {
    // The input history page (View::history, kept by the session from the
    // input the runtime commits) and the HUD's input displays beside the meter.
    bool inputHistory = false;
    // The classic meter's input lanes: the meter's samples carry each side's
    // pad (FighterSample::input).
    bool classicInputs = false;
    // The game's own dummy settings as rows (DummyState: action, guard,
    // counter hit, quick stand, gauges), which the runtime writes to the game.
    bool dummySettings = false;
    // Typed replies: reply slots written in notation, and the moves list each
    // fighter's command table fills (training/FighterMoves.hxx).
    bool typedReplies = false;
    // Position save and reset: their rows, the hotkeys that send them and
    // the pad's Back (tap resets, hold saves).
    bool position = false;
    // Combo check's pages (training/ComboCheck.hxx).
    bool comboCheck = false;
    // The phone remote's page (training/Remote*).
    bool remote = false;
    // The Data panel and its page (View::data).
    bool data = false;
    // The hitboxes' parts (hurt, throw and the skeleton) as rows of their
    // own; without it the Hitboxes page has its switch alone.
    bool hitboxParts = false;
    // The player's own Record, Play and controls buttons from the button
    // layout page (input::TrainingButtons), on the HUD's strip.
    bool padButtons = false;

    static constexpr TrainingFeatures All() {
        TrainingFeatures all;
        all.inputHistory = all.classicInputs = all.dummySettings = all.typedReplies = all.position = true;
        all.comboCheck = all.remote = all.data = all.hitboxParts = all.padButtons = true;
        return all;
    }
};

} // namespace ember
