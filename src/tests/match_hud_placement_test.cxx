#include "../common/MatchHudPlacement.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "test_support.hxx"
#include <cmath>
#include <iostream>

int main() {
    using namespace sf4e;
    netplay::PlayerPreferences preferences;
    CHECK(preferences.matchHudPosition == 0 && preferences.Valid());
    for (int p = 0; p < 6; ++p) {
        preferences.matchHudPosition = p; CHECK(preferences.Valid());
        for (float viewportHeight : {480.f, 720.f, 1080.f, 1440.f, 2160.f})
        for (float aspect : {4.f/3, 16.f/9, 21.f/9})
        for (float size : {.85f, 1.f, 1.25f})
        for (float spacing : {12.f, 48.f}) {
            const float viewportWidth = viewportHeight * aspect;
            const float scale = (std::max)(.8f, viewportHeight / 1080.f) * size;
            const float width = (std::min)(520.f*scale, viewportWidth*.8f), height = 62.f*scale;
            const float gap = spacing * (std::max)(.8f, viewportHeight / 1080.f);
            const auto point = PlaceMatchHud(viewportWidth, viewportHeight, width, height, gap, p);
            CHECK(point.x >= 0 && point.y >= 0 && point.x + width <= viewportWidth + .01f && point.y + height <= viewportHeight + .01f);
            const float notice = MatchHudNoticeY(point.y, height, 24*scale, 4*scale, p);
            CHECK(notice >= 0 && notice + 24*scale <= viewportHeight);
            if (MatchHudOnTop(p)) CHECK(notice > point.y + height);
            else CHECK(notice < point.y);
        }
    }
    preferences.matchHudPosition = -1; CHECK(!preferences.Valid());
    preferences.matchHudPosition = 6; CHECK(!preferences.Valid());
    const auto original = PlaceMatchHud(1920,1080,520,62,12,0);
    CHECK(original.x == 700 && original.y == 1006);
    const auto top = PlaceMatchHud(1920,1080,520,62,12,1);
    CHECK(top.x == 700 && top.y == 12);
    CHECK(PlaceMatchHud(1920,1080,520,62,12,99).y == original.y);
    std::cout << "HUD presets, preferences and warning bounds passed\n";
}
