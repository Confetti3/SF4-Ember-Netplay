#pragma once
#include "MenuNavigation.hxx"
#include <vector>
namespace sf4e { namespace ui {
std::vector<MenuEntry> AccentRows(unsigned rgb, bool enabled);
// The existing ImGui picker runs only on the drawing thread. The shell owns
// the preference, edit permission and ordinary delayed save.
bool DrawAccentPicker(unsigned& rgb, bool enabled);
} }
