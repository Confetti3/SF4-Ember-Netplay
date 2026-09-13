#pragma once
#include <string>
#include <filesystem>
namespace sf4e { namespace launcher {
// Installs allowlisted files with a rollback copy. Unknown user files and
// settings are never mirrored or deleted. Refuses reparse points in either tree.
bool InstallPackage(const std::filesystem::path& staging, const std::filesystem::path& install, std::string& error);
} }
