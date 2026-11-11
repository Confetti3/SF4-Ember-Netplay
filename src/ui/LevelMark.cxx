#include "LevelMark.hxx"
#include "../common/Localization.hxx"
#include "GameMenu.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>

namespace sf4e { namespace ui {
namespace level_mark {
Layout LayOut(unsigned number, float height, const point_digits::Glyphs& glyphs) {
    Layout layout;
    int tallest = 0;
    for (const auto& glyph : glyphs) tallest = (std::max)(tallest, glyph.h);
    if (!(height > 0) || tallest <= 0) return layout;
    layout.scale = height / tallest;
    // The digits, the most significant last.
    int values[10], count = 0;
    do { values[count++] = static_cast<int>(number % 10); number /= 10; } while (number);
    // The pen runs in the sheet's own pixels from the first digit's ink.
    int pen = 0;
    for (int i = count - 1; i >= 0; --i) {
        const auto& glyph = glyphs[values[i]];
        auto& placed = layout.digits[layout.count++];
        placed.source = glyph;
        placed.x = std::round(pen * layout.scale);
        placed.y = std::round((tallest - glyph.h) * layout.scale);
        placed.w = glyph.w * layout.scale;
        placed.h = glyph.h * layout.scale;
        layout.width = std::round((pen + glyph.w) * layout.scale);
        pen += glyph.w + Gap;
    }
    return layout;
}
}

namespace v2 {
namespace {
// The most points shown: six digits.
constexpr std::uint64_t TopPoints = 999999;
// A band takes this share of its picture's height (20 px at least), its number
// this share of the band, and keeps this share of the band clear at each side.
constexpr float BandShare = .22f, BandCap = .58f, BandMargin = .11f;

// White, as the game draws its points; the gold of its "rank up" digits for a gain.
constexpr ImU32 White = IM_COL32(255, 255, 255, 255), Gold = IM_COL32(255, 200, 64, 255);
// The dark outline round the label's letters, as the game's own digits have one.
constexpr ImU32 Outline = IM_COL32(10, 10, 14, 255);
// The space between a number and its label, and the label's outline, as shares
// of the digits' height; the gap after a gain's plus.
constexpr float Space = .3f, OutlineShare = 1.f / 14, GainGap = .16f;

// The art's digits, empty while the game's sheet loads or when it is missing.
PointDigitSheet DigitArt() {
    auto* art = MenuArt();
    return art ? art->PointDigits() : PointDigitSheet{};
}
// A number `cap` tall in one set of the game's digits, or, without the sheet,
// the heading font's digits (`text`) in the set's colour. `width` is what
// either paints.
struct Number {
    PointDigitSheet art;
    point_digits::Set set = point_digits::White;
    level_mark::Layout layout;
    std::string text;
    float cap = 0, width = 0;
};
// The heading font's size whose digits stand about `cap` tall.
float FallbackSize(float cap) { return std::round(cap * 1.3f); }
Number NumberOf(const PointDigitSheet& art, std::uint64_t value, float cap, point_digits::Set set) {
    value = (std::min)(value, TopPoints);
    Number number;
    number.art = art; number.set = set; number.cap = cap;
    if (art.sheet) {
        number.layout = level_mark::LayOut(static_cast<unsigned>(value), cap, art.sheet->sets[set]);
        number.width = number.layout.width;
    } else {
        number.text = std::to_string(value);
        number.width = std::round(HeadingFont()->CalcTextSizeA(FallbackSize(cap), FLT_MAX, 0, number.text.c_str()).x);
    }
    return number;
}
ImVec2 Uv(const point_digits::Box& box, int x, int y) { return ImVec2(float(box.x + x) / point_digits::SheetSide, float(box.y + y) / point_digits::SheetSide); }
// White text in the heading font with a dark outline: the text offset by about a
// fourteenth of `cap` in the eight directions in near black, then the text on top.
// At `alpha` of full strength (a number still being read).
void OutlinedText(ImDrawList* draw, float size, ImVec2 at, const char* text, ImU32 fill, float cap, float alpha) {
    const float edge = (std::max)(1.f, std::round(cap * OutlineShare));
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dy) draw->AddText(HeadingFont(), size, ImVec2(at.x + dx * edge, at.y + dy * edge), color::Alpha(Outline, alpha), text);
    draw->AddText(HeadingFont(), size, at, color::Alpha(fill, alpha), text);
}
// The number with its top left at `at`, at `alpha` of full strength.
void DrawNumber(ImDrawList* draw, const Number& number, ImVec2 at, float alpha = 1.f) {
    if (!number.art.sheet) {
        const float size = FallbackSize(number.cap);
        OutlinedText(draw, size, ImVec2(at.x, std::round(at.y + (number.cap - size) * .5f)), number.text.c_str(),
            number.set == point_digits::Gold ? Gold : White, number.cap, alpha);
        return;
    }
    for (int i = 0; i < number.layout.count; ++i) {
        const auto& placed = number.layout.digits[i];
        const ImVec2 min(at.x + placed.x, at.y + placed.y);
        draw->AddImage(number.art.image.texture, min, ImVec2(min.x + placed.w, min.y + placed.h),
            Uv(placed.source, 0, 0), Uv(placed.source, placed.source.w, placed.source.h), color::Alpha(White, alpha));
    }
}
// SF4's line under a player's name: the number, a space, then the two-letter
// label, the digits `cap` tall and the label's capitals as tall as they are.
struct Line {
    Number number;
    const char* label = "";
    float size = 0, space = 0, width = 0;
};
Line LineOf(const PointDigitSheet& art, std::uint64_t points, float cap, const char* labelId) {
    Line line;
    line.number = NumberOf(art, points, cap, point_digits::White);
    line.label = loc::T(labelId);
    line.size = FallbackSize(cap);
    line.space = std::round(cap * Space);
    line.width = line.number.width + line.space + std::round(HeadingFont()->CalcTextSizeA(line.size, FLT_MAX, 0, line.label).x);
    return line;
}
// The line with its top left at `at`.
void DrawLine(ImDrawList* draw, const Line& line, ImVec2 at, float alpha = 1.f) {
    DrawNumber(draw, line.number, at, alpha);
    OutlinedText(draw, line.size, ImVec2(at.x + line.number.width + line.space, std::round(at.y + (line.number.cap - line.size) * .5f)),
        line.label, White, line.number.cap, alpha);
}
}

Tier FrameTier(int tier) {
    return static_cast<Tier>(static_cast<int>(Tier::Ash) + (std::max)(0, (std::min)(tier, static_cast<int>(Tier::Ember) - static_cast<int>(Tier::Ash))));
}

float LevelWidth(std::uint64_t points, float height, const char* labelId) {
    return LineOf(DigitArt(), points, std::round(height), labelId).width;
}

float Level(ImDrawList* draw, ImVec2 at, float height, std::uint64_t points, const char* labelId, float alpha) {
    const Line line = LineOf(DigitArt(), points, std::round(height), labelId);
    DrawLine(draw, line, ImVec2(std::round(at.x), std::round(at.y)), alpha);
    return line.width;
}

float LevelLines(ImDrawList* draw, ImVec2 at, float height, float gap, std::uint64_t fighter, std::uint64_t account, float alpha) {
    const float a = LevelWidth(fighter, height, "points.label"), b = LevelWidth(account, height, "points.account_label"), wide = (std::max)(a, b);
    Level(draw, ImVec2(at.x + wide - a, at.y), height, fighter, "points.label", alpha);
    Level(draw, ImVec2(at.x + wide - b, at.y + height + gap), height, account, "points.account_label", alpha);
    return 2 * height + gap;
}

float LevelGain(ImDrawList* draw, ImVec2 at, float height, std::uint64_t gain) {
    if (gain == 0) return 0;
    // The sheet has no plus: it is drawn in the heading font, in the digits' gold.
    const float size = std::round(height * 1.3f);
    const float plus = std::round(HeadingFont()->CalcTextSizeA(size, FLT_MAX, 0, "+").x) + std::round(height * GainGap);
    const Number number = NumberOf(DigitArt(), gain, std::round(height), point_digits::Gold);
    const ImVec2 origin(std::round(at.x), std::round(at.y));
    draw->AddText(HeadingFont(), size, ImVec2(origin.x, std::round(origin.y + (height - size) * .5f)), Gold, "+");
    DrawNumber(draw, number, ImVec2(origin.x + plus, origin.y));
    return plus + number.width;
}

void LevelBand(ImDrawList* draw, Box tile, std::uint64_t points, float alpha) {
    if (tile.H() <= 0) return;
    const float s = Scale(), band = std::round((std::min)(tile.H(), (std::max)(tile.H() * BandShare, 20 * s)));
    const Box foot{ImVec2(tile.min.x, tile.max.y - band), tile.max};
    draw->AddRectFilled(foot.min, foot.max, color::Alpha(Colors().ink, .88f));
    // The line shrinks until it fits the picture's width less a margin each side, down to 1 px.
    const float room = (std::max)(1.f, foot.W() - 2 * std::round(band * BandMargin));
    float cap = std::round(band * BandCap);
    Line line = LineOf(DigitArt(), points, cap, "points.label");
    while (line.width > room && cap > 1) {
        cap = (std::max)(1.f, (std::min)(cap - 1, std::floor(cap * room / line.width)));
        line = LineOf(DigitArt(), points, cap, "points.label");
    }
    draw->PushClipRect(foot.min, foot.max, true);
    DrawLine(draw, line, ImVec2(std::round(foot.min.x + (foot.W() - line.width) * .5f), std::round(foot.min.y + (foot.H() - cap) * .5f)), alpha);
    draw->PopClipRect();
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
