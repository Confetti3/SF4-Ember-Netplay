#pragma once
#include <string>
#include <filesystem>
namespace sf4e { namespace launcher {
// Present in the install folder while an update transaction is unfinished.
inline constexpr wchar_t UpdateTransactionName[] = L".ember-update-transaction-v1.json";
// Installs allowlisted files with a rollback copy. Unknown user files and
// settings are never mirrored or deleted. Refuses reparse points in either tree.
bool InstallPackage(const std::filesystem::path& staging, const std::filesystem::path& install, std::string& error);
// Restores an interrupted transaction, or validates and clears a committed one.
bool RecoverPackage(const std::filesystem::path& install, std::string& error, bool inspectOnly = false);
} }
