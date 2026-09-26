#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "GameLocator.hxx"
#include "../platform/Utf8.hxx"

#include <algorithm>
#include <system_error>
#include <utility>
#include <vdf_parser.hpp>

namespace sf4e { namespace launcher {
namespace {

// The vendored parser follows a null or past-the-end pointer on some
// malformed input instead of throwing, so the text is checked first. Steam
// writes only quoted keys and values, braces and whitespace, with every
// top-level entry a block, and anything else is refused.
bool WellFormed(const std::string& text) {
    int depth = 0;
    bool keyWaiting = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') continue;
        if (c == '"') {
            std::size_t end = i + 1;
            while (end < text.size() && text[end] != '"') end += text[end] == '\\' ? 2 : 1;
            if (end >= text.size() || (keyWaiting && depth == 0)) return false;
            keyWaiting = !keyWaiting;
            i = end;
        } else if (c == '{' && keyWaiting) {
            keyWaiting = false;
            ++depth;
        } else if (c == '}' && !keyWaiting && depth > 0) {
            --depth;
        } else {
            return false;
        }
    }
    return depth == 0 && !keyWaiting;
}

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

} // namespace

std::vector<std::wstring> ParseLibraryFolders(const std::string& vdfUtf8) {
    std::string text = vdfUtf8;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    if (!WellFormed(text)) return {};
    tyti::vdf::Options options;
    options.ignore_includes = true;
    std::error_code error;
    const tyti::vdf::object root = tyti::vdf::read(text.cbegin(), text.cend(), error, options);
    if (error) return {};

    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& child : root.childs) {
        if (!child.second) continue;
        const auto path = child.second->attribs.find("path");
        if (path != child.second->attribs.end()) entries.emplace_back(child.first, path->second);
    }
    // The flat format lists each library as a numbered value of the root,
    // beside fields such as TimeNextStatsReport and ContentStatsID.
    for (const auto& attrib : root.attribs)
        if (IsIndex(attrib.first)) entries.emplace_back(attrib.first, attrib.second);
    std::sort(entries.begin(), entries.end(),
        [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
            return KeyBefore(a.first, b.first);
        });

    std::vector<std::wstring> libraries;
    for (const auto& entry : entries) {
        std::wstring path = sf4e::platform::Utf8ToWide(entry.second.c_str());
        if (!path.empty()) libraries.push_back(std::move(path));
    }
    return libraries;
}

std::vector<std::wstring> LibraryCandidates(const std::wstring& steamPath, const std::string& vdfUtf8) {
    std::vector<std::wstring> candidates, keys;
    auto add = [&](std::wstring folder) {
        const std::wstring key = FolderKey(folder);
        if (key.empty()) return;
        for (const auto& existing : keys)
            if (CompareStringOrdinal(existing.c_str(), static_cast<int>(existing.size()),
                    key.c_str(), static_cast<int>(key.size()), TRUE) == CSTR_EQUAL) return;
        std::replace(folder.begin(), folder.end(), L'/', L'\\');
        keys.push_back(key);
        candidates.push_back(std::move(folder));
    };
    add(steamPath);
    for (auto& library : ParseLibraryFolders(vdfUtf8)) add(std::move(library));
    return candidates;
}

} } // namespace sf4e::launcher
