#pragma once
namespace sf4e { namespace ui {
enum class LauncherRoot { Home, Recovery, Updates };
enum class LauncherPage { Root, Display };
// A missing optional Home surface must not prevent the game starting.
inline bool StartWithoutSurface(LauncherRoot root) { return root == LauncherRoot::Home; }
} }
