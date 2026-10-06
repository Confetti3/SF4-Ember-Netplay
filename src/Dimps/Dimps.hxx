#pragma once

#include <windows.h>

#include "Dimps__GameEvents.hxx"

namespace Dimps {
	extern char** characterCodes;
	extern char** characterNames;
	extern char** stageCodes;
	extern char** stageNames;

	void Locate(HMODULE peRoot);

	struct App {
		static void Locate(HMODULE peRoot);
		static GameEvents::RootEvent* (*GetRootEvent)();

		// The application's own handler for the window's messages (0x4033E0),
		// which the window procedure 0x77EB20 calls first. Its WM_KILLFOCUS
		// branch sets the master volume to 0; WM_SETFOCUS sets it back.
		typedef struct __publicMethods {
			unsigned int (App::* HandleMessage)(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
		} __publicMethods;
		static __publicMethods publicMethods;
		// The app's frame (0x4040A0) mutes the master volume on each frame
		// the window is behind another, and restores it in front. It asks at
		// this `call dword ptr [GetForegroundWindow]` (0x4042F8).
		static BYTE* soundFocusCheck;
		// The import slot of GetForegroundWindow that call reads (0x9312AC).
		static DWORD foregroundWindowImport;
	};
}
