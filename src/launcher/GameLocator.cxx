#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "GameLocator.hxx"
#include "../common/SteamKeyValues.hxx"
#include "../platform/Utf8.hxx"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace sf4e { namespace launcher {
namespace {

using steam::Token;

bool IsIndex(const std::string& key) {
    return !key.empty() && key.size() < 10 &&
        std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Numbered keys sort by value ahead of any other key, which sorts by text.
bool KeyBefore(const std::string& a, const std::string& b) {
    const bool indexA = IsIndex(a), indexB = IsIndex(b);
    if (indexA != indexB) return indexA;
    if (indexA && std::stoul(a) != std::stoul(b)) return std::stoul(a) < std::stoul(b);
    return a < b;
}

// Comparison form of a folder: backslashes only, no trailing separator.
std::wstring FolderKey(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    const std::wstring keyA = FolderKey(a), keyB = FolderKey(b);
    return CompareStringOrdinal(keyA.c_str(), static_cast<int>(keyA.size()),
        keyB.c_str(), static_cast<int>(keyB.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

std::vector<std::wstring> ParseLibraryFolders(const std::string& vdfUtf8) {
    // The file is one named root block. Depth 1 is inside the root, where the
    // flat format keeps numbered values, and depth 2 is inside one library of
    // the nested format, where its "path" lives. Deeper blocks such as "apps"
    // are only counted through.
    std::vector<Token> t;
    if (!steam::Tokenize(vdfUtf8, t) || t.size() < 2 || t[0].kind != Token::Text || t[1].kind != Token::Open) return {};
    using Entry = std::pair<std::string, std::string>;
    std::vector<Entry> entries;
    std::string library;
    std::size_t i = 2;
    for (int depth = 1; depth > 0;) {
        if (i >= t.size()) return {};
        if (t[i].kind == Token::Close) { --depth; ++i; continue; }
        if (t[i].kind != Token::Text || i + 1 >= t.size()) return {};
        const std::string& key = t[i].text;
        const Token& value = t[i + 1];
        i += 2;
        if (value.kind == Token::Open) {
            if (++depth == 2) library = key;
        } else if (value.kind != Token::Text) {
            return {};
        } else if (depth == 1 && IsIndex(key)) {
            entries.emplace_back(key, value.text);
        } else if (depth == 2 && key == "path") {
            entries.emplace_back(library, value.text);
        }
    }
    if (i != t.size()) return {};
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return KeyBefore(a.first, b.first); });

    std::vector<std::wstring> libraries;
    for (const auto& entry : entries) {
        std::wstring path = sf4e::platform::Utf8ToWide(entry.second.c_str());
        if (!path.empty()) libraries.push_back(std::move(path));
    }
    return libraries;
}

std::vector<std::wstring> LibraryCandidates(const std::wstring& steamPath, const std::string& vdfUtf8) {
    std::vector<std::wstring> candidates;
    auto add = [&](std::wstring folder) {
        std::replace(folder.begin(), folder.end(), L'/', L'\\');
        if (FolderKey(folder).empty()) return;
        for (const auto& existing : candidates)
            if (SameFolder(existing, folder)) return;
        candidates.push_back(std::move(folder));
    };
    add(steamPath);
    for (auto& library : ParseLibraryFolders(vdfUtf8)) add(std::move(library));
    return candidates;
}

std::wstring ManifestInstallDir(const std::string& acfUtf8) {
    // One root block, AppState; installdir and appid sit directly inside it.
    std::vector<Token> t;
    if (!steam::Tokenize(acfUtf8, t) || t.size() < 2 || t[0].kind != Token::Text ||
        CompareStringOrdinal(sf4e::platform::Utf8ToWide(t[0].text.c_str()).c_str(), -1, L"AppState", -1, TRUE) != CSTR_EQUAL ||
        t[1].kind != Token::Open) return {};
    std::string installDir, appId;
    std::size_t i = 2;
    for (int depth = 1; depth > 0;) {
        if (i >= t.size()) return {};
        if (t[i].kind == Token::Close) { --depth; ++i; continue; }
        if (t[i].kind != Token::Text || i + 1 >= t.size()) return {};
        const std::string& key = t[i].text;
        const Token& value = t[i + 1];
        i += 2;
        if (value.kind == Token::Open) { ++depth; continue; }
        if (value.kind != Token::Text) return {};
        if (depth == 1 && key == "installdir") installDir = value.text;
        if (depth == 1 && key == "appid") appId = value.text;
    }
    if (i != t.size() || (!appId.empty() && appId != "45760")) return {};
    // A folder name only: the manifest must not steer the search elsewhere.
    // A NUL would end the name early in the conversion below, so the name
    // checked here would not be the name used.
    if (installDir.find('\0') != std::string::npos) return {};
    std::wstring name = sf4e::platform::Utf8ToWide(installDir.c_str());
    if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos) return {};
    return name;
}

std::vector<std::wstring> GameFoldersInLibrary(const std::wstring& library, const std::string& manifestUtf8) {
    std::wstring common = library;
    std::replace(common.begin(), common.end(), L'/', L'\\');
    if (FolderKey(common).empty()) return {};
    common = FolderKey(common) + L"\\steamapps\\common\\";
    std::vector<std::wstring> folders;
    const std::wstring named = ManifestInstallDir(manifestUtf8);
    if (!named.empty()) folders.push_back(common + named);
    const std::wstring usual = common + L"Super Street Fighter IV - Arcade Edition";
    if (folders.empty() || !SameFolder(folders.front(), usual)) folders.push_back(usual);
    return folders;
}

std::wstring GameExecutable(const std::wstring& directory, const std::function<bool(const std::wstring&)>& exists) {
    if (directory.empty() || !exists) return {};
    std::wstring path = directory;
    if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += kGameExecutableName;
    return exists(path) ? path : std::wstring();
}

RememberedFolder::RememberedFolder(std::wstring persisted) : persisted_(std::move(persisted)) {}

const std::wstring& RememberedFolder::Persisted() const { return persisted_; }

bool RememberedFolder::Remember(const std::wstring& folder, const std::function<bool(const std::wstring&)>& save) {
    if (folder.empty() || SameFolder(folder, persisted_)) return true;
    if (!save || !save(folder)) return false;
    persisted_ = folder;
    return true;
}

GameLocation LocateGame(const std::wstring& chosenDirectory, const std::wstring& savedDirectory,
    const std::function<bool(const std::wstring&)>& exists,
    const std::function<std::wstring()>& search) {
    GameLocation location;
    location.fromRecovery = !chosenDirectory.empty();
    std::wstring directory = location.fromRecovery ? chosenDirectory : savedDirectory;
    location.executable = GameExecutable(directory, exists);
    if (location.executable.empty() && !location.fromRecovery && search) {
        directory = search();
        location.executable = GameExecutable(directory, exists);
    }
    if (!location.executable.empty()) location.directory = std::move(directory);
    return location;
}

std::vector<std::wstring> ShadowingRuntimeLibraries(const std::wstring& packageDirectory,
    const std::vector<std::wstring>& folders, const std::function<bool(const std::wstring&)>& exists) {
    std::vector<std::wstring> found;
    if (!exists) return found;
    for (const auto& folder : folders) {
        if (folder.empty()) continue;
        if (SameFolder(folder, packageDirectory)) break;
        std::wstring prefix = folder;
        if (prefix.back() != L'\\' && prefix.back() != L'/') prefix += L'\\';
        for (const wchar_t* library : kSidecarRuntimeLibraries) {
            std::wstring path = prefix + library;
            if (exists(path)) found.push_back(std::move(path));
        }
    }
    return found;
}

} } // namespace sf4e::launcher
