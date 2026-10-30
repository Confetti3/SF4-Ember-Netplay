#pragma once
// The checks ui_render_test.cxx shares with its scenario fragments, defined
// there. FindWindow is a windows.h macro, so every file that names it must see
// windows.h first, or it names a function nothing defines.
#include <windows.h>
#include <imgui_internal.h>
// Throws `message` when `condition` is false; the test reports it and fails.
void Require(bool condition, const char* message);
// A rule V2 does not keep yet: reported at the end, and the run fails, but the
// checks after it still run.
void KnownV2Gap(bool condition, const char* message);
// The active window whose name contains `fragment`; throws when there is none.
ImGuiWindow* FindWindow(const char* fragment);
// Whether a window whose name contains `name` was drawn this frame.
bool WindowDrawn(const char* name);
