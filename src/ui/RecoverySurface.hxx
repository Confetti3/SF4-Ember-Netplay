#pragma once
#include <functional>
#include <string>
namespace sf4e { namespace ui {
// Return true only for an explicit launch retry. Network work stays on the
// application worker; no game is created by this presentation surface.
// `artLog` receives one line per selection image that failed or needed a
// fallback, as SelectionArt reports it.
bool RunRecovery(std::string message, std::wstring& gameDirectory, bool updates = false,
    std::function<void(const std::string&)> artLog = {});
} }
