#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "SelectionArt.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
DrawnFlag DrawCountryFlagBeside(ImDrawList* d, float x, float textTop, float fontSize, const CountryCode& country) {
    DrawnFlag drawn;
    auto* art = MenuArt();
    if (!country.Present() || !art) return drawn;
    const auto image = art->Flag(country);
    if (!image.texture) return drawn;
    // Whole pixels, so the edge is one crisp pixel wide.
    const float h = FlagHeight(fontSize), w = std::round(h * 4 / 3);
    const ImVec2 min(std::round(x), std::round(textTop + (fontSize - h) * .5f)), max(min.x + w, min.y + h);
    d->AddImage(image.texture, min, max, image.uvMin, image.uvMax);
    // The ring of pixels just outside the flag (AddRect strokes half a pixel in).
    d->AddRect(ImVec2(min.x - 1, min.y - 1), ImVec2(max.x + 1, max.y + 1), AccentColor(IM_COL32(128, 120, 110, 150)), 0, 0, 1);
    drawn.width = FlagSlotWidth(fontSize);
    drawn.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(min, max);
    return drawn;
}
std::string CountryName(const CountryCode& country) {
    if (!country.Present()) return {};
    std::string name = loc::T(country.Key());
    // Names are not in the baked interface text (loc::InterfaceText).
    NoteUserText(name);
    return name;
}
} }
