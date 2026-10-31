#include "LineIcons.hxx"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace sf4e { namespace ui {
namespace {
const char* const Paths[] = {
#define SF4E_LINE_ICON(name, source, path) path,
#include "LineIcons.inc"
#undef SF4E_LINE_ICON
};
constexpr float Grid = 24;

// An SVG arc from `from` to `to` as ImGui's arc about its centre (SVG 1.1, F.6.5).
// Circles only (rx == ry), which is all the icons draw; build-line-icons.mjs
// lets through no arc without a radius or between one point and itself.
void ArcTo(ImDrawList* d, ImVec2 from, float r, bool large, bool sweep, ImVec2 to) {
    const float hx = (from.x - to.x) * .5f, hy = (from.y - to.y) * .5f;
    if (r <= 0 || (hx == 0 && hy == 0)) { d->PathLineTo(to); return; }
    r *= (std::max)(1.f, std::sqrt(hx * hx + hy * hy) / r);
    const float half = hx * hx + hy * hy;
    const float root = (large == sweep ? -1.f : 1.f) * std::sqrt((std::max)(0.f, (r * r - half) / half));
    const float cx = root * hy + (from.x + to.x) * .5f, cy = -root * hx + (from.y + to.y) * .5f;
    const float a0 = std::atan2(from.y - cy, from.x - cx);
    float a1 = std::atan2(to.y - cy, to.x - cx);
    if (sweep && a1 < a0) a1 += 2 * 3.14159265f;
    if (!sweep && a1 > a0) a1 -= 2 * 3.14159265f;
    d->PathArcTo(ImVec2(cx, cy), r, a0, a1, static_cast<int>(8 + r));
}
}

// The path's M, L, H, V, C, S, A and Z, absolute or relative, each subpath
// stroked as it ends. An open one gets Tabler's round caps, which ImGui's
// stroke lacks: Tabler draws a dot (Help's, Info's) as a line of no length.
void DrawLineIcon(ImDrawList* d, LineIcon icon, ImVec2 at, float size, ImU32 ink, float weight) {
    const float k = size / Grid;
    const auto P = [&](float x, float y) { return ImVec2(at.x + x * k, at.y + y * k); };
    const char* c = Paths[static_cast<int>(icon)];
    char command = 0, last = 0;
    // The pen, the subpath's start and the last curve's second control point (for S).
    float x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0;
    // A subpath's points, each on top of the one before dropped first (an arc
    // repeats the pen; "h 0" adds it again): ImGui's stroke spikes out of a
    // join at a segment of no length. An open subpath of two points or more
    // keeps its caps even when it shrinks to one point, a dot; a lone point
    // (an M followed by another, the pen Z leaves) draws nothing.
    const auto stroke = [&](bool closed) {
        auto& path = d->_Path;
        const bool caps = !closed && path.Size >= 2;
        const auto same = [](ImVec2 a, ImVec2 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) < 1e-4f; };
        int kept = 0;
        for (const ImVec2& point : path) if (!kept || !same(point, path[kept - 1])) path[kept++] = point;
        if (closed && kept > 1 && same(path[kept - 1], path[0])) --kept;
        path.resize(kept);
        const ImVec2 first = kept ? path.front() : ImVec2(), end = kept ? path.back() : ImVec2();
        d->PathStroke(ink, closed ? ImDrawFlags_Closed : 0, weight * k);
        if (caps) {
            d->AddCircleFilled(first, weight * k * .5f, ink);
            d->AddCircleFilled(end, weight * k * .5f, ink);
        }
    };
    const auto number = [&] { char* end; const float v = std::strtof(c, &end); c = end; while (*c == ' ' || *c == ',') ++c; return v; };
    while (*c == ' ') ++c;
    while (*c) {
        if (std::isalpha(static_cast<unsigned char>(*c))) { command = *c++; while (*c == ' ') ++c; }
        const bool rel = std::islower(static_cast<unsigned char>(command));
        const float ox = rel ? x : 0, oy = rel ? y : 0;
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        switch (upper) {
        case 'M':
            if (!d->_Path.empty()) stroke(false);
            x = ox + number(); y = oy + number(); sx = x; sy = y;
            d->PathLineTo(P(x, y));
            command = rel ? 'l' : 'L';  // further pairs are lines
            break;
        case 'L': x = ox + number(); y = oy + number(); d->PathLineTo(P(x, y)); break;
        case 'H': x = ox + number(); d->PathLineTo(P(x, y)); break;
        case 'V': y = oy + number(); d->PathLineTo(P(x, y)); break;
        case 'C': case 'S': {
            // S's first control point mirrors the last curve's second, or is the pen after anything else.
            float x1 = x, y1 = y;
            if (upper == 'C') { x1 = ox + number(); y1 = oy + number(); }
            else if (last == 'C' || last == 'S') { x1 = 2 * x - cx; y1 = 2 * y - cy; }
            cx = ox + number(); cy = oy + number();
            x = ox + number(); y = oy + number();
            d->PathBezierCubicCurveTo(P(x1, y1), P(cx, cy), P(x, y));
            break;
        }
        case 'A': {
            const float r = number();
            number(); number();
            const bool large = number() != 0, sweep = number() != 0;
            const ImVec2 from = P(x, y);
            x = ox + number(); y = oy + number();
            ArcTo(d, from, r * k, large, sweep, P(x, y));
            break;
        }
        // A command after Z starts its subpath where Z left the pen.
        case 'Z': x = sx; y = sy; stroke(true); d->PathLineTo(P(x, y)); break;
        default: return;  // build-line-icons.mjs lets no other command through
        }
        last = upper;
    }
    if (!d->_Path.empty()) stroke(false);
}
} }
