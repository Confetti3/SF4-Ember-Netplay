#include "../training/HitBoxes.hxx"
#include "test_support.hxx"

using namespace sf4e::training;

int main() {
    // A box the way the game lays one out: top left, top right, bottom right, bottom left.
    const float corners[16] = {1, 1.2f, 0, 1, 1.6f, 1.2f, 0, 1, 1.6f, .5f, 0, 1, 1, .5f, 0, 1};
    std::uint8_t record[0x40];
    std::memcpy(record, corners, sizeof(record));
    Box box{};
    CHECK(ReadBox(record, BoxKind::Hit, box));
    CHECK(box.x[0] == 1 && box.x[1] == 1.6f && box.y[0] == 1.2f && box.y[2] == .5f && box.kind == BoxKind::Hit);
    // An unused record is all zeros; one that was never written can hold anything.
    std::uint8_t empty[0x40] = {};
    CHECK(!ReadBox(empty, BoxKind::Hurt, box));
    const float infinite = INFINITY;
    std::memcpy(record + 0x14, &infinite, 4);
    CHECK(!ReadBox(record, BoxKind::Hurt, box));

    // The matrices of a training fight as the game held them: Player 1 stands
    // at x = -1.5, left of the middle and near the bottom of a 1920x1080 screen.
    const float view[16] = {1, 0, 0, 0, 0, .998146f, .0608626f, 0, 0, -.0608626f, .998146f, 0, 0, -1.27887f, 4.02963f, 1};
    const float projection[16] = {1.5454f, 0, 0, 0, 0, 2.74739f, 0, 0, 0, 0, 1.0001f, 1, 0, 0, -.10001f, 0};
    float sx = 0, sy = 0;
    CHECK(Project(view, projection, -1.5f, 0, 1920, 1080, sx, sy));
    CHECK(sx > 407 && sx < 409 && sy > 1010 && sy < 1012);
    CHECK(Project(view, projection, 0, 1.27887f / .998146f, 1920, 1080, sx, sy));
    CHECK(sx == 960 && sy > 539 && sy < 541);
    const float behind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1};
    CHECK(!Project(behind, projection, 0, 0, 1920, 1080, sx, sy));
    return 0;
}
