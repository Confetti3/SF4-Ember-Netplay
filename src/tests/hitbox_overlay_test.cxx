#include "../ui/HitboxOverlay.hxx"
#include "test_support.hxx"
#include <algorithm>
#include <iterator>
#include <vector>

// SF4's Training lab drew the fighters' boxes itself until they moved into
// the neutral overlay (ui/HitboxOverlay; TrainingPanel's DrawTrainingBoxes
// at 24ca041b9). That drawing is kept below as it was, call for call, and the
// overlay must give the same calls for every view and every set of the
// Hitboxes page's parts: SF4 draws exactly what it drew before.
using namespace sf4e::training;
using sf4e::ui::HitboxParts;

namespace {
// ImGui's IM_COL32 packing (red in the low byte), as SF4's drawing used it.
std::uint32_t Col32(unsigned r, unsigned g, unsigned b, unsigned a) { return (a << 24) | (b << 16) | (g << 8) | r; }

// One draw call: a filled quad, an outlined quad, a line or a filled circle,
// its points and its colour.
enum class Draw { QuadFilled, Quad, Line, Circle };
struct Call {
    Draw draw; std::vector<float> points; std::uint32_t color;
    bool operator==(const Call& other) const { return draw == other.draw && points == other.points && color == other.color; }
};

// SF4's DrawTrainingBoxes before the move, on a viewport at (left, top) of
// the given size, with the Hitboxes page's switch on.
std::vector<Call> Sf4Before(const BoxView& view, bool hurtBoxes, bool throwBoxes, bool skeleton, float left, float top, float width, float height) {
    std::vector<Call> calls;
    if (!view.valid) return calls;
    const std::uint32_t line[] = {Col32(60, 220, 90, 230), Col32(240, 220, 60, 230), Col32(255, 60, 60, 230), Col32(70, 140, 255, 230)};
    const std::uint32_t fill[] = {Col32(60, 220, 90, 90), Col32(240, 220, 60, 70), Col32(255, 60, 60, 110), Col32(70, 140, 255, 110)};
    const auto project = [&](float x, float y, float& atX, float& atY) {
        const bool seen = Project(view.view, view.projection, x, y, width, height, atX, atY);
        atX += left; atY += top;
        return seen;
    };
    for (const auto& box : view.boxes) {
        if ((box.kind == BoxKind::Hurt && !hurtBoxes) || (box.kind == BoxKind::Throwable && !throwBoxes)) continue;
        float x[4], y[4];
        bool seen = true;
        for (int corner = 0; corner < 4 && seen; ++corner) seen = project(box.x[corner], box.y[corner], x[corner], y[corner]);
        if (!seen) continue;
        const int kind = static_cast<int>(box.kind);
        const std::vector<float> points = {x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3]};
        calls.push_back({Draw::QuadFilled, points, fill[kind]});
        calls.push_back({Draw::Quad, points, line[kind]});
    }
    if (skeleton) for (const auto& bone : view.bones) {
        float fx, fy, tx, ty;
        if (!project(bone.x[0], bone.y[0], fx, fy) || !project(bone.x[1], bone.y[1], tx, ty)) continue;
        calls.push_back({Draw::Line, {fx, fy, tx, ty}, Col32(255, 255, 255, 220)});
        calls.push_back({Draw::Circle, {fx, fy}, Col32(255, 255, 255, 220)});
        calls.push_back({Draw::Circle, {tx, ty}, Col32(255, 255, 255, 220)});
    }
    return calls;
}

// The neutral overlay's calls (DrawHitboxOverlay over HitboxShapesOf).
std::vector<Call> Overlay(const BoxView& view, const HitboxParts& parts, float left, float top, float width, float height) {
    std::vector<Call> calls;
    if (!view.valid) return calls;
    const auto shapes = sf4e::ui::HitboxShapesOf(view, parts, left, top, width, height);
    for (const auto& quad : shapes.quads) {
        const std::vector<float> points = {quad.x[0], quad.y[0], quad.x[1], quad.y[1], quad.x[2], quad.y[2], quad.x[3], quad.y[3]};
        if (quad.fill) calls.push_back({Draw::QuadFilled, points, quad.fill});
        calls.push_back({Draw::Quad, points, quad.line});
    }
    for (const auto& bone : shapes.bones) {
        calls.push_back({Draw::Line, {bone.x[0], bone.y[0], bone.x[1], bone.y[1]}, sf4e::ui::HitboxBoneColor});
        calls.push_back({Draw::Circle, {bone.x[0], bone.y[0]}, sf4e::ui::HitboxBoneColor});
        calls.push_back({Draw::Circle, {bone.x[1], bone.y[1]}, sf4e::ui::HitboxBoneColor});
    }
    return calls;
}

