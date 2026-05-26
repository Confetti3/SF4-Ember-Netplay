#pragma once

#include "../common/sf4e__NetplayConfig.hxx"

namespace sf4e {

	struct NetplayStatus {
		bool active = false;
		bool connected = false;
		bool inLobby = false;
		bool inMatch = false;
		int pingMs = -1;
		uint8_t inputDelay = 0;
		char opponentName[NETPLAY_DISPLAY_NAME_LEN] = { 0 };
		char lastError[256] = { 0 };
	};

	namespace NetplayFacade {
		void InitFromPayload(const NetplayConfig& cfg);
		const NetplayConfig& GetConfig();
		bool IsDevOverlayEnabled();
		bool IsRelayEnabled();
		bool IsAutoNetplayPending();
		bool IsPadCapturePhaseReady();
		bool IsPadCapturePending();
		bool TryCapturePadForSide(int side, uint8_t& outIdx, uint8_t& outType);
		void NotifyGameReady();
		void TickMainMenu();
		void TickFrame();
		NetplayStatus GetStatus();
		void SetLastError(const char* msg);
		void PushAlert(const char* msg);
		void ShutdownNetplay(bool closeGgpo);
		void ClearBattleState();
		bool ShouldDeferGgpoClose();
		void NotifyMatchEnded();
		void CheckGraphicsWarning();
	}

} // namespace sf4e
