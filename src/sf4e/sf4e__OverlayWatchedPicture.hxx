#pragma once
#include <imgui.h>

// The watched game's picture (ShellView::watchedPicture): the render target
// copied at its own size, which also resolves a multisampled one, then
// filtered down. Made at the first size seen and let go with the overlay,
// which a Reset (also a change of resolution) or the device going frees.
namespace sf4e { namespace overlay {
// Drawing thread: this frame's picture at width x height, or none when the device cannot make it.
ImTextureID WatchedPicture(unsigned width, unsigned height);
// With the overlay: lets the picture's textures go, and tries again with the next overlay.
void FreeWatchedPicture();
} }
