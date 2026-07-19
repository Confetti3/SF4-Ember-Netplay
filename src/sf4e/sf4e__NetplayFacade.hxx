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

	struct TransportDiagnostics {
		uint8_t ggpoTransport = 0;
		uint64_t relayOutboundFrames = 0;
		uint64_t relayOutboundBytes = 0;
		uint64_t relayInboundFrames = 0;
		uint64_t relayInboundBytes = 0;
		uint64_t tunnelSendCount = 0;
		uint64_t tunnelSendBytes = 0;
		uint64_t tunnelRecvCount = 0;
		uint64_t tunnelRecvBytes = 0;
	};

	// Effective GGPO path chosen at battle start (may differ from launcher plan on fallback).
	struct GgpoTransportStatus {
		uint8_t effectiveMode = 0;
		bool legacyTunnelActive = false;
		char remoteHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
		uint16_t remotePort = 0;
	};

	enum class GgpoSyncPhase : uint8_t {
		None = 0,
		Starting = 1,
		Connected = 2,
		Synchronizing = 3,
		Running = 4,
	};

	namespace NetplayFacade {
		void InitFromPayload(const NetplayConfig& cfg);
		const NetplayConfig& GetConfig();
		void ApplyGgpoTransportConfig(const NetplayConfig& cfg);
		void RestoreBrokerGgpoEndpoint(NetplayConfig& cfg);
		void ReportGgpoTransport(uint8_t effectiveMode, bool legacyTunnelActive, const char* remoteHost, uint16_t remotePort);
		GgpoTransportStatus GetGgpoTransportStatus();
		GgpoSyncPhase GetGgpoSyncPhase();
		void NotifyGgpoSyncPhase(GgpoSyncPhase phase);
		void ResetGgpoBattleWatch();
		void MarkGgpoBattleStarted();
		const char* GgpoTransportModeName(uint8_t mode);
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
		TransportDiagnostics GetTransportDiagnostics();
		void SetLastError(const char* msg);
		void PushAlert(const char* msg);
		void HandleNetplayFailure(const char* reason, bool closeGgpo);
		void ShutdownNetplay(bool closeGgpo);
		void ClearBattleState();
		void CancelDeferredGgpoClose();
		bool ShouldDeferGgpoClose();
		void NotifyMatchEnded();
		void CheckGraphicsWarning();
	}

} // namespace sf4e
