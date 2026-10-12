#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace sf4e { namespace launcher {
// Extracts a release zip (stored or deflate entries, no zip64) under destDir
// using zlib, for prefixes without tar.exe such as Wine and Proton. Entry
// names that leave destDir fail the whole extraction. progress(done, total)
// receives bytes written; returning false cancels. Returns an empty string on
// success, else the reason.
std::string ExtractZip(const std::wstring& zipPath, const std::wstring& destDir,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {});
} }
