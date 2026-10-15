#pragma once
#include <cstdint>
#include <functional>
#include "ProgressWindow.hxx"
#include "UpdaterArguments.hxx"

namespace sf4e { namespace launcher {
// What an install or a recovery needs from the process around it. Updater.exe
// passes the real ones; tests pass their own.
struct UpdaterHost {
    // True once the launcher with this process id has exited; 0 is none.
    std::function<bool(std::uint32_t pid)> waitForLauncher;
    // Starts the folder's launcher with these arguments (null for none);
    // false when it could not.
    std::function<bool(const wchar_t* installDir, const wchar_t* arguments)> startLauncher;
    std::function<void(const char* line)> log;
    // The window's options before the run adds its texts.
    ProgressWindow::Options window;
};
// Updater.exe's work for an install, or with -RecoverOnly for finishing one
// that stopped part way, under one window opened first: it waits for the
// launcher to close, does the work, and starts the launcher again
// (UpdaterReopensLauncher), on the Updates page when the work failed. Returns
// the process's exit code.
int RunUpdater(const UpdaterArguments& arguments, const UpdaterHost& host);
} }
