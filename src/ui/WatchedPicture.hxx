#pragma once
// The watched game's picture over the menu (ShellView::watchedPicture).
#include <imgui.h>
#include <optional>

namespace sf4e { namespace ui {
// The size the picture is taken at this frame (ember::ShellGame::WatchedPicture):
// while a game is watched and its picture is not Off (PlayerPreferences::pictureMode),
// the largest share of `screen` it is shown at (WatchedPictureTaken), so any
// size it is drawn at stays sharp. None otherwise, or for an empty screen.
struct PictureSize { unsigned width = 0, height = 0; };
std::optional<PictureSize> WatchedPictureToTake(bool watching, int pictureMode, ImVec2 screen);

// Draws it; while `movable` (ShellView::CanEditRoomSettings) the mouse can
// move it to another corner and resize it. True when that changed `spot`
// (PlayerPreferences::pictureSpot).
bool DrawWatchedPicture(ImTextureID picture, int& spot, bool movable);
} }
