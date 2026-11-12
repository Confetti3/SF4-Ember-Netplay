#pragma once
#include "../training/HitBoxes.hxx"
#include <cstdint>
#include <vector>

// The fighters' boxes over the fight, as the player sees them: a
// training::BoxView (training/HitBoxes.hxx) and nothing else, so any game
// that fills that view draws its boxes with these. SF4's Training lab
// (TrainingPanel's DrawTrainingBoxes) draws them under its Hitboxes page's
// switch and parts; a game without that lab offers its switch on Training
// select (game::Capabilities::hitboxes) and draws them alone.
//
// The shapes are worked out apart from the drawing (HitboxShapesOf, no ImGui
// in it), so what a view becomes on the screen is tested without a frame.
namespace sf4e { namespace ui {
// The parts of the boxes the player may hide (SF4's Hitboxes page: its
// hurt, throwable and skeleton rows). An attack's boxes, its throw boxes and
// its proximity boxes are always drawn.
struct HitboxOverlayParts { bool hurt = true, throwable = true, skeleton = true; };

// A colour as ImGui packs IM_COL32 (red in the low byte); HitboxOverlay.cxx
// checks the packing against ImGui's own.
constexpr std::uint32_t HitboxRgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(g) << 8) | r;
}
// Each kind's outline and fill, by training::BoxKind: green where a fighter
// can be hit, yellow where it can be thrown, red where an attack hits and
// blue where it throws, as SF4's lab has always drawn them; an orange outline
// with no fill where an attack makes the opponent guard, since a proximity
// box is large and would hide the fighters under it.
inline std::uint32_t HitboxLine(training::BoxKind kind) {
    switch (kind) {
    case training::BoxKind::Hurt: return HitboxRgba(60, 220, 90, 230);
    case training::BoxKind::Throwable: return HitboxRgba(240, 220, 60, 230);
    case training::BoxKind::Hit: return HitboxRgba(255, 60, 60, 230);
    case training::BoxKind::Throw: return HitboxRgba(70, 140, 255, 230);
    case training::BoxKind::Proximity: return HitboxRgba(255, 150, 40, 230);
    }
    return 0;
}
inline std::uint32_t HitboxFill(training::BoxKind kind) {
    switch (kind) {
    case training::BoxKind::Hurt: return HitboxRgba(60, 220, 90, 90);
    case training::BoxKind::Throwable: return HitboxRgba(240, 220, 60, 70);
    case training::BoxKind::Hit: return HitboxRgba(255, 60, 60, 110);
    case training::BoxKind::Throw: return HitboxRgba(70, 140, 255, 110);
    case training::BoxKind::Proximity: return 0;
    }
    return 0;
}
constexpr std::uint32_t HitboxBoneColor = HitboxRgba(255, 255, 255, 220);

// A box as drawn: its corners on the screen, in order round it, and its colours.
struct HitboxQuad { float x[4], y[4]; std::uint32_t fill, line; };
// A bone of the skeleton as drawn: its two ends on the screen.
struct HitboxBone { float x[2], y[2]; };
struct HitboxShapes { std::vector<HitboxQuad> quads; std::vector<HitboxBone> bones; };

// What a view becomes on a screen whose top left is (left, top): every box
// the parts show whose corners are all in front of the camera, in the
// view's order, then the skeleton's bones. Empty when the view is not valid.
inline HitboxShapes HitboxShapesOf(const training::BoxView& view, const HitboxOverlayParts& parts, float left, float top, float width, float height) {
    HitboxShapes shapes;
    if (!view.valid) return shapes;
    const auto project = [&](float x, float y, float& screenX, float& screenY) {
        const bool seen = training::Project(view.view, view.projection, x, y, width, height, screenX, screenY);
        screenX += left; screenY += top;
        return seen;
    };
    for (const auto& box : view.boxes) {
        if ((box.kind == training::BoxKind::Hurt && !parts.hurt) || (box.kind == training::BoxKind::Throwable && !parts.throwable)) continue;
        HitboxQuad quad{};
        bool seen = true;
        for (int corner = 0; corner < 4 && seen; ++corner) seen = project(box.x[corner], box.y[corner], quad.x[corner], quad.y[corner]);
        if (!seen) continue;
        quad.fill = HitboxFill(box.kind); quad.line = HitboxLine(box.kind);
        shapes.quads.push_back(quad);
    }
    if (parts.skeleton) for (const auto& bone : view.bones) {
        HitboxBone drawn{};
        if (!project(bone.x[0], bone.y[0], drawn.x[0], drawn.y[0]) || !project(bone.x[1], bone.y[1], drawn.x[1], drawn.y[1])) continue;
        shapes.bones.push_back(drawn);
    }
    return shapes;
}

// Draws a view's boxes over the main viewport, behind every window (ImGui's
// background list); nothing when the view is not valid.
void DrawHitboxOverlay(const training::BoxView& view, const HitboxOverlayParts& parts);
} }
