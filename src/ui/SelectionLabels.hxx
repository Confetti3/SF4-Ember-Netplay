#pragma once
#include "../common/FighterCatalog.hxx"
#include <string>

namespace sf4e { namespace selection { struct InstalledContent; } }

namespace sf4e { namespace ui {
// The names every screen uses for a costume ("Original", "Alternate 1 / pack",
// a custom costume's installed name or "Custom 12") and an Ultra, so a saved
// choice reads the same as the card that made it.
std::string CostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed = nullptr);
const char* UltraLabel(int ultra);
// Home's short forms of the same: "Alt 1" (a pack costume by its pack's name
// alone, one costume each), "Alt 1, color 3" and "U1", "U2" or "Double".
std::string ShortCostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed = nullptr);
std::string ShortAppearanceLabel(const selection::Pick& pick, const selection::InstalledContent* installed = nullptr);
const char* ShortUltraLabel(int ultra);
// A stage's name (a custom stage's installed name, or "Custom C12"), or Random.
std::string StageLabel(int stageId, const selection::InstalledContent* installed = nullptr);
} }
