#pragma once
#include <windows.h>
#include "../common/DisplayConfigOverride.hxx"
namespace Dimps { namespace Platform { struct D3D; } }

namespace sf4e { namespace display {
// Optional launcher overrides. Native dimensions are configured before device creation;
// all HWND mutations are posted to the game's own window thread.
void Prepare(Dimps::Platform::D3D* graphics);
void RestoreNative(Dimps::Platform::D3D* graphics = nullptr);
bool NativeConfig(NativeDisplayValues& values);
void RequestApply(HWND gameWindow);
#ifdef SF4E_DISPLAY_TESTING
enum class WindowFault { None, Style, Position };
void FailNextWindowOperation(WindowFault fault);
#endif
bool WindowMessage(HWND gameWindow, UINT message, WPARAM wParam, LPARAM lParam);
} }
