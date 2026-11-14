// A device reset gives the overlay a new ImGui context over the same font
// atlas (EMBER-70): the atlas must not be baked again, and the new context
// must still get the theme's font, scale and style. A rebuild only for player
// text waits while the address space has no free range twice the atlas's size.
#include "../ui/Theme.hxx"
#include <imgui.h>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}
}

int main() {
    ImFontAtlas atlas;
    ImGui::CreateContext(&atlas);
    Check(sf4e::ui::ApplyTheme(1.5f), "The first context did not build the atlas");
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
    const float scrollbar = ImGui::GetStyle().ScrollbarSize;
    ImGui::DestroyContext();

    ImGui::CreateContext(&atlas);
    auto& io = ImGui::GetIO();
    Check(!sf4e::ui::ApplyTheme(1.5f), "A new context over the kept atlas baked it again");
    unsigned char* kept = nullptr;
    atlas.GetTexDataAsRGBA32(&kept, &width, &height);
    Check(kept != nullptr && kept == pixels, "The kept atlas's pixels were allocated again");
    Check(std::fabs(io.FontGlobalScale - 1.5f) < .001f, "The new context kept ImGui's default scale");
    Check(atlas.Fonts.Size == 3 && io.FontDefault == atlas.Fonts[0], "The new context kept ImGui's default font");
    Check(std::fabs(ImGui::GetStyle().ScrollbarSize - scrollbar) < .001f && scrollbar != ImGuiStyle().ScrollbarSize,
        "The new context kept ImGui's default style");
    Check(!sf4e::ui::ApplyTheme(1.5f), "A themed context rebuilt the atlas");
    Check(sf4e::ui::ApplyTheme(2.f), "A new scale kept the old atlas");
    Check(std::fabs(io.FontGlobalScale - 2.f) < .001f, "A rebuild for a new scale kept the old scale");

    // The decision itself: twice the atlas in one free range.
    static_assert(sf4e::ui::AtlasRebuildFits(80u << 20, 40u << 20), "Exactly twice the atlas did not fit");
    static_assert(!sf4e::ui::AtlasRebuildFits((80u << 20) - 2, 40u << 20), "Less than twice the atlas fit");
    static_assert(sf4e::ui::AtlasRebuildFits(1u << 30, 40u << 20), "A large free range did not fit an atlas");

    // Player text the atlas lacks, with no room to bake it: the atlas stays.
    sf4e::ui::SetUserGlyphRebuildInterval(std::chrono::milliseconds(0));
    std::size_t room = 0;
    int measured = 0;
    sf4e::ui::SetLargestFreeRange([&] { ++measured; return room; });
    ImFont* const body = atlas.Fonts[0];
    sf4e::ui::NoteUserText("\xE6\xBC\xA2");
    Check(!sf4e::ui::ApplyTheme(2.f), "A rebuild for player text ran with no room for the atlas");
    Check(atlas.Fonts.Size == 3 && atlas.Fonts[0] == body, "The skipped rebuild changed the atlas");
    const int measuredOnce = measured;
    Check(!sf4e::ui::ApplyTheme(2.f) && measured == measuredOnce, "A skipped rebuild measured the address space again at once");
    // Room again (a new measure forgets the wait): the player text is baked.
    room = static_cast<std::size_t>(1) << 30;
    sf4e::ui::SetLargestFreeRange([&] { ++measured; return room; });
    Check(sf4e::ui::ApplyTheme(2.f), "A rebuild for player text did not run once there was room");
    // A scale change is not optional: it rebuilds with no room.
    room = 0;
    sf4e::ui::NoteUserText("\xE5\xAD\x97");
    Check(sf4e::ui::ApplyTheme(2.5f), "A new scale waited for room");
    Check(std::fabs(io.FontGlobalScale - 2.5f) < .001f, "A rebuild for a new scale kept the old scale");
    ImGui::DestroyContext();

    if (failures) return 1;
    std::puts("ThemeAtlasReset: ok");
    return 0;
}
