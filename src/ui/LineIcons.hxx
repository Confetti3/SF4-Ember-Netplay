#pragma once
// Ember's line icons: Tabler Icons outlines (LineIcons.inc, written by
// scripts/build-line-icons.mjs) stroked on the draw list, so they stay sharp
// at every interface scale and take any ink.
#include <imgui.h>

namespace sf4e { namespace ui {
enum class LineIcon {
#define SF4E_LINE_ICON(name, source, path) name,
#include "LineIcons.inc"
#undef SF4E_LINE_ICON
};
// `icon` in a `size` square at `at`, stroked in `ink` `weight` wide on its 24
// grid (Tabler draws them at 2).
void DrawLineIcon(ImDrawList* draw, LineIcon icon, ImVec2 at, float size, ImU32 ink, float weight = 2);
} }
