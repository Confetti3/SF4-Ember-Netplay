#pragma once
#include <cstdint>
#include <cstring>
namespace sf4e { namespace input {
// ReplayExport: a replay is being exported in the game's battle log. Ember's
// menu can be opened there with F10 to follow or cancel it, and the pad then
// drives it; Start stays the game's, whose pause it opens, so it does not
// open Ember's menu there.
enum class MenuContext { Unavailable, MainMenu, OfflineTraining, ReplayExport };
// The pad drives Ember's menus at the main menu, the training controls in
// offline Training (opened there by Back and Start, TrainingPad.hxx), and the
// menu F10 opened over an export. Start opens Ember at the main menu only.
inline bool ControllerMenuAvailable(MenuContext context) { return context != MenuContext::Unavailable; }
inline bool ControllerOpensMenu(MenuContext context) { return context == MenuContext::MainMenu; }
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
