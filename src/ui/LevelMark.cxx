#include "LevelMark.hxx"
#include "GameMenu.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace sf4e { namespace ui {
namespace level_mark {
Layout LayOut(unsigned number, float height) {
    Layout layout;
    if (!(height > 0)) return layout;
    const level_digits::Size* size = &level_digits::Sizes[level_digits::SizeCount - 1];
    for (const auto& candidate : level_digits::Sizes)
        if (candidate.cap >= height) { size = &candidate; break; }
    layout.size = size;
    layout.scale = height / size->cap;
    // The digits, the most significant last.
    int values[10], count = 0;
    do { values[count++] = static_cast<int>(number % 10); number /= 10; } while (number);
    // The pen runs in the size's own pixels from the first digit's ink.
    const int lead = size->digits[values[count - 1]].bearing;
    int pen = -lead, end = 0;
    for (int i = count - 1; i >= 0; --i) {
        const auto& digit = size->digits[values[i]];
        auto& placed = layout.digits[layout.count++];
        placed.digit = &digit;
        placed.x = std::round((pen + digit.bearing - size->pad) * layout.scale);
        placed.y = std::round((size->cap - size->baseline) * layout.scale);
        placed.w = digit.under.w * layout.scale;
        placed.h = digit.under.h * layout.scale;
        end = pen + digit.bearing + digit.ink;
        pen += digit.advance;
    }
    layout.width = std::round(end * layout.scale);
    return layout;
}
}

namespace v2 {
namespace {
constexpr int TopLevel = 60;
// A plate's number stands this share of its height tall, and the plate keeps
// this share of its height clear at each side of the number.
constexpr float PlateCap = .56f, PlateSide = .28f;
// A band takes this share of its picture's height (20 px at least), and its
// number this share of the band.
constexpr float BandShare = .3f, BandCap = .62f;

// A level's number `cap` tall: laid out in the digits once their atlas has
// loaded; while it loads, or when it is missing, the heading font's digits
// (`text`) in ivory. `width` is what either paints.
struct Number {
    SelectionImage atlas;
    level_mark::Layout layout;
    std::string text;
    float cap = 0, width = 0;
};
// The heading font's size whose digits stand about `cap` tall.
float FallbackSize(float cap) { return std::round(cap * 1.3f); }
Number NumberOf(int level, float cap) {
    Number number;
    number.cap = cap;
    if (auto* art = MenuArt()) number.atlas = art->LevelDigits();
    if (number.atlas.texture) {
        number.layout = level_mark::LayOut(static_cast<unsigned>(level), cap);
        number.width = number.layout.width;
    } else {
        number.text = std::to_string(level);
        number.width = std::round(HeadingFont()->CalcTextSizeA(FallbackSize(cap), FLT_MAX, 0, number.text.c_str()).x);
    }
    return number;
}
ImVec2 Uv(int x, int y) { return ImVec2(float(x) / level_digits::AtlasWidth, float(y) / level_digits::AtlasHeight); }
// The number centred in `box` and clipped to it, so its flame stays inside
// what frames it. The digits go every under sprite first, then every core
// sprite, so no glow covers a neighbour's stroke where slanted digits tuck.
void DrawCentred(ImDrawList* draw, const Number& number, Box box) {
    const ImVec2 at(std::round(box.min.x + (box.W() - number.width) * .5f), std::round(box.min.y + (box.H() - number.cap) * .5f));
    draw->PushClipRect(box.min, box.max, true);
    if (!number.atlas.texture) {
        const float size = FallbackSize(number.cap);
        draw->AddText(HeadingFont(), size, ImVec2(at.x, std::round(at.y + (number.cap - size) * .5f)), Colors().text, number.text.c_str());
    }
    else for (const bool core : {false, true})
        for (int i = 0; i < number.layout.count; ++i) {
            const auto& placed = number.layout.digits[i];
            const auto& cell = core ? placed.digit->core : placed.digit->under;
            const ImVec2 min(at.x + placed.x, at.y + placed.y);
            draw->AddImage(number.atlas.texture, min, ImVec2(min.x + placed.w, min.y + placed.h), Uv(cell.x, cell.y), Uv(cell.x + cell.w, cell.y + cell.h));
        }
    draw->PopClipRect();
}
// A plate `height` tall: its number, and its width, square at least.
struct Plate { Number number; float width; };
Plate PlateOf(int level, float height) {
    Plate plate{NumberOf(level, std::round(height * PlateCap)), 0};
    plate.width = (std::max)(std::round(height), plate.number.width + 2 * std::round(height * PlateSide));
    return plate;
}
}

Tier LevelTier(int level) {
    if (level <= 0) return Tier::None;
    return static_cast<Tier>(static_cast<int>(Tier::Ash) + ((std::min)(level, TopLevel) - 1) / 12);
}

float LevelWidth(int level, float height) { return level > 0 ? PlateOf(level, height).width : 0; }

float Level(ImDrawList* draw, ImVec2 at, float height, int level) {
    if (level <= 0) return 0;
    const Plate plate = PlateOf(level, height);
    const float s = Scale(), edge = 2 * s;
    const Box box{ImVec2(std::round(at.x), std::round(at.y)), ImVec2(std::round(at.x) + plate.width, std::round(at.y) + std::round(height))};
    draw->AddRectFilled(box.min, box.max, Colors().ink, 4 * s);
    DrawCentred(draw, plate.number, Inset(box, edge, edge));
    draw->AddRect(box.min, box.max, TierColor(LevelTier(level)).stripe, 4 * s, 0, edge);
    return plate.width;
}

void LevelBand(ImDrawList* draw, Box tile, int level) {
    if (level <= 0 || tile.H() <= 0) return;
    const float s = Scale(), band = std::round((std::min)(tile.H(), (std::max)(tile.H() * BandShare, 20 * s)));
    const Box foot{ImVec2(tile.min.x, tile.max.y - band), tile.max};
    draw->AddRectFilled(foot.min, foot.max, color::Alpha(Colors().ink, .88f));
    DrawCentred(draw, NumberOf(level, std::round(band * BandCap)), foot);
    draw->AddRect(tile.min, tile.max, TierColor(LevelTier(level)).stripe, 0, 0, 2 * s);
}

void Meter(ImDrawList* draw, Box box, float part, float fresh) {
    const auto& c = Colors();
    const float s = Scale(), r = 4 * s;
    part = (std::max)(0.f, (std::min)(part, 1.f));
    fresh = (std::max)(0.f, (std::min)(fresh, part));
    const float end = std::round(box.min.x + box.W() * part), from = std::round(box.min.x + box.W() * (part - fresh));
    draw->AddRectFilled(box.min, box.max, c.frame, r);
    if (end > box.min.x) draw->AddRectFilled(box.min, ImVec2(end, box.max.y), c.accent, r);
    if (end > from) draw->AddRectFilled(ImVec2(from, box.min.y), ImVec2(end, box.max.y), c.accentLight, r,
        from > box.min.x ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersAll);
    draw->AddRect(box.min, box.max, c.border, r, 0, s);
}
} } }
