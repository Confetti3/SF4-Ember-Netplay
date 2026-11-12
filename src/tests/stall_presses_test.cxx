#include "../common/StallPresses.hxx"
#include "../training/RecordingTypes.hxx"
#include <cstdlib>
#include <iostream>
#include "test_support.hxx"

int main() {
    using sf4e::StallPresses;
    using Word = StallPresses::Word;
    static_assert(StallPresses::AttackBits == sf4e::training::AttackButtons, "the game's six attack bits");
    constexpr unsigned LP = 0x10, HK = 0x800, Down = 0x2, Forward = 0x8;
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
        latch.Accepted({}, {});
        latch.Refused({LP, LP});
        latch.Refused({0, 0});
        const Word next{Down, Down};
        const Word sent = latch.Send(next);
        CHECK(same(sent, Word{Down | LP, Down | LP}));
        CHECK(latch.Accepted(next, sent) == 1);
        // The frame after sends the pad as it is: the game sees press, release.
        CHECK(same(latch.Send(next), next));
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
    // The two words latch apart (the mapped word can name another bit than the raw one).
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
