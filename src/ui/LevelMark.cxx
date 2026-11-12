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
    // The faces, each glyph standing on the baseline as it is drawn below.
    float faceTop = FLT_MAX, faceBottom = -FLT_MAX;
    for (const auto& glyph : glyphs) {
        if (glyph.faceH <= 0) continue;
        const int above = tallest - glyph.h + glyph.faceY - glyph.y;
        faceTop = (std::min)(faceTop, above * layout.scale);
        faceBottom = (std::max)(faceBottom, (above + glyph.faceH) * layout.scale);
    }
    layout.faceTop = faceTop <= faceBottom ? faceTop : 0;
    layout.faceBottom = faceTop <= faceBottom ? faceBottom : height;
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
// Where a heading-font character's ink stands at `size`: its top and bottom
// below the top of the line the text is drawn on (the glyph's Y0 and Y1).
struct Ink { float top = 0, bottom = 0; };
Ink InkOf(ImWchar c, float size) {
    ImFont* font = HeadingFont();
    const ImFontGlyph* glyph = font->FindGlyph(c);
    if (!glyph || !(glyph->Y1 > glyph->Y0) || !(font->FontSize > 0)) return {size * .2f, size * .9f};
    const float k = size / font->FontSize;
    return {glyph->Y0 * k, glyph->Y1 * k};
}
// The heading font's size whose capitals stand `height` tall, measured on the
// F (flat at its top and on its baseline).
float CapitalSize(float height) {
    const Ink unit = InkOf('F', HeadingFont()->FontSize);
    return unit.bottom > unit.top ? height * HeadingFont()->FontSize / (unit.bottom - unit.top) : std::round(height * 1.3f);
}
// A number `cap` tall in one set of the game's digits, or, without the sheet,
// the heading font's digits (`text`) in the set's colour. `width` is what
// either paints. The face is where the digits' light fill starts and ends
// below the number's top: the art's measured faces, or, for the font's
// digits, the whole `cap`, which their capitals' height fills from the top.
struct Number {
    PointDigitSheet art;
    point_digits::Set set = point_digits::White;
    level_mark::Layout layout;
    std::string text;
    float cap = 0, width = 0, faceTop = 0, faceBottom = 0;
};
// The heading font set beside a number: its size, whose capitals are as tall
// as the number's face, and how far below the number's top its line starts so
// that the capitals' top and baseline are level with the face's.
struct FontBeside { float size = 0, y = 0; };
FontBeside FontBesideOf(const Number& number) {
    FontBeside beside;
    beside.size = CapitalSize(number.faceBottom - number.faceTop);
    beside.y = number.faceTop - InkOf('F', beside.size).top;
    return beside;
}
Number NumberOf(const PointDigitSheet& art, std::uint64_t value, float cap, point_digits::Set set) {
    value = (std::min)(value, TopPoints);
    Number number;
    number.art = art; number.set = set; number.cap = cap;
    if (art.sheet) {
        number.layout = level_mark::LayOut(static_cast<unsigned>(value), cap, art.sheet->sets[set]);
        number.width = number.layout.width;
        number.faceTop = number.layout.faceTop;
        number.faceBottom = number.layout.faceBottom;
    } else {
        number.text = std::to_string(value);
        number.faceBottom = cap;
        number.width = std::round(HeadingFont()->CalcTextSizeA(FontBesideOf(number).size, FLT_MAX, 0, number.text.c_str()).x);
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
        const FontBeside text = FontBesideOf(number);
        OutlinedText(draw, text.size, ImVec2(at.x, std::round(at.y + text.y)), number.text.c_str(),
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
// The game's lettering, empty while its font is read or when it is missing.
GameLetterSheet LetterArt() {
    auto* art = MenuArt();
    return art ? art->GameLetters() : GameLetterSheet{};
}
// Text set beside a number (a label, a gain's plus), its capitals as tall as
// the number's face with their top and baseline level with the face's: in the
// game's own lettering, as SF4 sets its PP and BP, beside the game's digits
// when the lettering has every character of the text; otherwise, and beside
// the heading font's digits, in the heading font. `width` is its advance.
struct Beside {
    GameLetterSheet letters;
    FontBeside font;
    const char* text = "";
    float top = 0, bottom = 0, width = 0;
};
Beside BesideOf(const Number& number, const char* text) {
    Beside beside;
    beside.text = text;
    beside.top = number.faceTop; beside.bottom = number.faceBottom;
    if (number.art.sheet) {
        const GameLetterSheet letters = LetterArt();
        if (letters.sheet && letters.sheet->Covers(text)) beside.letters = letters;
    }
    if (const auto* sheet = beside.letters.sheet.get()) {
        const float k = (beside.bottom - beside.top) / (sheet->capBottom - sheet->capTop);
        float pen = 0;
        for (const char* c = text; *c; ++c) pen += sheet->Find(*c)->advance * k;
        beside.width = std::round(pen);
    } else {
        beside.font = FontBesideOf(number);
        beside.width = std::round(HeadingFont()->CalcTextSizeA(beside.font.size, FLT_MAX, 0, text).x);
    }
    return beside;
}
// The game's lettering of `beside` with its pen at `left` and the number's top
// at `top`, in `fill`, each glyph from the sheet's nearest size at least as
// tall; outlined as OutlinedText outlines, with an `edge` of 0 for none.
void DrawLetters(ImDrawList* draw, const Beside& beside, float left, float top, ImU32 fill, float edge, float alpha) {
    const auto& sheet = *beside.letters.sheet;
    const float cap = beside.bottom - beside.top, k = cap / (sheet.capBottom - sheet.capTop);
    const int size = game_lettering::Sheet::SizeFor(cap);
    const float ratio = k / sheet.scale[size], baseline = std::round(top + beside.bottom);
    const auto pass = [&](float dx, float dy, ImU32 color) {
        float pen = std::round(left);
        for (const char* c = beside.text; *c; ++c) {
            const auto& glyph = *sheet.Find(*c);
            const auto& box = glyph.box[size];
            const ImVec2 min(pen + glyph.left[size] * ratio + dx, baseline + glyph.top[size] * ratio + dy);
            draw->AddImage(beside.letters.image.texture, min, ImVec2(min.x + box.w * ratio, min.y + box.h * ratio),
                ImVec2(float(box.x) / game_lettering::SheetWidth, float(box.y) / game_lettering::SheetHeight),
                ImVec2(float(box.x + box.w) / game_lettering::SheetWidth, float(box.y + box.h) / game_lettering::SheetHeight), color::Alpha(color, alpha));
            pen += glyph.advance * k;
        }
    };
    if (edge > 0)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dy) pass(dx * edge, dy * edge, Outline);
    pass(0, 0, fill);
}
// `beside` with its left at `left` beside a number whose top is at `top`, outlined.
void DrawBeside(ImDrawList* draw, const Beside& beside, float left, float top, ImU32 fill, float cap, float alpha) {
    if (beside.letters.sheet) DrawLetters(draw, beside, left, top, fill, (std::max)(1.f, std::round(cap * OutlineShare)), alpha);
    else OutlinedText(draw, beside.font.size, ImVec2(left, std::round(top + beside.font.y)), beside.text, fill, cap, alpha);
}
// SF4's line under a player's name: the number, a space, then the two-letter
// label, its capitals as tall as the digits' faces and on their baseline.
struct Line {
    Number number;
    Beside label;
    float space = 0, width = 0;
};
Line LineOf(const PointDigitSheet& art, std::uint64_t points, float cap, const char* labelId) {
    Line line;
    line.number = NumberOf(art, points, cap, point_digits::White);
    line.label = BesideOf(line.number, loc::T(labelId));
    line.space = std::round(cap * Space);
    line.width = line.number.width + line.space + line.label.width;
    return line;
}
// The line with its top left at `at`. With a `column` wider than the number,
// the number is right-aligned in it and the label starts after it, so lines
// stacked with one column share their numbers' right edge and labels' left.
void DrawLine(ImDrawList* draw, const Line& line, ImVec2 at, float alpha = 1.f, float column = 0) {
    column = (std::max)(column, line.number.width);
    DrawNumber(draw, line.number, ImVec2(at.x + column - line.number.width, at.y), alpha);
    DrawBeside(draw, line.label, at.x + column + line.space, at.y, White, line.number.cap, alpha);
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
    const PointDigitSheet art = DigitArt();
    const float cap = std::round(height);
    const Line top = LineOf(art, fighter, cap, "points.label"), bottom = LineOf(art, account, cap, "points.account_label");
    // One column of numbers, right-aligned, and the labels after it at one x.
    const float column = (std::max)(top.number.width, bottom.number.width);
    const ImVec2 origin(std::round(at.x), std::round(at.y));
    DrawLine(draw, top, origin, alpha, column);
    DrawLine(draw, bottom, ImVec2(origin.x, std::round(origin.y + height + gap)), alpha, column);
    return 2 * height + gap;
}

float LevelGain(ImDrawList* draw, ImVec2 at, float height, std::uint64_t gain) {
    if (gain == 0) return 0;
    // The sheet has no plus: it is the game's lettering's (on the digits'
    // baseline, as a label is), or the heading font's centred on the digits'
    // faces, in the digits' gold.
    const Number number = NumberOf(DigitArt(), gain, std::round(height), point_digits::Gold);
    const Beside plus = BesideOf(number, "+");
    const float advance = plus.width + std::round(height * GainGap);
    const ImVec2 origin(std::round(at.x), std::round(at.y));
    if (plus.letters.sheet) DrawLetters(draw, plus, origin.x, origin.y, Gold, 0, 1.f);
    else {
        const Ink ink = InkOf('+', plus.font.size);
        draw->AddText(HeadingFont(), plus.font.size, ImVec2(origin.x, std::round(origin.y + (number.faceTop + number.faceBottom - ink.top - ink.bottom) * .5f)), Gold, "+");
    }
    DrawNumber(draw, number, ImVec2(origin.x + advance, origin.y));
    return advance + number.width;
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
