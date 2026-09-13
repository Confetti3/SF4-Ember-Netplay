#pragma once

#include <windows.h>
#include <d3d9.h>

#include "../session/sf4e__SessionClient.hxx"

namespace sf4e {
	namespace Overlay {
        bool CapturesMenuInput();
        bool HasInputFocus();
        void RequestMainControls();
		void InitializeOverlay(HWND hWnd, IDirect3DDevice9* lpDevice);
		void DrawOverlay();
		void FreeOverlay();
		void OnClientError(SessionClient::ErrorType errType, SessionClient* const client, const SessionClient::Callbacks& callbacks);
		void PushNetplayAlert(const char* msg);

		LRESULT WINAPI OverlayWindowFunc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	}
}
