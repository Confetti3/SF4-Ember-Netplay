#pragma once
#include "Theme.hxx"
#include <functional>
#include <string>
namespace sf4e { namespace ui {
// Return true only for an explicit launch retry. Network work stays on the
// application worker; no game is created by this presentation surface.
// `artLog` receives one line per selection image that failed or needed a
// fallback, as SelectionArt reports it. `messageTone` is the severity of
// `message` (Error unless it only explains why the window opened). `canStart`
// adds a Start SF4 row to the updater, answered with true like Retry.
bool RunRecovery(std::string message, std::wstring& gameDirectory, bool updates = false,
    std::function<void(const std::string&)> artLog = {}, Tone messageTone = Tone::Error, bool canStart = false);
} }
