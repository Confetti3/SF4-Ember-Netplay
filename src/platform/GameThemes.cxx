#include "GameThemes.hxx"
#include "Utf8.hxx"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace sf4e { namespace platform { namespace game_themes {
namespace {
// A theme is one folder name: nothing that leaves ember\themes.
bool PlainName(const std::wstring& name) {
    return !name.empty() && name != L"." && name != L".." && name.find_first_of(L"\\/:") == std::wstring::npos;
}
}

std::vector<std::string> Names(const std::wstring& gameDirectory, const wchar_t* part) {
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it(fs::path(gameDirectory) / Folder, error), end; !error && it != end; it.increment(error)) {
        const std::wstring name = it->path().filename().wstring();
        if (PlainName(name) && fs::is_directory(it->path() / part, error)) names.push_back(WideToUtf8(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool Accept(const std::wstring& partFolder, std::string& reason, const Limits& limits) {
    unsigned files = 0;
    std::uintmax_t bytes = 0;
    std::error_code failed;
    for (fs::recursive_directory_iterator it(partFolder, failed), end; !failed && it != end; it.increment(failed)) {
        const fs::file_status status = it->symlink_status(failed);
        if (failed) break;
        if (status.type() == fs::file_type::directory) continue;
        if (status.type() != fs::file_type::regular) { reason = "has a link or other entry that is not a file"; return false; }
        const std::uintmax_t size = it->file_size(failed);
        if (failed) break;
        bytes += size;
        if (++files > limits.files) { reason = "more than " + std::to_string(limits.files) + " files"; return false; }
        if (bytes > limits.bytes) { reason = "more than " + std::to_string((limits.bytes + (std::uintmax_t(1) << 20) - 1) >> 20) + " MB"; return false; }
    }
    if (failed) { reason = failed.message(); return false; }
    return true;
}

Outcome Use(const std::wstring& gameDirectory, const wchar_t* part, const std::string& name, std::string& error, const Limits& limits) {
    error.clear();
    const fs::path game(gameDirectory), active = game / ActiveFolder / part;
    fs::path next = active, old = active;
    next += L".next";
    old += L".old";
    const std::wstring theme = Utf8ToWide(name.c_str());
    std::error_code ignored;
    // The part is put together beside the one in use and swapped in by
    // renames, so the game never reads a half-copied folder. A file it opens
    // between the two renames is the game's own, which is harmless.
    fs::remove_all(next, ignored);
    if (!name.empty()) {
        std::error_code copied;
        const fs::path source = game / Folder / theme / part;
        if (!PlainName(theme)) error = "not a folder name";
        else if (Accept(source.wstring(), error, limits)) {
            fs::create_directories(active.parent_path(), copied);
            if (!copied) fs::copy(source, next, fs::copy_options::recursive, copied);
            if (copied) error = copied.message();
        }
        // A part that cannot be used leaves the game's own files, never the theme before.
        if (!error.empty()) fs::remove_all(next, ignored);
    }
    fs::remove_all(old, ignored);
    std::error_code moved;
    if (fs::exists(active, ignored)) fs::rename(active, old, moved);
    if (moved) { error = moved.message(); fs::remove_all(next, ignored); return Outcome::Unchanged; }
    if (fs::exists(next, ignored)) fs::rename(next, active, moved);
    if (moved) { error = moved.message(); return Outcome::GameOwn; }
    fs::remove_all(old, ignored);
    return error.empty() ? Outcome::Used : Outcome::GameOwn;
}

bool WriteActive(const std::wstring& gameDirectory, const wchar_t* part, const std::string& file, const std::string& bytes, std::string& error) {
    error.clear();
    const fs::path relative = fs::u8path(file);
    bool plain = !file.empty() && file.find(':') == std::string::npos && relative.is_relative() && !relative.has_root_name();
    for (const auto& piece : relative) plain = plain && piece != ".." && piece != ".";
    if (!plain) { error = "not a plain file name"; return false; }
    const fs::path target = fs::path(gameDirectory) / ActiveFolder / part / relative;
    fs::path temp = target;
    temp += L".tmp";
    std::error_code failed;
    fs::create_directories(target.parent_path(), failed);
    {
        std::ofstream out(temp, std::ios::binary);
        if (!failed && out) out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (failed || !out) { error = failed ? failed.message() : "cannot write"; return false; }
    }
    fs::remove(target, failed);
    fs::rename(temp, target, failed);
    if (failed) { error = failed.message(); fs::remove(temp, failed); return false; }
    return true;
}
} } }
