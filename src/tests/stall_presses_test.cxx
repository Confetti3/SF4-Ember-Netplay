#include "../common/StallPresses.hxx"
#include "../training/RecordingTypes.hxx"
#include <cstdlib>
#include <iostream>
#include "test_support.hxx"

int main() {
    using sf4e::StallPresses;
    using Word = StallPresses::Word;
    static_assert(StallPresses::AttackBits == sf4e::training::AttackButtons, "the game's six attack bits");
    constexpr unsigned LP = 0x10, LK = 0x40, MK = 0x80, HK = 0x800, Down = 0x2, Forward = 0x8;
    auto same = [](Word a, Word b) { return a.mapped == b.mapped && a.raw == b.raw; };

    // No stall: the sample goes out as read.
    {
        StallPresses latch;
        const Word held{LP | Down, LP | Down};
        CHECK(same(latch.Send(held), held));
        CHECK(latch.Accepted(held, latch.Send(held)) == 0);
    }
    // A tap made and let go inside a stall rides on the next accepted frame,
    // once, and is counted.
    {
        StallPresses latch;
        latch.Refused({LP, LP});
        latch.Refused({0, 0});
        const Word next{0, 0};
        const Word sent = latch.Send(next);
        CHECK(same(sent, Word{LP, LP}));
        CHECK(latch.Accepted(next, sent) == 1);
        // The frame after sends the pad as it is: the game sees press, release.
        CHECK(same(latch.Send(next), next));
    }
    // With the stick where it was when the button went down: crouching MK stays crouching MK.
    {
        StallPresses latch;
        latch.Refused({Down | MK, Down | MK});
        latch.Refused({Down, Down});
        CHECK(same(latch.Send({Down, Down}), Word{Down | MK, Down | MK}));
    }
    // A moved stick drops the press: it would be another move.
    {
        StallPresses latch;
        latch.Refused({Down | MK, Down | MK});
        latch.Refused({Forward, Forward});
        const Word sent = latch.Send({Forward, Forward});
        CHECK(same(sent, Word{Forward, Forward}));
        CHECK(latch.Accepted({Forward, Forward}, sent) == 0);
    }
    // Two taps inside one stall would land as a chord (LP+LK is a throw): none is carried.
    {
        StallPresses latch;
        latch.Refused({LP, LP});
        latch.Refused({0, 0});
        latch.Refused({LK, LK});
        latch.Refused({0, 0});
        CHECK(same(latch.Send({0, 0}), Word{0, 0}));
        latch.Accepted({0, 0}, {0, 0});
        // The next stall starts clean.
        latch.Refused({HK, HK});
        CHECK(same(latch.Send({0, 0}), Word{HK, HK}));
    }
    // Two buttons pressed together are a chord the player made, but one frame
    // cannot tell it from two taps: none is carried.
    {
        StallPresses latch;
        latch.Refused({LP | LK, LP | LK});
        CHECK(same(latch.Send({0, 0}), Word{0, 0}));
    }
    // A press still held when the stall ends is the sample itself: not counted.
    {
        StallPresses latch;
        latch.Refused({HK, HK});
        const Word next{HK, HK};
        const Word sent = latch.Send(next);
        CHECK(same(sent, next));
        CHECK(latch.Accepted(next, sent) == 0);
    }
    // A button held in the last frame sent makes no new press: nothing to carry.
    {
        StallPresses latch;
        latch.Accepted({LP, LP}, {LP, LP});
        latch.Refused({LP, LP});
        latch.Refused({0, 0});
        const Word sent = latch.Send({0, 0});
        CHECK(same(sent, Word{0, 0}));
        CHECK(latch.Accepted({0, 0}, sent) == 0);
    }
    // Directions are never latched: they could finish a motion never made.
    {
        StallPresses latch;
        latch.Refused({Down | Forward, Down | Forward});
        CHECK(same(latch.Send({0, 0}), Word{0, 0}));
    }
    // The raw word rides with the mapped press (it can name another bit).
    {
        StallPresses latch;
        latch.Refused({LP, HK});
        CHECK(same(latch.Send({0, 0}), Word{LP, HK}));
    }
    // Reset forgets a stall from the last session.
    {
        StallPresses latch;
        latch.Refused({LP, LP});
        latch.Reset();
        CHECK(same(latch.Send({0, 0}), Word{0, 0}));
    }
    std::cout << "Stall presses passed\n";
    return 0;
}
