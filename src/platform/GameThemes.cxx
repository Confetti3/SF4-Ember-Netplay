#include "GameThemes.hxx"
#include "Utf8.hxx"
#include <algorithm>
#include <filesystem>
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

bool Use(const std::wstring& gameDirectory, const wchar_t* part, const std::string& name, std::string& error) {
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
        if (!PlainName(theme)) error = "not a folder name";
        else {
            fs::create_directories(active.parent_path(), copied);
            if (!copied) fs::copy(game / Folder / theme / part, next, fs::copy_options::recursive, copied);
            if (copied) error = copied.message();
        }
        // A part that cannot be used leaves the game's own files, never the theme before.
        if (!error.empty()) fs::remove_all(next, ignored);
    }
    fs::remove_all(old, ignored);
    std::error_code moved;
    if (fs::exists(active, ignored)) fs::rename(active, old, moved);
    if (moved) { error = moved.message(); fs::remove_all(next, ignored); return false; }
    if (fs::exists(next, ignored)) fs::rename(next, active, moved);
    if (moved) { error = moved.message(); return false; }
    fs::remove_all(old, ignored);
    return error.empty();
}
} } }
