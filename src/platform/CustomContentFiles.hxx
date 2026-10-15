#pragma once
#include "../common/InstalledContent.hxx"
#include <string>

namespace sf4e { namespace platform { namespace custom_content {
// Read complete custom installs from an explicit patch_ae2_tu3/battle folder.
// Missing folders and incomplete items yield no entries. Names are bounded,
// valid UTF-8. The returned catalog is immutable; startup owns caching it.
selection::InstalledContentPtr Scan(const std::wstring& battleDirectory);
} } }
