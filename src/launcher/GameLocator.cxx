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

namespace {
std::wstring ManifestInstallDirectory(const std::string& text) {
    std::vector<Token> tokens;
    if (!steam::Tokenize(text, tokens) || tokens.size() < 2 || tokens[0].kind != Token::Text ||
        _stricmp(tokens[0].text.c_str(), "AppState") != 0 || tokens[1].kind != Token::Open) return {};
    int depth = 1;
    std::size_t i = 2;
    std::string appId, folder;
    bool haveId = false, haveFolder = false;
    while (depth && i < tokens.size()) {
        if (tokens[i].kind == Token::Close) { --depth; ++i; continue; }
        if (tokens[i].kind != Token::Text || i + 1 >= tokens.size()) return {};
        const auto& key = tokens[i++].text;
        const auto& value = tokens[i++];
        if (value.kind == Token::Open) { ++depth; continue; }
        if (value.kind != Token::Text) return {};
        if (depth != 1) continue;
        if (_stricmp(key.c_str(), "appid") == 0) {
            if (haveId) return {};
            haveId = true; appId = value.text;
        } else if (_stricmp(key.c_str(), "installdir") == 0) {
            if (haveFolder) return {};
            haveFolder = true; folder = value.text;
        }
    }
    // installdir is one directory below steamapps/common, not an arbitrary
    // path. Do not let damaged metadata select a different executable tree.
    if (depth || i != tokens.size() || appId != "45760" || folder.empty() ||
        folder == "." || folder == ".." || folder.back() == '.' || folder.back() == ' ' ||
        folder.find_first_of("\\/:*?\"<>|") != std::string::npos ||
        std::any_of(folder.begin(), folder.end(), [](unsigned char c) { return c < 32; })) return {};
    return platform::Utf8ToWide(folder.c_str());
}
}

std::wstring FindGameInLibraries(const std::vector<std::wstring>& libraries,
    const std::function<std::string(const std::wstring&)>& readText,
    const std::function<bool(const std::wstring&)>& exists) {
    if (!exists) return {};
    for (const auto& library : libraries) {
        if (library.empty()) continue;
        const auto prefix = library + (library.back() == L'\\' || library.back() == L'/' ? L"" : L"\\");
        const auto manifest = readText ? readText(prefix + L"steamapps\\appmanifest_45760.acf") : std::string();
        const auto folder = ManifestInstallDirectory(manifest);
        for (const auto& name : {folder, std::wstring(L"Super Street Fighter IV - Arcade Edition")}) {
            if (name.empty()) continue;
            const auto directory = prefix + L"steamapps\\common\\" + name;
            if (!GameExecutable(directory, exists).empty()) return directory;
        }
    }
    return {};
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
