#pragma once
#include <algorithm>

namespace sf4e {
// Persisted presentation values. Bottom center is the existing default.
enum class MatchHudPosition { BottomCenter, TopCenter, TopLeft, TopRight, BottomLeft, BottomRight };
inline bool ValidMatchHudPosition(int value) { return value >= 0 && value <= 5; }
inline bool MatchHudOnTop(int value) { return value >= 1 && value <= 3; }
struct MatchHudPoint { float x, y; };
inline MatchHudPoint PlaceMatchHud(float viewportWidth, float viewportHeight,
    float width, float height, float gap, int position) {
    if (!ValidMatchHudPosition(position)) position = 0;
    const float maxX = (std::max)(0.f, viewportWidth - width);
    const float maxY = (std::max)(0.f, viewportHeight - height);
    const auto bound = [](float value, float maximum) { return (std::max)(0.f, (std::min)(value, maximum)); };
    const bool left = position == 2 || position == 4, right = position == 3 || position == 5;
    return {left ? bound(gap, maxX) : right ? bound(maxX - gap, maxX) : maxX * .5f,
        MatchHudOnTop(position) ? bound(gap, maxY) : bound(maxY - gap, maxY)};
}
// Top-anchored warnings grow downward. Bottom warnings retain the old upward
// placement, so neither can disappear beyond its chosen screen edge.
inline float MatchHudNoticeY(float panelY, float panelHeight, float noticeHeight, float gap, int position) {
    return MatchHudOnTop(position) ? panelY + panelHeight + gap : panelY - noticeHeight - gap;
}
}
