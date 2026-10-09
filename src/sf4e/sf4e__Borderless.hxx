#pragma once
#include <windows.h>
namespace Dimps { namespace Platform { struct D3D; } }

namespace sf4e { namespace display {
// Optional launcher overrides. Native dimensions are configured before device creation;
// all HWND mutations are posted to the game's own window thread.
void Prepare(Dimps::Platform::D3D* graphics);
void RestoreNative(Dimps::Platform::D3D* graphics = nullptr);
void RequestApply(HWND gameWindow);
bool WindowMessage(HWND gameWindow, UINT message, WPARAM wParam, LPARAM lParam);
} }
