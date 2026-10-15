#include "UpdaterRun.hxx"
#include "../../platform/Utf8.hxx"
#include <cstdio>
#include <string>

namespace sf4e { namespace launcher {
namespace {
// How long each installer step took and over how many files, in the log:
// the record of where an update's time went.
struct StepLog {
    const UpdaterHost& host;
    int step = -1;
    ULONGLONG started = 0;
    std::uint64_t files = 0;
    void Heard(UpdateStage stage, std::uint64_t total) {
        const int heard = InstallerStepOf(stage);
        if (heard != step) { End(); step = heard; started = GetTickCount64(); }
        files = total;
    }
    void End() {
        if (step < 0) return;
        char line[160] = { 0 };
        snprintf(line, sizeof(line), "%s: %llu files in %llu ms", InstallerSteps[step].nameKey,
            static_cast<unsigned long long>(files), static_cast<unsigned long long>(GetTickCount64() - started));
        host.log(line);
        step = -1;
    }
};

bool InstallFiles(const UpdaterArguments& arguments, ProgressWindow& window, const UpdaterHost& host) {
    std::string error;
    StepLog steps{host};
    const bool installed = InstallPackage(arguments.stagingDir, arguments.installDir, error, NeverCancelled,
        [&](UpdateStage stage, std::uint64_t done, std::uint64_t total) {
            window.Report(stage, done, total);
            steps.Heard(stage, total);
        },
        [&](InstallActivity activity, const std::filesystem::path& file) { window.Hear(activity, file); });
    steps.End();
    if (!installed) host.log(error.c_str());
    return installed;
}

// No step counts a recovery: the window shows its phase (finishing the
// update while the folder is looked over, then restoring when that is
// needed, as the recovery tells), its clock and any file it waits for.
bool RecoverFiles(const UpdaterArguments& arguments, ProgressWindow& window, const UpdaterHost& host) {
    std::string error;
    const bool recovered = RecoverPackage(arguments.installDir, error, false,
        [&](InstallActivity activity, const std::filesystem::path& file) { window.Hear(activity, file); });
    host.log(recovered ? "Update recovery complete" : error.c_str());
    return recovered;
}
} // namespace

int RunUpdater(const UpdaterArguments& arguments, const UpdaterHost& host) {
    const wchar_t* installDir = arguments.installDir.c_str();
    host.log(("Updater start InstallDir=" + platform::WideToUtf8(arguments.installDir) +
        " StagingDir=" + platform::WideToUtf8(arguments.stagingDir) +
        " WaitPid=" + std::to_string(arguments.waitPid) +
        " RecoverOnly=" + (arguments.recoverOnly ? "1" : "0")).c_str());
    if (GetFileAttributesW(installDir) == INVALID_FILE_ATTRIBUTES) {
        host.log("ERROR: install directory not found");
        return 1;
    }
    if (!arguments.recoverOnly && GetFileAttributesW(arguments.stagingDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        host.log("ERROR: staging directory not found");
        return 1;
    }

    // The launcher passes the window's texts in the player's language; one
    // from before them does not, and the window shows their English.
    ProgressWindow::Options windowOptions = host.window;
    windowOptions.notices = NoticeTexts(arguments.notices);
    windowOptions.headline = UpdaterHeadline(arguments, windowOptions.notices);
    windowOptions.stages = arguments.stages;
    ProgressWindow window(L"SF4 Ember Netplay", std::move(windowOptions));
    window.Enter(UpdaterNotice::WaitingForLauncher);
    if (!host.waitForLauncher(arguments.waitPid)) {
        host.log("ERROR: launcher is still running; nothing was changed");
        window.Close();
        // An install's launcher shows the failure; a recovery's is the one
        // still running.
        if (!arguments.recoverOnly) host.startLauncher(installDir, L"--updates --update-error");
        return 1;
    }

    const bool done = arguments.recoverOnly ? RecoverFiles(arguments, window, host) : InstallFiles(arguments, window, host);
    window.Close();
    if (!UpdaterReopensLauncher(arguments)) return done ? 0 : 1;
    host.log(done ? "starting Launcher.exe" : "starting Launcher.exe to show the failure");
    const bool started = host.startLauncher(installDir, done ? nullptr : L"--updates --update-error");
    if (done && started) host.log("Updater complete");
    return done && started ? 0 : 1;
}
} }
