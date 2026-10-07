#pragma once
#include <string>
#include <filesystem>
#include <cstdint>
#include <functional>
namespace sf4e { namespace launcher {
// Present in the install folder while an update transaction is unfinished.
inline constexpr wchar_t UpdateTransactionName[] = L".ember-update-transaction-v1.json";
// How far a pass over a package's files is. Returning false asks it to stop:
// a validation then fails, and an install ends like any other failed one.
using PackageProgress = std::function<bool(std::uint64_t done, std::uint64_t total)>;
// A package folder answers to its own MANIFEST.txt and PackageInventory.inc:
// every file the manifest names is present with that hash, nothing else is in
// it, its inventory's required files are among them and none of its obsolete
// ones, and every path is one this build's inventory knows, current or
// obsolete, so an older package passes too. `progress` hears how many of the
// manifest's files are checked.
bool ValidatePackageFolder(const std::filesystem::path& package, std::string& error,
    const PackageProgress& progress = {});
// Lowercase hex SHA-256 of a file, as MANIFEST.txt writes it.
std::string Sha256Hex(const std::filesystem::path& file);
// Makes the folder's product files exactly the package's, whichever of the
// two is older: files the package has are replaced, unless the folder's copy
// already is the package's; files the installed
// MANIFEST.txt names, or the obsolete list, that the package lacks are
// removed; each with a rollback copy and a journal entry. Anything else in
// the folder is the player's and is never touched. Refuses reparse points in
// either tree. `progress` hears how many of the file steps (check the package,
// compare, back up, replace, check the result) are done, of a total that does
// not change; a file left alone counts its back-up and replace steps as done.
bool InstallPackage(const std::filesystem::path& staging, const std::filesystem::path& install, std::string& error,
    const PackageProgress& progress = {});
// Restores an interrupted transaction, or validates and clears a committed one.
// A file the update left alone has no rollback copy: when one is gone, the
// rest is still restored, and the failure says the update must be installed
// again.
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
