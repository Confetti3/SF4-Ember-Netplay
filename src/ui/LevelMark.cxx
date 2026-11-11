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
// A plate's number stands this share of its height tall, and the plate keeps
// this share of its height clear at each side of the number.
constexpr float PlateCap = .44f, PlateSide = .22f;
// A band takes this share of its picture's height (20 px at least), and its
// number this share of the band.
constexpr float BandShare = .22f, BandCap = .58f;

// White, as the game draws its points; the gold of its "rank up" digits for a gain.
constexpr ImU32 White = IM_COL32(255, 255, 255, 255), Gold = IM_COL32(255, 200, 64, 255);
// The label before a plate's number stands this share of the plate's height, and
// is set off from the plate by this share of it.
constexpr float LabelSize = .34f, LabelGap = .16f;

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
// The number with its top left at `at`.
void DrawNumber(ImDrawList* draw, const Number& number, ImVec2 at) {
    if (!number.art.sheet) {
        const float size = FallbackSize(number.cap);
        draw->AddText(HeadingFont(), size, ImVec2(at.x, std::round(at.y + (number.cap - size) * .5f)), number.set == point_digits::Gold ? Gold : White, number.text.c_str());
        return;
    }
    for (int i = 0; i < number.layout.count; ++i) {
        const auto& placed = number.layout.digits[i];
        const ImVec2 min(at.x + placed.x, at.y + placed.y);
        draw->AddImage(number.art.image.texture, min, ImVec2(min.x + placed.w, min.y + placed.h),
            Uv(placed.source, 0, 0), Uv(placed.source, placed.source.w, placed.source.h));
    }
}
// The number centred in `box` and clipped to it.
void DrawCentred(ImDrawList* draw, const Number& number, Box box) {
    draw->PushClipRect(box.min, box.max, true);
    DrawNumber(draw, number, ImVec2(std::round(box.min.x + (box.W() - number.width) * .5f), std::round(box.min.y + (box.H() - number.cap) * .5f)));
    draw->PopClipRect();
}
// A plate `height` tall: its number, and its width, square at least.
struct Plate { Number number; float width; };
Plate PlateOf(const PointDigitSheet& art, std::uint64_t points, float height) {
    Plate plate{NumberOf(art, points, std::round(height * PlateCap), point_digits::White), 0};
    plate.width = (std::max)(std::round(height), plate.number.width + 2 * std::round(height * PlateSide));
    return plate;
}
// The label's text size, and the room it and its gap take before a plate `height` tall.
float LabelFont(float height) { return std::round(height * LabelSize); }
float LabelRoom(float height) {
    return std::round(HeadingFont()->CalcTextSizeA(LabelFont(height), FLT_MAX, 0, loc::T("points.label")).x) + std::round(height * LabelGap);
}
// The number in a band `band` tall over `width`: its cap, shrunk until it fits
// the width less a margin each side, down to 1 px.
Number BandNumber(const PointDigitSheet& art, std::uint64_t points, float band, float width) {
    float cap = std::round(band * BandCap);
    const float room = (std::max)(1.f, width - 2 * std::round(band * PlateSide * .5f));
    Number number = NumberOf(art, points, cap, point_digits::White);
    while (number.width > room && cap > 1) {
        cap = (std::max)(1.f, (std::min)(cap - 1, std::floor(cap * room / number.width)));
        number = NumberOf(art, points, cap, point_digits::White);
    }
    return number;
}
}

Tier FrameTier(int tier) {
    return static_cast<Tier>(static_cast<int>(Tier::Ash) + (std::max)(0, (std::min)(tier, static_cast<int>(Tier::Ember) - static_cast<int>(Tier::Ash))));
}

float LevelWidth(std::uint64_t points, float height) {
    return points > 0 ? LabelRoom(height) + PlateOf(DigitArt(), points, height).width : 0;
}

float Level(ImDrawList* draw, ImVec2 at, float height, std::uint64_t points, int tier) {
    if (points == 0) return 0;
    const Plate plate = PlateOf(DigitArt(), points, height);
    const float s = Scale(), edge = 2 * s, room = LabelRoom(height), label = LabelFont(height);
    const ImVec2 origin(std::round(at.x), std::round(at.y));
    draw->AddText(HeadingFont(), label, ImVec2(origin.x, std::round(origin.y + (height - label) * .5f)), White, loc::T("points.label"));
    const Box box{ImVec2(origin.x + room, origin.y), ImVec2(origin.x + room + plate.width, origin.y + std::round(height))};
    draw->AddRectFilled(box.min, box.max, Colors().ink, 4 * s);
    DrawCentred(draw, plate.number, Inset(box, edge, edge));
    draw->AddRect(box.min, box.max, TierColor(FrameTier(tier)).stripe, 4 * s, 0, edge);
    return room + plate.width;
}

float LevelGain(ImDrawList* draw, ImVec2 at, float height, std::uint64_t gain) {
    if (gain == 0) return 0;
    // The sheet has no plus: it is drawn in the heading font, in the digits' gold.
    const float size = std::round(height * 1.3f);
    const float plus = std::round(HeadingFont()->CalcTextSizeA(size, FLT_MAX, 0, "+").x) + std::round(height * LabelGap);
    const Number number = NumberOf(DigitArt(), gain, std::round(height), point_digits::Gold);
    const ImVec2 origin(std::round(at.x), std::round(at.y));
    draw->AddText(HeadingFont(), size, ImVec2(origin.x, std::round(origin.y + (height - size) * .5f)), Gold, "+");
    DrawNumber(draw, number, ImVec2(origin.x + plus, origin.y));
    return plus + number.width;
}

void LevelBand(ImDrawList* draw, Box tile, std::uint64_t points, int tier) {
    if (points == 0 || tile.H() <= 0) return;
    const float s = Scale(), band = std::round((std::min)(tile.H(), (std::max)(tile.H() * BandShare, 20 * s)));
    const Box foot{ImVec2(tile.min.x, tile.max.y - band), tile.max};
    draw->AddRectFilled(foot.min, foot.max, color::Alpha(Colors().ink, .88f));
    DrawCentred(draw, BandNumber(DigitArt(), points, band, foot.W()), foot);
    draw->AddRect(tile.min, tile.max, TierColor(FrameTier(tier)).stripe, 0, 0, 2 * s);
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
