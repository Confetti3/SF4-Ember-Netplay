#pragma once
#include <cstdint>
#include <cstring>
#include "PadKind.hxx"
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
// F10 toggles Ember's menu over it, where the spectator leaves the game (Stop
// watching); the game keeps running underneath, as it does for the fighters.
// MatchResult: the WIN (or DRAW) screen of a room game this PC fought, at a table with
// Instant rematch on (room::Rules::instantRematch). Ember's post-match prompt
// opens over it by itself; Start does not open the menu there.
enum class MenuContext { Unavailable, MatchLoading, MainMenu, OfflineTraining, ReplayExport, PracticeMatch, WatchedMatch, MatchResult };
inline bool Loading(MenuContext context) { return context == MenuContext::MatchLoading; }
// A room game Ember's menu is shown over: the one place a fight does not keep it away.
inline bool MenuOverFight(MenuContext context) {
    return context == MenuContext::PracticeMatch || context == MenuContext::WatchedMatch || context == MenuContext::MatchResult;
}
// The pad drives Ember's menus at the main menu, the training controls in
// offline Training (opened there by Back and Start, TrainingPad.hxx), and the
// menu opened over an export or a fight. Start opens Ember at the main menu
// and over a fight.
inline bool ControllerMenuAvailable(MenuContext context) { return context != MenuContext::Unavailable && context != MenuContext::MatchLoading; }
inline bool ControllerOpensMenu(MenuContext context) {
    return context == MenuContext::MainMenu || context == MenuContext::PracticeMatch || context == MenuContext::WatchedMatch;
}
// Practice keeps Start open-only: a fighter opens the menu to end the game,
// and a second Start there is a mispress.
inline bool StartTogglesMenu(MenuContext context) { return context == MenuContext::WatchedMatch; }
// Where Ember's menu can be shown: offline Training has its own controls.
inline bool MenuShownIn(MenuContext context) {
    return context == MenuContext::MainMenu || context == MenuContext::ReplayExport || MenuOverFight(context);
}
// A room game being prepared or played keeps Ember's menu away, except one
// where the menu is shown over the fight itself.
inline bool FightKeepsMenuAway(bool fighting, MenuContext context) {
    return context == MenuContext::MatchLoading || (fighting && !MenuOverFight(context));
}
// Native Pad::System publishes five adjacent uint32 input caches per player.
// Keep assignment metadata and provider input untouched for overlay polling.
inline unsigned NativeMenuHeld(const void* system) {
    const auto* bytes=static_cast<const unsigned char*>(system);unsigned held=0;
    for(unsigned side=0;side<2;++side){unsigned value;std::memcpy(&value,bytes+0x18+side*0x50,4);held|=value;}
    return held;
}
constexpr unsigned NativeStart = 0x200;
inline void ClearNativeMenuInputs(void* system, unsigned allowed = 0) {
    auto* bytes=static_cast<unsigned char*>(system);
    for(unsigned side=0;side<2;++side)for(unsigned cache=0;cache<5;++cache) {
        auto* address=bytes+0x18+side*0x50+cache*sizeof(std::uint32_t);
        std::uint32_t value;std::memcpy(&value,address,sizeof(value));value&=allowed;
        std::memcpy(address,&value,sizeof(value));
    }
}
// Start is sampled on the game thread, before native input is cleared. A
// press is consumed even while the menu is shown, so closing it cannot make
// the same held press open it again. A changed owner or context needs neutral.
class MenuStartInput {
public:
    bool Update(MenuContext context, int type, int index, bool connected,
        unsigned mapped, unsigned physical, bool enabled) {
        const bool changed = context != context_ || type != type_ || index != index_;
        context_ = context; type_ = type; index_ = index;
        if (changed || !connected || !enabled || !ControllerOpensMenu(context)) armed_ = false;
        else if (!mapped && !physical) armed_ = true;
        else if (armed_ && StartHeld(type,mapped,physical)) {
            armed_ = false;
            return true;
        }
        return false;
    }
private:
    MenuContext context_ = MenuContext::Unavailable;
    int type_ = -1, index_ = -1;
    bool armed_ = false;
};
class MenuInputCapture {
public:
    bool Update(bool capture,unsigned held) {
        if(capture)draining_=true;
        else if(!held)draining_=false;
        return capture||draining_;
    }
private:
    bool draining_=false;
};
} }
