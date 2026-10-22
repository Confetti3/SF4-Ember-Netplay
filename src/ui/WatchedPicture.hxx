#pragma once
// The watched game's picture over the menu (ShellView::watchedPicture).
#include <imgui.h>

namespace sf4e { namespace ui {
namespace v2 { struct Box; }
// Draws it; in the V2 look the mouse can move it to another corner and resize
// it. True when that changed `spot` (PlayerPreferences::pictureSpot). A board
// that keeps a place for it (`dock`) takes it there from the top right
// corner, at the place's size; dragged to another corner it is small again.
bool DrawWatchedPicture(ImTextureID picture, int& spot, const v2::Box* dock = nullptr);
} }
