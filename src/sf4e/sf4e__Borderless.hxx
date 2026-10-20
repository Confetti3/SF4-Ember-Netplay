#pragma once
#include <windows.h>

#include "../common/GameDisplayConfig.hxx"

namespace Dimps { namespace Platform { struct D3D; struct Main; struct DisplayBlock; } }

// Borderless (Player > Borderless): the game renders at the largest 16:9 size
// of its monitor, in a window without a frame over that area, with black bars
// beside or above it on a monitor of another shape. A failure turns it off
// for the session; the next device build uses the player's own mode.
namespace sf4e { namespace borderless {
	// The player's choice, read once at startup like Faster display, so a
	// change applies from the next launch.
	void Choose(bool chosen);
	// Before the game builds its present parameters, at device creation and
	// every Reset; the window is then laid out on its own thread. Puts the
	// Borderless mode in the device's and Main's display blocks, or the
	// player's back once it is off. The game also writes config.ini from that
	// mode (WriteDisplayConfig), and calls both from its main thread.
	void BeforePresentParameters(Dimps::Platform::D3D* device, Dimps::Platform::Main* main, gameconfig::BorderlessOverride& mode);
	// A display block's mode, for the config write (DisplayConfigOverrides).
	gameconfig::DisplayMode ModeOf(const Dimps::Platform::DisplayBlock& block);
	void PutMode(const gameconfig::DisplayMode& mode, Dimps::Platform::DisplayBlock& block);
	// The game's window procedure asks first: true when the message was
	// Borderless's own and the game must not see it.
	bool WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
} }
