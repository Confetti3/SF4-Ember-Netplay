#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace launcher {

// The game executable inside its install folder.
inline constexpr wchar_t kGameExecutableName[] = L"SSFIV.exe";

// Library folders listed in the text of Steam's libraryfolders.vdf, which Steam
// writes as UTF-8. Both the nested format ("1" { "path" "D:\\Lib" }) and the
// older flat format ("1" "D:\\Lib") are read. Numbered keys come first in
// numeric order, then any other key in text order. Unreadable or malformed
// text, including #include and #base lines, yields an empty list.
std::vector<std::wstring> ParseLibraryFolders(const std::string& vdfUtf8);

// The Steam folder followed by every library in libraryfolders.vdf, with '/'
// turned into '\' and repeats removed. Two paths that differ only in letter
// case or a trailing separator count as the same folder.
std::vector<std::wstring> LibraryCandidates(const std::wstring& steamPath, const std::string& vdfUtf8);

// The full path of the game executable in directory when exists reports it
// there, otherwise empty. An empty directory never resolves.
std::wstring GameExecutable(const std::wstring& directory, const std::function<bool(const std::wstring&)>& exists);

// The game folder last written to settings. A folder needs saving when it is
// not empty and is not the persisted one under the same comparison the
// library candidates use. Update persisted only after a save succeeds.
struct RememberedFolder {
    std::wstring persisted;
    bool NeedsSave(const std::wstring& folder) const;
};

} } // namespace sf4e::launcher
