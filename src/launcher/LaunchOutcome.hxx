#pragma once
#include "../common/StartupHandshake.hxx"
#include "../common/CrashReport.hxx"

namespace sf4e { namespace launcher {
struct LaunchResult {
    startup::Result status;
    HANDLE process = nullptr; // Owned by the caller, also for EarlyExit.
    startup::StopResult stop{true, 0, WAIT_OBJECT_0, 0};
    DWORD processId = 0;
};
// The same message classification for an early exit and a played session.
struct GameExit {
    bool crashed, reportable;
    const char* message;
};
inline GameExit ClassifyGameExit(DWORD code, bool steamAbove, bool logsAvailable, bool early = false) {
    const bool crashed = crash::IsCrashExit(code);
    const bool reportable = crashed && code != 0xC0000139u && code != 0xC0E90002u && !steamAbove && logsAvailable;
    const char* message = code == 0xC0000139u ? "launcher.game_wrong_dll" :
        code == 0xC0E90002u ? "launcher.game_blocked_by_windows" :
        steamAbove ? "launcher.steam_runs_as_admin" :
        crashed ? (reportable ? "launcher.game_crashed" : "launcher.game_crashed_files") :
        early ? "launcher.start_failed" : "launcher.game_error";
    return {crashed, reportable, message};
}
inline const char* StartupMessage(const startup::Result& result) {
    using startup::Outcome;
    switch (result.outcome) {
    case Outcome::UnsupportedGame: return "launcher.unsupported_game";
    case Outcome::ValidationIo: return "launcher.validation_io";
    case Outcome::SidecarFailed: case Outcome::Incomplete: return "launcher.sidecar_failed";
    case Outcome::Timeout: return "launcher.start_timeout";
    case Outcome::CreateFailed:
        return result.error == ERROR_ELEVATION_REQUIRED ? "launcher.game_runs_as_admin" : "launcher.start_failed";
    default: return "launcher.start_failed";
    }
}
} }
