#pragma once
#include <string>
#include <filesystem>
namespace sf4e { namespace launcher {
// Present in the install folder while an update transaction is unfinished.
inline constexpr wchar_t UpdateTransactionName[] = L".ember-update-transaction-v1.json";
// A package folder answers to its own MANIFEST.txt and PackageInventory.inc:
// every file the manifest names is present with that hash, nothing else is in
// it, its inventory's required files are among them and none of its obsolete
// ones, and every path is one this build's inventory knows, current or
// obsolete, so an older package passes too.
bool ValidatePackageFolder(const std::filesystem::path& package, std::string& error);
// Lowercase hex SHA-256 of a file, as MANIFEST.txt writes it.
std::string Sha256Hex(const std::filesystem::path& file);
// Makes the folder's product files exactly the package's, whichever of the
// two is older: files the package has are replaced; files the installed
// MANIFEST.txt names, or the obsolete list, that the package lacks are
// removed; each with a rollback copy and a journal entry. Anything else in
// the folder is the player's and is never touched. Refuses reparse points in
// either tree.
bool InstallPackage(const std::filesystem::path& staging, const std::filesystem::path& install, std::string& error);
// Restores an interrupted transaction, or validates and clears a committed one.
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
