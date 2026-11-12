// The game's lettering for the points' labels (GameLettering.hxx): a font
// library holding the game's font is read and its glyphs drawn into the sheet
// at each size; a library without it, or with a glyph missing or cut short,
// is refused. The library here is made in the test: every glyph a box.
#include "../common/GameLettering.hxx"
#include "game_lettering_support.hxx"
#include <cstdio>
#include <string>
#include <vector>

namespace {
namespace lettering = sf4e::game_lettering;
int failures = 0;
void Check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}
using lettering_support::Library;
}

int main() {
    const std::string all = lettering::Characters;
    lettering::Sheet sheet;
    std::vector<unsigned char> bgra;
    std::string why;
    auto library = Library(lettering::FontName, "abc" + all + "xyz");
    Check(lettering::Read(library.data(), library.size(), sheet, bgra, why), "the library was not read: " + why);
    Check(bgra.size() == static_cast<std::size_t>(lettering::SheetWidth) * lettering::SheetHeight * 4, "the sheet is not its size");
    Check(sheet.glyphs.size() == all.size(), "the sheet does not hold every character");
    Check(sheet.capTop == -15360 && sheet.capBottom == 0, "the capital height is not the F's");
    Check(sheet.Covers("FP") && sheet.Covers("BP") && sheet.Covers("+") && !sheet.Covers("Fp") && !sheet.Covers(""), "the characters covered are wrong");
    Check(lettering::Sheet::SizeFor(10) == 0 && lettering::Sheet::SizeFor(16) == 0 && lettering::Sheet::SizeFor(17) == 1 &&
        lettering::Sheet::SizeFor(40) == 2 && lettering::Sheet::SizeFor(200) == 2, "the size chosen for a height is wrong");
    if (const auto* f = sheet.Find('F')) {
        Check(f->advance == 14000, "the advance is not the font's");
        for (int size = 0; size < lettering::SizeCount; ++size) {
            const std::string at = "size " + std::to_string(lettering::Sizes[size]);
            const auto& box = f->box[size];
            const float scale = sheet.scale[size];
            Check(scale * 15360 == static_cast<float>(lettering::Sizes[size]), at + ": the capitals are not that tall");
            // The glyph's ink, its padding clear round it, its top on the capital line.
            Check(f->top[size] == -lettering::Sizes[size] - lettering::Padding && box.h == lettering::Sizes[size] + 2 * lettering::Padding, at + ": the box is not the ink and padding");
            const auto alpha = [&](int x, int y) { return bgra[((box.y + y) * lettering::SheetWidth + box.x + x) * 4 + 3]; };
            Check(alpha(box.w / 2, box.h / 2) == 255 && alpha(0, 0) == 0 && alpha(box.w - 1, box.h - 1) == 0, at + ": the glyph is not filled inside its padding");
            Check(bgra[((box.y + box.h / 2) * lettering::SheetWidth + box.x + box.w / 2) * 4] == 255, at + ": the glyph is not white");
            Check(box.x + box.w <= lettering::SheetWidth && box.y + box.h <= lettering::SheetHeight, at + ": the glyph is outside the sheet");
        }
    } else Check(false, "no F");
    // Refused: another font, a character missing, a glyph cut short, nothing.
    library = Library("FOT-Seurat Pro DB", all);
    Check(!lettering::Read(library.data(), library.size(), sheet, bgra, why) && why.find("no FOT-RodinNTLG Pro B") == 0, "another font was read");
    library = Library(lettering::FontName, all.substr(1));
    Check(!lettering::Read(library.data(), library.size(), sheet, bgra, why) && why == "no glyph A", "a library without A was read");
    library = Library(lettering::FontName, all, true);
    Check(!lettering::Read(library.data(), library.size(), sheet, bgra, why), "a glyph cut short was read");
    library.resize(40);
    Check(!lettering::Read(library.data(), library.size(), sheet, bgra, why), "a cut library was read");
    Check(!lettering::Read(nullptr, 0, sheet, bgra, why), "nothing was read");
    std::printf(failures ? "%d failures\n" : "Game lettering: all checks passed\n", failures);
    return failures != 0;
}
