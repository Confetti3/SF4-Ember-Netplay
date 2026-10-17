#pragma once
namespace sf4e { namespace ui {
enum class LauncherRoot { Home, Recovery, Updates };
enum class LauncherPage { Root, Display };
// Optional Home and automatic startup checks cannot prevent the game starting.
// Explicit recovery/update requests still fail closed when their UI is unavailable.
inline bool StartWithoutSurface(LauncherRoot root, bool startup = false) { return startup || root == LauncherRoot::Home; }
} }
