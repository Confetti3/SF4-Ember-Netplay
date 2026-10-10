#pragma once
#include <string>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <atomic>
namespace sf4e { namespace launcher {
// Present in the install folder while an update transaction is unfinished.
inline constexpr wchar_t UpdateTransactionName[] = L".ember-update-transaction-v1.json";
// The steps of an update, in order. The launcher downloads, verifies the
// download, extracts it and prepares (checks the package); the installer
// checks the package too, then compares, backs up, replaces and confirms.
enum class UpdateStage { Downloading, Verifying, Extracting, Preparing, Comparing, BackingUp, Replacing, Confirming };
// The installer's own steps, in order: the one table of them. Each has the
// locale key Updater.exe's window names it by, which the launcher passes in
// the player's language as -Stages (UpdaterArguments.hxx), and its share of
// that window's bar, in thousandths of it: the three passes that hash every
// file take most of the time, and between releases few files are backed up
// or replaced. Adding or reordering a step is a change to this table alone.
struct InstallerStep { UpdateStage stage; const char* nameKey; int from, to; };
inline constexpr InstallerStep InstallerSteps[] = {
    { UpdateStage::Preparing, "update.stage.preparing", 0, 300 },
    { UpdateStage::Comparing, "update.stage.comparing", 300, 600 },
    { UpdateStage::BackingUp, "update.stage.backing_up", 600, 700 },
    { UpdateStage::Replacing, "update.stage.replacing", 700, 800 },
    { UpdateStage::Confirming, "update.stage.confirming", 800, 1000 } };
inline constexpr std::size_t InstallerStepCount = sizeof(InstallerSteps) / sizeof(InstallerSteps[0]);
// The steps follow each other in UpdateStage's order and share the bar
// whole, each going only forward.
inline constexpr bool InstallerStepsInOrder() {
    for (std::size_t i = 0; i < InstallerStepCount; ++i) {
        if (InstallerSteps[i].from >= InstallerSteps[i].to) return false;
        if (i == 0 ? InstallerSteps[i].from != 0 : InstallerSteps[i].from != InstallerSteps[i - 1].to ||
            static_cast<int>(InstallerSteps[i].stage) <= static_cast<int>(InstallerSteps[i - 1].stage)) return false;
    }
    return InstallerSteps[InstallerStepCount - 1].to == 1000;
}
static_assert(InstallerStepsInOrder(), "installer steps out of order or not sharing the bar whole");
// stage's place in InstallerSteps, or -1 for a step that is not the installer's.
inline constexpr int InstallerStepOf(UpdateStage stage) {
    for (std::size_t i = 0; i < InstallerStepCount; ++i) if (InstallerSteps[i].stage == stage) return static_cast<int>(i);
    return -1;
}
// Where a bar of `range` stands at `done` of `total` in stage; 0 for any other stage.
inline int InstallerBarPosition(UpdateStage stage, std::uint64_t done, std::uint64_t total, int range) {
    const int step = InstallerStepOf(stage);
    if (step < 0) return 0;
    const auto& share = InstallerSteps[step];
    const int from = share.from * range / 1000, to = share.to * range / 1000;
    return from + (total ? static_cast<int>(static_cast<std::uint64_t>(to - from) * (done < total ? done : total) / total) : 0);
}
// How far a step is: bytes while downloading and verifying, files after. Each
// step counts from zero to its own total; a total of 0 is not known.
using Progress = std::function<void(UpdateStage stage, std::uint64_t done, std::uint64_t total)>;
// Every pass that takes a `cancel` stops soon after it is set. For a caller
// that cannot cancel.
inline const std::atomic<bool> NeverCancelled{false};
namespace testing {
// For tests only; Ember never sets it. Hears each block the package passes'
// worker threads hash: the file and the bytes of it hashed so far.
extern std::function<void(const std::filesystem::path& file, std::uint64_t hashed)> HashedBlock;
}
// A package folder answers to its own MANIFEST.txt and PackageInventory.inc:
// every file the manifest names is present with that hash, nothing else is in
// it, its inventory's required files are among them and none of its obsolete
// ones, and every path is one this build's inventory knows, current or
// obsolete, so an older package passes too. `progress` hears, as Preparing,
// how many of the manifest's files are checked; a cancel fails it.
bool ValidatePackageFolder(const std::filesystem::path& package, std::string& error,
    const std::atomic<bool>& cancel = NeverCancelled, const Progress& progress = {});
// Lowercase hex SHA-256 of a file, as MANIFEST.txt writes it. `progress`
// hears the bytes hashed, as Verifying. Throws when the file cannot be read
// or when cancelled.
std::string Sha256Hex(const std::filesystem::path& file, const std::atomic<bool>& cancel = NeverCancelled,
    const Progress& progress = {});
// Makes the folder's product files exactly the package's, whichever of the
// two is older: files the package has are replaced, unless the folder's copy
// already is the package's; files the installed
// MANIFEST.txt names, or the obsolete list, that the package lacks are
// removed; each with a rollback copy and a journal entry. Anything else in
// the folder is the player's and is never touched. Refuses reparse points in
// either tree. `progress` hears each step on its own: Preparing (the
// package's files checked), Comparing (the folder's copies), BackingUp,
// Replacing and Confirming (the result checked). A cancel ends it like any
// other failure: once files have changed, the prior ones are restored.
bool InstallPackage(const std::filesystem::path& staging, const std::filesystem::path& install, std::string& error,
    const std::atomic<bool>& cancel = NeverCancelled, const Progress& progress = {});
// Observes an interrupted transaction's full target inventory before clearing
// a finished update, restoring its operations or preserving a replaced folder.
// An unreadable file keeps the journal for retry. A missing skipped file has
// no backup: completed operations stay installed, partial ones are restored,
// and the failure asks for another install. A committed journal is only cleared.
// A rolling-back journal resumes restoration without reclassifying skipped edits.
bool RecoverPackage(const std::filesystem::path& install, std::string& error, bool inspectOnly = false);
// Removes every file the installed MANIFEST.txt names (whatever version that
// is), every file this build's inventory names, obsolete ones too, the
// updater's own state and the folders that leaves empty. Selection art counts
// only when the manifest names it. Anything else in the folder is the
// player's and stays.
// Reparse points are left alone, never followed. MANIFEST.txt goes last, and
// only when nothing failed, so a retry still knows what is ours.
bool UninstallPackage(const std::filesystem::path& install, std::string& error);
} }
