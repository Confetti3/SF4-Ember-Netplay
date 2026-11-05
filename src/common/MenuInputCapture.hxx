#pragma once
#include <cstdint>
#include <cstring>
namespace sf4e { namespace input {
// Where the game is for Ember's menu.
// ReplayExport: a replay is being exported in the game's battle log. Ember's
// menu can be opened there with F10 to follow or cancel it, and the pad then
// drives it; Start stays the game's, whose pause it opens, so it does not
// open Ember's menu there.
// PracticeMatch: a Practice room's game (room::Snapshot::practice) that
// this PC fights in. Such a game ends only when its round time runs out, so
// Start or F10 opens Ember's menu over the fight, where a fighter ends it
// (End practice). The game has no pause under rollback for Start to open.
// WatchedMatch: a room game this PC only watches, under rollback. Start or
// F10 opens Ember's menu over it, where the spectator leaves the game (Stop
// watching); the game keeps running underneath, as it does for the fighters.
// MatchResult: the WIN (or DRAW) screen of a room game this PC fought, at a table with
// Instant rematch on (room::Rules::instantRematch). Ember's post-match prompt
// opens over it by itself; Start does not open the menu there.
enum class MenuContext { Unavailable, MainMenu, OfflineTraining, ReplayExport, PracticeMatch, WatchedMatch, MatchResult };
// A room game Ember's menu is shown over: the one place a fight does not keep it away.
inline bool MenuOverFight(MenuContext context) {
    return context == MenuContext::PracticeMatch || context == MenuContext::WatchedMatch || context == MenuContext::MatchResult;
}
// The pad drives Ember's menus at the main menu, the training controls in
// offline Training (opened there by Back and Start, TrainingPad.hxx), and the
// menu opened over an export or a fight. Start opens Ember at the main menu
// and over a fight.
inline bool ControllerMenuAvailable(MenuContext context) { return context != MenuContext::Unavailable; }
inline bool ControllerOpensMenu(MenuContext context) {
    return context == MenuContext::MainMenu || context == MenuContext::PracticeMatch || context == MenuContext::WatchedMatch;
}
// Where Start closes Ember's menu again, back to the full game: over a fight,
// as F10 does. At the main menu Ember is the menu, so Start keeps it.
inline bool ControllerClosesMenu(MenuContext context) {
    return context == MenuContext::PracticeMatch || context == MenuContext::WatchedMatch;
}
// The drawing thread's answer to Start's request (StartPress), the one place
// that decides it: a hidden menu opens; a shown one closes over a fight
// (ControllerClosesMenu) unless a text field is being edited, where Start
// belongs to the editor (the on-screen keyboard's Done), and otherwise stays.
enum class StartAnswer { None, Open, Close };
inline StartAnswer AnswerStart(bool visible, MenuContext context, bool textEntry) {
    if (!visible) return StartAnswer::Open;
    return ControllerClosesMenu(context) && !textEntry ? StartAnswer::Close : StartAnswer::None;
}
// Where Ember's menu can be shown: offline Training has its own controls.
inline bool MenuShownIn(MenuContext context) {
    return context == MenuContext::MainMenu || context == MenuContext::ReplayExport || MenuOverFight(context);
}
// A room game being prepared or played keeps Ember's menu away, except one
// where the menu is shown over the fight itself.
inline bool FightKeepsMenuAway(bool fighting, MenuContext context) {
    return fighting && !MenuOverFight(context);
}
// Native Pad::System publishes five adjacent uint32 input caches per player.
// Keep assignment metadata and provider input untouched for overlay polling.
inline unsigned NativeMenuHeld(const void* system) {
    const auto* bytes=static_cast<const unsigned char*>(system);unsigned held=0;
    for(unsigned side=0;side<2;++side){unsigned value;std::memcpy(&value,bytes+0x18+side*0x50,4);held|=value;}
    return held;
}
constexpr unsigned NativeStart = 0x200;
// Start on the pad the game polls (sf4e__Pad), where it may open Ember now
// (connected, owning the menu, nothing being captured, ControllerOpensMenu):
// the one detector of Start's press. A press counts once, after every button
// was up, whatever the menu shows, so a Start still held after it closed the
// menu never counts again. Each counted press asks for the menu, and the
// drawing thread decides what it does (AnswerStart).
struct StartPress {
    bool armed = false;
    bool Update(bool allowed, unsigned physical) {
        if (!allowed) { armed = false; return false; }
        if (!physical) { armed = true; return false; }
        if (!armed || !(physical & NativeStart)) return false;
        armed = false;
        return true;
    }
};
inline void ClearNativeMenuInputs(void* system, unsigned allowed = 0) {
    auto* bytes=static_cast<unsigned char*>(system);
    for(unsigned side=0;side<2;++side)for(unsigned cache=0;cache<5;++cache) {
        auto* address=bytes+0x18+side*0x50+cache*sizeof(std::uint32_t);
        std::uint32_t value;std::memcpy(&value,address,sizeof(value));value&=allowed;
        std::memcpy(address,&value,sizeof(value));
    }
}
// The game's buttons Ember's menu keeps from it this update (ClearNativeMenuInputs
// takes the complement): every one while the menu captures the pads. Once the
// capture ends, the buttons held on the first update after it are the menu's
// (the press that closed it among them), and each stays kept until it is let
// go. A button pressed after that reaches the game at once, even while a kept
// one, such as a direction held from the menu into the fight, is still down.
class MenuInputCapture {
public:
    static constexpr unsigned All=~0u;
    unsigned Update(bool capture,unsigned held) {
        if(capture){capturing_=true;return All;}
        if(capturing_){capturing_=false;kept_=held;}
        else kept_&=held;
        return kept_;
    }
private:
    bool capturing_=false;
    unsigned kept_=0;
};
} }
