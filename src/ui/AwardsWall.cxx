#include "AwardsWall.hxx"
#include "CardModel.hxx"
#include "NameMarks.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui { namespace awards_wall {
namespace aw = netplay::awards;

std::string LevelHow(const aw::Level& level) {
    return level.band ? loc::Tf("award.rank.how", ClassName(static_cast<std::uint8_t>(level.band))) : std::string(loc::T(level.how));
}

std::string LevelLines(const aw::Counters& counters, const aw::Family& family, bool counted) {
    const int level = aw::LevelOf(counters, family);
    const auto next = aw::Next(counters, family);
    std::string lines;
    for (std::size_t i = 0; i < family.count; ++i) {
        const auto& one = family.levels[i];
        std::string state;
        if (static_cast<int>(i) < level) state = loc::T("awards.earned");
        else if (static_cast<int>(i) == level && counted && aw::Counts(family)) state = loc::Tf("awards.progress", next.have, next.need);
        if (!lines.empty()) lines += "\n";
        lines += v2::TierName(v2::FrameTier(one.tier)) + " \xC2\xB7 " + player_card::LevelTitle(one) + " \xC2\xB7 " + LevelHow(one) + (state.empty() ? "" : " \xC2\xB7 " + state);
    }
    return lines;
}

std::string StateOf(const Tile& tile) {
    const auto& family = *tile.family;
    if (tile.next.top) return loc::T("awards.top_level");
    if (!tile.counted) return loc::T("awards.locked");
    if (aw::Counts(family)) return loc::Tf("awards.progress", tile.next.have, tile.next.need);
    return loc::Tf("awards.next", player_card::LevelTitle(family.levels[tile.level]));
}

void DrawTile(ImDrawList* d, v2::Box tile, const Tile& shown, bool focused, bool hovered) {
    const auto& c = v2::Colors();
    const float s = Scale(), icon = 52 * s, pip = 8 * s, pipGap = 3 * s;
    const aw::Family& family = *shown.family;
    const bool earned = shown.level > 0;
    v2::CardGround(d, tile, focused, hovered);
    if (shown.worn && !focused) d->AddRect(tile.min, tile.max, v2::color::Alpha(c.accent, .5f), 4 * s, 0, Hairline());
    // The name on two lines when it needs them; the icon, name, pips and progress centred as a stack.
    auto* font = HeadingFont();
    const float size = 14 * s, inner = tile.W() - 16 * s;
    const char *text = shown.name.c_str(), *stop = text + shown.name.size();
    const char* cut = font->CalcTextSizeA(size, FLT_MAX, 0, text, stop).x > inner ? font->CalcWordWrapPositionA(size / font->FontSize, text, stop, inner) : stop;
    const float lines = cut < stop ? 2.f : 1.f;
    const auto tier = v2::TierColor(earned ? v2::FrameTier(aw::LevelAt(family, shown.level)->tier) : v2::Tier::None);
    // How far the next level is: a thin meter over "37 of 100" for a count, the next level's name for a class or a frame, or the top.
    const bool meter = shown.counted && !shown.next.top && aw::Counts(family);
    const float stack = icon + 6 * s + lines * (size + 2 * s) + 6 * s + pip + 6 * s + (meter ? 9 * s : 0) + 14 * s;
    const ImVec2 box(tile.min.x + (tile.W() - icon) * .5f, tile.min.y + (std::max)(6 * s, (tile.H() - stack) * .5f));
    // The tier is the plate under the icon and the icon's colour, never a frame.
    d->AddRectFilled(box, ImVec2(box.x + icon, box.y + icon), earned ? tier.plate : c.frame, 4 * s);
    v2::Icon(d, family.icon, ImVec2(box.x + 10 * s, box.y + 10 * s), icon - 20 * s, earned ? tier.stripe : v2::color::Alpha(c.muted, .5f));
    const auto line = [&](const char* from, const char* to, float top) {
        const float w = font->CalcTextSizeA(size, FLT_MAX, 0, from, to).x;
        d->AddText(font, size, ImVec2(tile.min.x + (tile.W() - w) * .5f, top), earned && shown.live ? c.text : c.muted, from, to);
    };
    d->PushClipRect(tile.min, tile.max, true);
    line(text, cut, box.y + icon + 6 * s);
    while (cut < stop && *cut == ' ') ++cut;
    if (cut < stop) line(cut, stop, box.y + icon + 8 * s + size);
    // A pip for each level, in its own tier's colour up to the level reached.
    float py = box.y + icon + 10 * s + lines * (size + 2 * s);
    const float pips = family.count * pip + (family.count - 1) * pipGap;
    for (std::size_t i = 0; i < family.count; ++i) {
        const float px = tile.min.x + (tile.W() - pips) * .5f + i * (pip + pipGap);
        const bool reached = static_cast<int>(i) < shown.level;
        const ImVec2 p0(px, py), p1(px + pip, py + pip);
        if (reached) d->AddRectFilled(p0, p1, v2::TierColor(v2::FrameTier(family.levels[i].tier)).stripe);
        else { d->AddRectFilled(p0, p1, c.frame); d->AddRect(p0, p1, c.border, 0, 0, Hairline()); }
    }
    py += pip + 6 * s;
    if (meter) {
        v2::Meter(d, {ImVec2(tile.min.x + 12 * s, py), ImVec2(tile.max.x - 12 * s, py + 5 * s)}, shown.next.Part(), 0.f);
        py += 9 * s;
    }
    const std::string state = StateOf(shown);
    const float sw = ImGui::GetFont()->CalcTextSizeA(player_card::TextFloor * s, FLT_MAX, 0, state.c_str()).x;
    d->AddText(ImGui::GetFont(), player_card::TextFloor * s, ImVec2(tile.min.x + (std::max)(4 * s, (tile.W() - sw) * .5f), py), shown.worn ? c.accent : c.muted, state.c_str());
    d->PopClipRect();
    if (focused) v2::Focus(d, tile.min, tile.max, false);
}

} } }
