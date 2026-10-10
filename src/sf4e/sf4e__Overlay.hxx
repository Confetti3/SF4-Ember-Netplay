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
        // Game thread: what the pad's Back and Start ask for in offline
        // Training, in order (TrainingPad.hxx). An event posted under an epoch
        // that has gone is dropped: DropTrainingPad, on a change of pad or
        // context, and a change of focus end the epoch.
        void PostTrainingPad(const input::TrainingPadEvent& event);
        void DropTrainingPad();
        // The training controls' one controller: whether they are open, the
        // pad owner and the position events (TrainingPad.hxx). Whether Ember
        // takes the game's input follows it directly (CapturesMenuInput).
        input::TrainingControls& TrainingControls();
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
