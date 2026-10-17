#pragma once
#include <functional>
#include <windows.h>

namespace Dimps { namespace Platform { struct D3D; struct Main; } }

// Borderless (Player > Borderless): the game renders at the largest 16:9 size
// of its monitor, in a window without a frame over that area, with black bars
// beside or above it on a monitor of another shape. A failure turns it off
// for the session; the next device build uses the player's own mode.
namespace sf4e { namespace borderless {
	// The player's choice, read once at startup like Faster display, so a
	// change applies from the next launch.
	void Choose(bool chosen);
	// Before the game builds its present parameters, at device creation and
	// every Reset; the window is then laid out on its own thread.
	void BeforePresentParameters(Dimps::Platform::D3D* device, Dimps::Platform::Main* main);
	// Runs the game's config write with the player's own display mode.
	void WriteConfig(Dimps::Platform::Main* main, const std::function<void()>& write);
	// The game's window procedure asks first: true when the message was
	// Borderless's own and the game must not see it.
	bool WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
} }
