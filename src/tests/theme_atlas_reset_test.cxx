// A device reset gives the overlay a new ImGui context over the same font
// atlas (EMBER-70): the atlas must not be baked again, and the new context
// must still get the theme's font, scale and style.
#include "../ui/Theme.hxx"
#include <imgui.h>
#include <cmath>
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
    ImGui::DestroyContext();

    if (failures) return 1;
    std::puts("ThemeAtlasReset: ok");
    return 0;
}
