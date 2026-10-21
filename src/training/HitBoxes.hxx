#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// The fighters' collision boxes as the game holds them during a fight, and the
// camera they are drawn through. Owns only the model and the byte layout of
// one box; it knows nothing of the game or the overlay.
namespace sf4e { namespace training {
// Hurt: where a strike lands. Throwable: where a throw connects. Hit, Throw:
// an attack's, on the frames it can touch.
enum class BoxKind : std::uint8_t { Hurt, Throwable, Hit, Throw };
// A box in the fight's own units, on its plane (z = 0): its four corners, in
// order round it. Not always upright: a box on a leaning body leans with it,
// and the upright rectangle round it reaches further than the box does.
// side: whose it is, 0 or 1.
struct Box { float x[4], y[4]; BoxKind kind; std::uint8_t side; };
// view, projection: the game's camera matrices, 16 floats each, rows first.
// valid: both were read this frame.
struct BoxView { bool valid = false; float view[16] = {}, projection[16] = {}; std::vector<Box> boxes; };

// One box of the game: its four corners, 16 bytes each (x, y, z and one more
// float). Reads the 0x40 bytes at record. False for a box without area or
// with a number that is not one, which is how an unused record reads.
inline bool ReadBox(const std::uint8_t* record, BoxKind kind, Box& box) {
    for (int corner = 0; corner < 4; ++corner) {
        float at[2];
        std::memcpy(at, record + corner * 0x10, sizeof(at));
        if (!std::isfinite(at[0]) || !std::isfinite(at[1])) return false;
        box.x[corner] = at[0]; box.y[corner] = at[1];
    }
    // Twice the area, by the corners' cross products.
    float area = 0;
    for (int corner = 0; corner < 4; ++corner) area += box.x[corner] * box.y[(corner + 1) % 4] - box.x[(corner + 1) % 4] * box.y[corner];
    if (std::fabs(area) < .001f) return false;
    box.kind = kind;
    return true;
}

// A point of the fight plane on a screen of the given size: a point is
// multiplied from the left, as Direct3D does. False when it is behind the camera.
inline bool Project(const float* view, const float* projection, float x, float y, float width, float height, float& screenX, float& screenY) {
    const float in[4] = {x, y, 0, 1};
    float eye[4], clip[4];
    for (int column = 0; column < 4; ++column) eye[column] = in[0] * view[column] + in[1] * view[4 + column] + in[2] * view[8 + column] + in[3] * view[12 + column];
    for (int column = 0; column < 4; ++column) clip[column] = eye[0] * projection[column] + eye[1] * projection[4 + column] + eye[2] * projection[8 + column] + eye[3] * projection[12 + column];
    if (clip[3] <= 1e-4f) return false;
    screenX = (clip[0] / clip[3] * .5f + .5f) * width;
    screenY = (.5f - clip[1] / clip[3] * .5f) * height;
    return true;
}
} }
