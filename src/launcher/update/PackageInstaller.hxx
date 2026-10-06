#pragma once
#include <string>
#include <filesystem>
namespace sf4e { namespace launcher {
// Present in the install folder while an update transaction is unfinished.
inline constexpr wchar_t UpdateTransactionName[] = L".ember-update-transaction-v1.json";
// A package folder answers to its own MANIFEST.txt: every file it names is
// present with that hash, nothing else is in it, and every path is one this
// build's inventory knows, current or obsolete, so an older package passes too.
bool ValidatePackageFolder(const std::filesystem::path& package, std::string& error);
// Lowercase hex SHA-256 of a file, as MANIFEST.txt writes it.
std::string Sha256Hex(const std::filesystem::path& file);
// Makes the folder's product files exactly the package's, whichever of the
// two is older: files the package has are replaced, product files it lacks
// are removed, each with a rollback copy and a journal entry. Files the
// inventory does not know are the player's and are never touched. Refuses
// reparse points in either tree.
bool InstallPackage(const std::filesystem::path& staging, const std::filesystem::path& install, std::string& error);
// Restores an interrupted transaction, or validates and clears a committed one.
bool RecoverPackage(const std::filesystem::path& install, std::string& error, bool inspectOnly = false);
// Removes every file the package inventory names or allows (so files an update
// added later and obsolete ones too), every file the installed MANIFEST.txt
// names (whatever version that is), the updater's own state and the folders
// that leaves empty. Anything else in the folder is the player's and stays.
// Reparse points are left alone, never followed. MANIFEST.txt goes last, and
// only when nothing failed, so a retry still knows what is ours.
bool UninstallPackage(const std::filesystem::path& install, std::string& error);
} }
