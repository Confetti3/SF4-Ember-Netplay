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
		// Just before, the frame keeps whether the window is in front and
		// not minimized in the app's +4: the same call at 0x4042AE, and
		// `call dword ptr [IsIconic]` (slot 0x931378) at 0x4042D7. The sound
		// check asks IsIconic too, at 0x404321. Read from the code alone;
		// what else reads +4 is not known.
		static BYTE* activeFocusCheck;
		static BYTE* iconicChecks[2];
		static DWORD iconicImport;
	};
}
