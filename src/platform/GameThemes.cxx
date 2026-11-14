#include "GameThemes.hxx"
#include "DurableFile.hxx"
#include "Utf8.hxx"
#include "../common/Utf8Text.hxx"
#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace sf4e { namespace platform { namespace game_themes {
bool PlainName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.size() < 100 && name.find_first_of("\\/:") == std::string::npos && IsWellFormedUtf8(name);
}

std::vector<std::string> Names(const std::wstring& gameDirectory, const wchar_t* part) {
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it(fs::path(gameDirectory) / Folder, error), end; !error && it != end; it.increment(error)) {
        const std::wstring name = it->path().filename().wstring();
        if (PlainName(WideToUtf8(name)) && fs::is_directory(it->path() / part, error)) names.push_back(WideToUtf8(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

Verdict Accept(const wchar_t* part, const std::wstring& partFolder, std::string& reason, const Rules& rules) {
    const Limits& limits = rules.limits;
    unsigned files = 0;
    std::uintmax_t bytes = 0;
    std::error_code failed;
    for (fs::recursive_directory_iterator it(partFolder, failed), end; !failed && it != end; it.increment(failed)) {
        const fs::file_status status = it->symlink_status(failed);
        if (failed) break;
        if (status.type() == fs::file_type::directory) continue;
        if (status.type() != fs::file_type::regular) { reason = "has a link or other entry that is not a file"; return Verdict::Refused; }
        const std::uintmax_t size = it->file_size(failed);
        if (failed) break;
        bytes += size;
        if (++files > limits.files) { reason = "more than " + std::to_string(limits.files) + " files"; return Verdict::Refused; }
        if (bytes > limits.bytes) { reason = "more than " + std::to_string((limits.bytes + (std::uintmax_t(1) << 20) - 1) >> 20) + " MB"; return Verdict::Refused; }
    }
    if (failed) { reason = failed.message(); return Verdict::Refused; }
    if (rules.battleHud && std::wstring(part) == BattleHud) {
        const Verdict checked = rules.battleHud(partFolder, reason);
        return checked == Verdict::PicturesMismatch && rules.pictureGuard ? Verdict::PicturesGuarded : checked;
    }
    return Verdict::Accepted;
}

Outcome Use(const std::wstring& gameDirectory, const wchar_t* part, const std::string& name, std::string& error, const Rules& rules) {
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
    Verdict verdict = Verdict::Accepted;
    std::string guarded;
    if (!name.empty()) {
        std::error_code copied;
        const fs::path source = game / Folder / theme / part;
        if (!PlainName(name)) error = "not a folder name";
        else {
            verdict = Accept(part, source.wstring(), error, rules);
            // A part used with pictures missing keeps its reason apart: no error stops the copy.
            if (verdict == Verdict::PicturesGuarded) guarded.swap(error);
            if (verdict == Verdict::Accepted || verdict == Verdict::PicturesGuarded) {
                fs::create_directories(active.parent_path(), copied);
                if (!copied) fs::copy(source, next, fs::copy_options::recursive, copied);
                if (copied) error = copied.message();
            }
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
    if (error.empty() && verdict == Verdict::PicturesGuarded) { error = guarded; return Outcome::UsedWithMissingPictures; }
    if (error.empty()) return Outcome::Used;
    return verdict == Verdict::PicturesMismatch ? Outcome::PicturesMismatch : verdict == Verdict::Unreadable ? Outcome::Unreadable : Outcome::GameOwn;
}

bool WriteActive(const std::wstring& gameDirectory, const wchar_t* part, const std::string& file, const std::string& bytes, std::string& error) {
    error.clear();
    const fs::path relative = fs::u8path(file);
    bool plain = !file.empty() && file.find(':') == std::string::npos && relative.is_relative() && !relative.has_root_name();
    for (const auto& piece : relative) plain = plain && piece != ".." && piece != ".";
    if (!plain) { error = "not a plain file name"; return false; }
    const fs::path target = fs::path(gameDirectory) / ActiveFolder / part / relative;
    std::error_code failed;
    fs::create_directories(target.parent_path(), failed);
    if (failed) { error = failed.message(); return false; }
    // Written whole beside it and swapped in, so the game never reads a half-written file.
    const durable::WriteResult written = durable::PublishReplace(target, durable::PartialPath(target), bytes.data(), bytes.size());
    if (!written) error = "cannot write (error " + std::to_string(written.error) + ")";
    return static_cast<bool>(written);
}
} } }
