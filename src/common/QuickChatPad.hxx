#pragma once
// The pad's and the keyboard's quick lines (QuickChat.hxx), in a fight this PC
// fights in:
// - With Back held, the pad's four directions belong to the chord: a direction
//   that goes down while Back is held sends its slot once, until it is let go.
//   Back and the directions are the chord's for as long as Back is held, so
//   the game never sees them (the fighter stands still while choosing).
// - F1 to F4 send slots 0 to 3, and the game never sees them either.
// A Back that is already down when the chord is allowed again is not a chord
// until it is pressed again, and a chord held when it is no longer allowed
// ends there.
#include <cstdint>
#include "QuickChat.hxx"
#include "TrainingPad.hxx"

namespace sf4e { namespace input {
// The pad's physical direction bits: bits 0 to 7 of the physical word are
// directions (MenuButtons.hxx), and the first four are the pad's, the same
// bits as the fight map's directions (ControllerButtons, ReplayInputs.hxx).
constexpr std::uint32_t PhysicalUp = 0x1, PhysicalDown = 0x2, PhysicalLeft = 0x4, PhysicalRight = 0x8;
// The slot of each direction: Up, Right, Down, Left.
constexpr std::uint32_t SlotDirection[quickchat::Slots] = {PhysicalUp, PhysicalRight, PhysicalDown, PhysicalLeft};
constexpr std::uint32_t PhysicalChord = PhysicalBack | PhysicalUp | PhysicalDown | PhysicalLeft | PhysicalRight;
// What the chord withholds from the game's native caches (WithholdNativeInputs):
// the fight map's directions (ReplayInputs.hxx: Up, Down, Left, Right) and its
// Select, the function of the pad's Back (ReplayInputs.hxx: the pad word's 0x2000).
constexpr unsigned NativeDirections = 0xF, NativeSelect = 0x2000;
constexpr unsigned NativeChord = NativeDirections | NativeSelect;

// What one pad update did.
struct QuickChatSample {
    // Bit n: slot n was sent this update.
    unsigned slots = 0;
    // The chord's physical buttons and the native ones it withholds, both 0
    // unless Back is held in a chord.
    std::uint32_t owned = 0;
    unsigned native = 0;
};
class QuickChatChord {
public:
    // physical: the pad's physical buttons now; enabled: a fight this PC
    // fights in, Ember not capturing the pads, quick lines on.
    QuickChatSample Update(bool enabled, std::uint32_t physical) {
        QuickChatSample sample;
        const bool back = (physical & PhysicalBack) != 0;
        const std::uint32_t fresh = physical & ~previous_;
        if (!enabled || !back) chording_ = false;
        else if (!back_) chording_ = true;
        back_ = back; previous_ = physical;
        if (!chording_) return sample;
        for (int slot = 0; slot < quickchat::Slots; ++slot) if (fresh & SlotDirection[slot]) sample.slots |= 1u << slot;
        sample.owned = PhysicalChord; sample.native = NativeChord;
        return sample;
    }
private:
    bool back_ = false, chording_ = false;
    // The physical word as of the last update: a direction held before the chord began is not fresh.
    std::uint32_t previous_ = 0;
};

// What a window message does with a quick line key (Overlay::OverlayWindowFunc).
struct QuickKey {
    // The slot a fresh press sends, -1 for none.
    int slot = -1;
    // The message is the chord's: the game and Ember never see it.
    bool keep = false;
};
// F1 to F4 going down, while active: a fresh press sends its slot, a repeat of
// a held key only stays out of the game. Anything else, or inactive, is left alone.
inline QuickKey QuickLineKey(unsigned message, unsigned long long key, long long lParam, bool active) {
    // WM_KEYDOWN and VK_F1, written out so this header needs no Windows headers.
    constexpr unsigned KeyDown = 0x100, F1 = 0x70;
    constexpr long long WasDown = 1LL << 30;
    QuickKey result;
    if (!active || message != KeyDown || key < F1 || key >= F1 + quickchat::Slots) return result;
    result.keep = true;
    if (!(lParam & WasDown)) result.slot = static_cast<int>(key - F1);
    return result;
}
} }
