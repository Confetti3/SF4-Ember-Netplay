#pragma once
#include <atomic>
#include <string>

namespace sf4e { namespace launcher {
// Extracts a release zip (stored or deflate entries, no zip64) under destDir
// with zlib. An entry name the installer would refuse fails the whole
// extraction, and `cancel` is read after every 64 KB. On failure, error says why.
bool ExtractZip(const std::wstring& zipPath, const std::wstring& destDir, std::string& error, const std::atomic<bool>& cancel);
} }
