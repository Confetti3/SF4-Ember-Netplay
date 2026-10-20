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
		// GetRootEvent takes the event system's singleton (0xA85690, built on
		// the first call of 0x405840, which bit 0 of 0xA856A0 records), asks
		// the object at its +4 (0xA85694) for the current event through that
		// object's virtual +0xA8, and returns the event when it is named
		// "Game". It reads the name before it checks the event, so it faults
		// while the game is starting and there is none. This takes the same
		// steps with the checks, building nothing: true once GetRootEvent can
		// be called.
		static bool RootEventReady();

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

		// The battle's step setting (0xAACA44, a float). Each frame the app
		// (0x402DC0) sets the battle's time scale from it: below 0 one frame,
		// 0 the last frame's measured length times 60 (at most 2), above 0
		// that many frames. config.ini FrameRate=SMOOTH leaves it 0; any
		// other FrameRate sets -1 (AppSettings_Read 0x402A50). The game's own
		// online battles set -1 at battle open (0x5DAD40), which saves the
		// value in System+0x58, and put it back at close (0x5D8470).
		static float* battleStepSetting;
		// The setter (0x6B2530, cdecl): stores the value and derives the
		// step and the frame limiter's period from it (0x6B2170).
		static void (__cdecl* SetBattleStepSetting)(float setting);
	};
}
