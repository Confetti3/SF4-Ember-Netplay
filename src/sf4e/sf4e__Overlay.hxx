#pragma once

#include <windows.h>
#include <d3d9.h>

#include "../session/sf4e__SessionClient.hxx"
#include "../common/TrainingPad.hxx"

namespace sf4e {
	namespace Overlay {
        bool CapturesMenuInput();
        bool HasInputFocus();
        // The last overlay frame drew Ember's menu or the training controls,
        // which a captioned export leaves out of its video (sf4e__Platform.cxx).
        bool ShellShown();
        void RequestMainControls();
        // Game thread: the pad's Back and Start in offline Training. Opening
        // takes the input at once, as RequestMainControls does.
        void PostTrainingPad(const input::TrainingPadEvents& events);
        bool TrainingControlsOpen();
		void InitializeOverlay(HWND hWnd, IDirect3DDevice9* lpDevice);
		void DrawOverlay();
		void FreeOverlay();
		void OnClientError(SessionClient::ErrorType errType, SessionClient* const client, const SessionClient::Callbacks& callbacks);
		void PushNetplayAlert(const char* msg);

		LRESULT WINAPI OverlayWindowFunc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	}
}
