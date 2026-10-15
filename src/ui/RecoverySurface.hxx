#pragma once
#include "Theme.hxx"
#include "../platform/ProblemReport.hxx"
#include <functional>
#include <optional>
#include <string>
namespace sf4e { namespace ui {
// Return true only for an explicit launch retry. Network work stays on the
// application worker; no game is created by this presentation surface.
// `artLog` receives one line per selection image that failed or needed a
// fallback, as SelectionArt reports it. `messageTone` is the severity of
// `message` (Error unless it only explains why the window opened). `canStart`
// adds a Start SF4 row to the updater, answered with true like Retry.
// `crash` is the crash that opened the window: with Send problem reports on
// its report goes at once, logs only, unless the day's automatic reports are
// used up; otherwise it is offered, and goes only from its preview or Always send.
bool RunRecovery(std::string message, std::wstring& gameDirectory, bool updates = false,
    std::function<void(const std::string&)> artLog = {}, Tone messageTone = Tone::Error, bool canStart = false,
    const std::optional<reports::CrashContext>& crash = std::nullopt);
} }