Box Upright(float left, float bottom, float right, float top, BoxKind kind, std::uint8_t side) {
    return Box{{left, right, right, left}, {top, top, bottom, bottom}, kind, side};
}
} // namespace

int main() {
    // A training fight's camera as SF4 held it (hit_boxes_test): Player 1 at
    // x = -1.5, Player 2 at 1.5, each with a hurt, a throwable, a hit and a
    // throw box, a leaning box and two bones; one box and one bone reach
    // behind the camera.
    BoxView view;
    view.valid = true;
    const float camera[16] = {1, 0, 0, 0, 0, .998146f, .0608626f, 0, 0, -.0608626f, .998146f, 0, 0, -1.27887f, 4.02963f, 1};
    const float projection[16] = {1.5454f, 0, 0, 0, 0, 2.74739f, 0, 0, 0, 0, 1.0001f, 1, 0, 0, -.10001f, 0};
    std::copy(std::begin(camera), std::end(camera), view.view);
    std::copy(std::begin(projection), std::end(projection), view.projection);
    for (std::uint8_t side = 0; side < 2; ++side) {
        const float at = side ? 1.5f : -1.5f, ahead = side ? -1.f : 1.f;
        view.boxes.push_back(Upright(at - .3f, 0, at + .3f, 1.7f, BoxKind::Hurt, side));
        view.boxes.push_back(Upright(at - .2f, .2f, at + .2f, 1.4f, BoxKind::Throwable, side));
        view.boxes.push_back(Upright(at + ahead * .2f, 1.1f, at + ahead * .9f, 1.3f, BoxKind::Hit, side));
        view.boxes.push_back(Upright(at + ahead * .1f, .5f, at + ahead * .6f, 1.2f, BoxKind::Throw, side));
        view.boxes.push_back(Box{{at - .1f, at + .3f, at + .5f, at + .1f}, {1.5f, 1.6f, 1.2f, 1.1f}, BoxKind::Hurt, side});
        view.bones.push_back(Bone{{at, at + ahead * .4f}, {1.2f, 1.3f}, side});
        view.bones.push_back(Bone{{at, at}, {.9f, 1.2f}, side});
    }
    // Reaching behind the camera (z = -4 is in front of it; this is far behind).
    view.boxes.push_back(Box{{0, .5f, .5f, 0}, {0, 0, -90, -90}, BoxKind::Hurt, 0});
    view.bones.push_back(Bone{{0, 0}, {0, -90}, 1});
    CHECK(!Sf4Before(view, true, true, true, 0, 0, 1920, 1080).empty());

    // Every set of the Hitboxes page's parts, on viewports with and without
    // an offset: the same calls as SF4 made.
    for (int mask = 0; mask < 8; ++mask) {
        HitboxParts parts;
        parts.hurt = (mask & 1) != 0; parts.throwable = (mask & 2) != 0; parts.skeleton = (mask & 4) != 0;
        CHECK(Overlay(view, parts, 0, 0, 1920, 1080) == Sf4Before(view, parts.hurt, parts.throwable, parts.skeleton, 0, 0, 1920, 1080));
        CHECK(Overlay(view, parts, 12, 34, 1280, 720) == Sf4Before(view, parts.hurt, parts.throwable, parts.skeleton, 12, 34, 1280, 720));
    }
    // The box and the bone behind the camera are left out, as SF4 left them.
    const auto all = sf4e::ui::HitboxShapesOf(view, HitboxParts{}, 0, 0, 1920, 1080);
    CHECK(all.quads.size() == 10 && all.bones.size() == 4);
    // A view that was not read draws nothing.
    BoxView unread = view;
    unread.valid = false;
    CHECK(Overlay(unread, HitboxParts{}, 0, 0, 1920, 1080).empty());

    // SFV's proximity box (a kind SF4's view never has): an orange outline
    // with no fill, drawn whatever the parts hide.
    BoxView proximity = view;
    proximity.boxes = {Upright(-1.2f, 0, .8f, .6f, BoxKind::Proximity, 0)};
    proximity.bones.clear();
    HitboxParts none; none.hurt = none.throwable = none.skeleton = false;
    const auto calls = Overlay(proximity, none, 0, 0, 1920, 1080);
    CHECK(calls.size() == 1 && calls[0].draw == Draw::Quad && calls[0].color == Col32(255, 150, 40, 230));
    CHECK(sf4e::ui::HitboxFill(BoxKind::Proximity) == 0);
    return 0;
}
