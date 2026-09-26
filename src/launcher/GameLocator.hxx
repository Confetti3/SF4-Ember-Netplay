#pragma once

#include <string>
#include <vector>

namespace sf4e { namespace launcher {

// Library folders listed in the text of Steam's libraryfolders.vdf, which Steam
// writes as UTF-8. Both the nested format ("1" { "path" "D:\\Lib" }) and the
// older flat format ("1" "D:\\Lib") are read. Numbered keys come first in
// numeric order, then any other key in text order. Unreadable or malformed
// text yields an empty list.
std::vector<std::wstring> ParseLibraryFolders(const std::string& vdfUtf8);

// The Steam folder followed by every library in libraryfolders.vdf, with '/'
// turned into '\' and repeats removed. Two paths that differ only in letter
// case or a trailing separator count as the same folder.
std::vector<std::wstring> LibraryCandidates(const std::wstring& steamPath, const std::string& vdfUtf8);

} } // namespace sf4e::launcher
