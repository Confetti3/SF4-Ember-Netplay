#include <cstdio>
#include <cstdlib>
#include "../common/QuickChatPad.hxx"

// The pad's chord and the keyboard's keys that send quick lines (QuickChatPad.hxx).
using namespace sf4e::input;
static void Check(bool pass, const char* why) {
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
constexpr std::uint32_t Back = PhysicalBack, Up = xinput::Up, Right = xinput::Right, Down = xinput::Down, Left = xinput::Left;

int main() {
    {
        // Each direction with Back held sends its slot, once per press.
        const std::uint32_t pressed[4] = {Up, Right, Down, Left};
        for (int slot = 0; slot < 4; ++slot) {
            QuickChatChord chord;
            Check(chord.Update(true, 0).slots == 0, "An idle pad sent a line");
            Check(chord.Update(true, Back).slots == 0, "Back alone sent a line");
            Check(chord.Update(true, Back | pressed[slot]).slots == (1u << slot), "A direction did not send its slot");
            Check(chord.Update(true, Back | pressed[slot]).slots == 0, "A held direction sent again");
            Check(chord.Update(true, Back).slots == 0, "Letting a direction go sent a line");
            Check(chord.Update(true, Back | pressed[slot]).slots == (1u << slot), "A second press did not send again");
        }
    }
    {
        // Nothing without Back, and a direction held before Back is not a press.
        QuickChatChord chord;
        Check(chord.Update(true, Up).slots == 0 && chord.Update(true, Up).native == 0, "A direction alone sent a line or was withheld");
        const auto held = chord.Update(true, Up | Back);
        Check(held.slots == 0 && held.native == NativeChord, "A direction held before Back sent a line, or Back's chord was not withheld");
        Check(chord.Update(true, Up | Right | Back).slots == (1u << 1), "A second direction under the chord did not send");
    }
    {
        // Back and the directions are withheld while Back is held, and that ends with it.
        QuickChatChord chord;
        Check(chord.Update(true, 0).native == 0, "An idle pad withheld a button");
        const auto down = chord.Update(true, Back);
        Check(down.native == NativeChord, "Back did not withhold the chord");
        Check((down.native & NativeDirections) == NativeDirections && (down.native & NativeSelect) == NativeSelect, "The chord left a native button");
        const auto up = chord.Update(true, 0);
        Check(up.native == 0 && up.slots == 0, "A released chord kept its buttons");
    }
    {
        // Disabled: nothing is withheld or sent, and a chord held when it is disabled ends there.
        QuickChatChord chord;
        const auto off = chord.Update(false, Back | Up);
        Check(off.slots == 0 && off.native == 0, "A disabled chord withheld or sent");
        Check(chord.Update(true, Back | Up | Right).slots == 0, "A Back held since before the chord was allowed began one");
        chord.Update(true, 0);
        chord.Update(true, Back);
        Check(chord.Update(true, Back | Down).slots == (1u << 2), "A chord pressed afresh did not send");
        const auto ended = chord.Update(false, Back | Down);
        Check(ended.slots == 0 && ended.native == 0, "A held chord kept its buttons once disabled");
        Check(chord.Update(true, Back | Down | Left).slots == 0, "A chord that ended came back without a fresh Back");
    }
    {
        // F1 to F4 as window messages.
        constexpr unsigned KeyDown = 0x100, KeyUp = 0x101;
        constexpr unsigned long long F1 = 0x70, F4 = 0x73, F5 = 0x74;
        for (unsigned long long key = F1; key <= F4; ++key) {
            const auto fresh = QuickLineKey(KeyDown, key, 1, true);
            Check(fresh.keep && fresh.slot == static_cast<int>(key - F1), "A fresh F key did not send its slot and stay out of the game");
            const auto repeat = QuickLineKey(KeyDown, key, (1LL << 30) | 1, true);
            Check(repeat.keep && repeat.slot == -1, "A repeat sent again or reached the game");
            const auto inactive = QuickLineKey(KeyDown, key, 1, false);
            Check(!inactive.keep && inactive.slot == -1, "An F key was taken outside a fight");
        }
        Check(!QuickLineKey(KeyDown, F5, 1, true).keep && !QuickLineKey(KeyDown, 0x41, 1, true).keep, "A key past F4 was taken");
        Check(!QuickLineKey(KeyUp, F1, 1, true).keep && !QuickLineKey(0x104, F1, 1, true).keep, "A key release or system key was taken");
        Check(!QuickLineKey(KeyDown, 0x6F, 1, true).keep, "A key before F1 was taken");
    }
    std::puts("quick_chat_pad_test: ok");
    return 0;
}
