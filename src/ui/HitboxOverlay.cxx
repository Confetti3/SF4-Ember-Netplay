#include "HitboxOverlay.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {
static_assert(HitboxRgba(1, 2, 3, 4) == IM_COL32(1, 2, 3, 4), "HitboxRgba packs a colour as IM_COL32 does");

void DrawHitboxOverlay(const training::BoxView& view, const HitboxOverlayParts& parts) {
    if (!view.valid) return;
    const auto* vp = ImGui::GetMainViewport();
    auto* draw = ImGui::GetBackgroundDrawList();
    const HitboxShapes shapes = HitboxShapesOf(view, parts, vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
    for (const auto& quad : shapes.quads) {
        const ImVec2 at[4] = {{quad.x[0], quad.y[0]}, {quad.x[1], quad.y[1]}, {quad.x[2], quad.y[2]}, {quad.x[3], quad.y[3]}};
        if (quad.fill) draw->AddQuadFilled(at[0], at[1], at[2], at[3], quad.fill);
        draw->AddQuad(at[0], at[1], at[2], at[3], quad.line, 2);
    }
    for (const auto& bone : shapes.bones) {
        const ImVec2 from(bone.x[0], bone.y[0]), to(bone.x[1], bone.y[1]);
        draw->AddLine(from, to, HitboxBoneColor, 2);
        draw->AddCircleFilled(from, 3, HitboxBoneColor);
        draw->AddCircleFilled(to, 3, HitboxBoneColor);
    }
}
} }
