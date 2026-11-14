#pragma once
#include <imgui.h>

// The watched game's picture for the menu (ShellView::watchedPicture), taken from the game's
// render target at the size asked. Drawing thread.
namespace sf4e { namespace overlay {
// The picture's texture, or an empty ImTextureID when the driver cannot make it (then not tried again until the overlay is made again).
ImTextureID WatchedPicture(unsigned width, unsigned height);
// Lets go of the textures with the overlay.
void ReleaseWatchedPicture();
} }
