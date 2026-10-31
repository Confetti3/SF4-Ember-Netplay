#pragma once
// The watched game's picture over the menu (ShellView::watchedPicture).
#include <imgui.h>

namespace sf4e { namespace ui {
// Draws it; while `movable` (ShellView::CanEditRoomSettings) the mouse can
// move it to another corner and resize it. True when that changed `spot`
// (PlayerPreferences::pictureSpot).
bool DrawWatchedPicture(ImTextureID picture, int& spot, bool movable);
} }
