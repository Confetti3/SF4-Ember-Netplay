#pragma once
#include "LineIcons.hxx"
#include "MenuRows.hxx"
#include "V2.hxx"
#include "../platform/ApplicationServices.hxx"
#include <string>
namespace sf4e { namespace ui {
// The row that asks to open `link` in the browser, "link-<id>": Help & About lists them, Settings >
// Discord ends with the first, and Home puts them over the player's card as icons. Every one is a
// confirmation and submits the same OpenLink request, so a link behaves alike wherever it is pressed.
// While the service reports on this link, its message takes the detail's place.
inline MenuEntry LinkRow(const platform::Link& link, const platform::ServiceSnapshot& services, std::string label = std::string()) {
    const bool reported = services.lastAction == platform::ServiceAction::OpenLink && services.lastLink == &link && !services.message.empty();
    return ConfirmRow(std::string("link-") + link.id, label.empty() ? std::string(loc::T(link.label)) : std::move(label),
        reported ? services.message : loc::Tf(link.detail, link.address), !services.pending);
}
// What a row of LinkRow asks for: its link, or null for any other row.
inline const platform::Link* LinkOfRow(const std::string& rowId) {
    return rowId.compare(0, 5, "link-") == 0 ? platform::FindLink(rowId.substr(5)) : nullptr;
}
// How a link's icon button draws its mark (v2::IconButton). The Ember mark is the brand's bitmap, which takes the
// ink's alpha: it dims with a disabled button and has no other state of its own.
inline v2::IconDraw LinkIcon(platform::LinkMark mark) {
    if (mark == platform::LinkMark::Ember)
        return [](ImDrawList* draw, ImVec2 at, float size, ImU32 ink) { v2::BrandMark(draw, at, size, v2::color::Alpha(IM_COL32_WHITE, static_cast<float>(ink >> IM_COL32_A_SHIFT) / 255.f)); };
    const LineIcon icon = mark == platform::LinkMark::Discord ? LineIcon::Discord : mark == platform::LinkMark::Coffee ? LineIcon::Coffee : LineIcon::Twitter;
    return [icon](ImDrawList* draw, ImVec2 at, float size, ImU32 ink) { DrawLineIcon(draw, icon, at, size, ink); };
}
} }
