#pragma once
#include "MenuRows.hxx"
#include "../platform/ApplicationServices.hxx"
#include <string>
namespace sf4e { namespace ui {
// The row that asks to open `link` in the browser, "link-<id>": Help & About lists them, Settings >
// Discord ends with the first, and Home's corner draws each as an icon. Every one is a confirmation
// and submits the same OpenLink request, so a link behaves alike wherever it is pressed. While the
// service reports on this link, its message takes the detail's place.
inline MenuEntry LinkRow(const platform::Link& link, const platform::ServiceSnapshot& services, std::string label = std::string()) {
    const bool reported = services.lastAction == platform::ServiceAction::OpenLink && services.lastLink == link.id && !services.message.empty();
    return ConfirmRow(std::string("link-") + link.id, label.empty() ? std::string(loc::T(link.label)) : std::move(label),
        reported ? services.message : loc::Tf(link.detail, link.address), !services.pending);
}
// What a row of LinkRow asks for: the link's id, or null for any other row.
inline const platform::Link* LinkOfRow(const std::string& rowId) {
    return rowId.compare(0, 5, "link-") == 0 ? platform::FindLink(rowId.substr(5)) : nullptr;
}
} }
