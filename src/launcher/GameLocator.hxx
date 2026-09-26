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

// The game folder last written to settings, and the one operation that
// changes it: a write attempt whose success is the only thing that moves
// persisted forward, so a failed write is retried next time.
class RememberedFolder {
public:
    explicit RememberedFolder(std::wstring persisted);
    const std::wstring& Persisted() const;
    // Writes folder through save when it differs from the persisted one, under
    // the same comparison the library candidates use. An empty folder is never
    // written. Returns false only when a write was attempted and failed.
    bool Remember(const std::wstring& folder, const std::function<bool(const std::wstring&)>& save);
private:
    std::wstring persisted_;
};

// Where the launcher found the game. executable and directory are empty when
// nothing resolved, and fromRecovery marks a folder picked in recovery.
struct GameLocation {
    std::wstring directory, executable;
    bool fromRecovery = false;
};

// The folder picked in recovery this run, else the saved folder while it
// still holds the game, else whatever search finds. search returns the
// found directory or empty. A recovery choice is never replaced by the
// search, and search runs at most once.
GameLocation LocateGame(const std::wstring& chosenDirectory, const std::wstring& savedDirectory,
    const std::function<bool(const std::wstring&)>& exists,
    const std::function<std::wstring()>& search);

// The libraries Sidecar.dll imports by name once it is inside the game. The
// game process resolves them from the game folder and the Windows system
// folders before the package folder, so a copy left there by another netplay
// mod or an older install is loaded instead of ours and the game stops with
// a missing entry point before Ember runs a line.
inline constexpr const wchar_t* kSidecarRuntimeLibraries[] = {L"GGPO.dll", L"spdlog.dll", L"fmt.dll", L"zlib1.dll"};

// Full paths of runtime library copies in folders that would be loaded ahead
// of the package's, in folder then library order. A folder that is the
// package folder itself, in any spelling, is not a shadow; empty folders are
// skipped.
std::vector<std::wstring> ShadowingRuntimeLibraries(const std::wstring& packageDirectory,
    const std::vector<std::wstring>& folders, const std::function<bool(const std::wstring&)>& exists);

} } // namespace sf4e::launcher
