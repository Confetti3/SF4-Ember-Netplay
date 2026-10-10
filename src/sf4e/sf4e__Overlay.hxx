#pragma once

#include <windows.h>
#include <d3d9.h>
#include <functional>

#include "../session/sf4e__SessionClient.hxx"
#include "../common/TrainingPad.hxx"

namespace sf4e {
	namespace Overlay {
        bool CapturesMenuInput();
        bool HasInputFocus();
        void RequestMainControls();
        // Game thread: the pad's Back and Start in offline Training. Opening
        // takes the input at once, as RequestMainControls does.
        void PostTrainingPad(const input::TrainingPadEvents& events);
        bool TrainingControlsOpen();
        // Whether a room is calling the player back from Training, and
        // whether its banner offers go now on the assigned Xbox pad.
        input::TrainingCall TrainingCallState();
		void InitializeOverlay(HWND hWnd, IDirect3DDevice9* lpDevice);
		// Draws the overlay over the game's picture. picture, which takes an
		// export's picture, runs once each call: after the layers an export's
		// video takes and before the rest of Ember (ui/OverlayLayers.hxx:
		// SplitExportPasses), or before anything when the overlay draws nothing.
		void DrawOverlay(const std::function<void()>& picture);
		void FreeOverlay();
		void OnClientError(SessionClient::ErrorType errType, SessionClient* const client, const SessionClient::Callbacks& callbacks);
		void PushNetplayAlert(const char* msg);

		LRESULT WINAPI OverlayWindowFunc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	}
}
