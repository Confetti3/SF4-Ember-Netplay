#pragma once
// The watched game's picture over the menu (ShellView::watchedPicture).
#include <imgui.h>

namespace sf4e { namespace ui {
// Draws it; in the V2 look the mouse can move it to another corner and resize
// it. True when that changed `spot` (PlayerPreferences::pictureSpot).
bool DrawWatchedPicture(ImTextureID picture, int& spot);
} }
