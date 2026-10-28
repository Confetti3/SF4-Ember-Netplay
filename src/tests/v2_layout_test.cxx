// The V2 layout rules (src/ui/V2.hxx): margins by width, the content cap, and box cuts.
#include "../ui/V2.hxx"
#include <cstdio>

int main() {
    using namespace sf4e::ui::v2;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL: %s\n", what); } };
    check(Pad(800, 1) == 16 && Pad(1280, 1) == 24 && Pad(1920, 1) == 40, "pad is 16 / 24 / 40 by width");
    check(Pad(2560, 2) == 24 * 2, "pad reads logical width at scale 2");
    check(Narrow(899, 1) && !Narrow(900, 1), "narrow below 900");
    check(SideMargin(1920, 1) == 40, "1080p keeps its pad");
    check(SideMargin(5120, 1) == (5120 - 2000) / 2.f, "32:9 centres 2000 of content");
    Box screen{{0, 0}, {1280, 800}};
    const Box bar = CutBottom(screen, 56, 16);
    check(bar.min.y == 744 && screen.max.y == 728, "bottom cut leaves its gap");
    const Box side = CutRight(screen, 400, 24);
    check(side.min.x == 880 && screen.max.x == 856, "right cut leaves its gap");
    const Box head = CutTop(screen, 2000);
    check(head.H() == 728 && screen.H() == 0, "a cut takes no more than there is");
    std::printf(failures ? "%d failures\n" : "V2 layout: all checks passed\n", failures);
    return failures != 0;
}
