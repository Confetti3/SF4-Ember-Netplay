#pragma once
#include <string>

namespace sf4e { namespace selection {

// Only the selector's data files may be added to an update package. Exact
// generated paths reject traversal, streams, executable extensions and suffixes.
// The files are the game's, so each game defines this once, as it defines
// Catalog() (GameCatalog.hxx): SF4's is Sf4SelectionAssetPath.cxx, and the
// runtime's tests have the fake game's (tests/fake_selection.cxx), which
// allows none.
bool IsSelectionAssetPath(const std::wstring& path);

} }
