#include "netplay_wizard.hxx"

#include <string.h>

#include "../common/sf4e__NetUtil.hxx"
#include "netplay_launch_controller.hxx"
#include "connect_strategy.hxx"
#include "netplay_room_code.hxx"
#ifdef SF4E_LAUNCHER_QT_UI
#include "qt/qt_launcher_app.hxx"
#else
#include "webview/webview_host.hxx"
#endif

namespace sf4e {
namespace launcher {

	bool ApplyNetplayConfigFromWizard(
		NetplayConfig& outConfig,
		PersistedSettings& ioSettings,
		int mode,
		const char* joinRoomCodeOrNull
	) {
		memset(&outConfig, 0, sizeof(outConfig));
		outConfig.version = SF4E_NETPLAY_CONFIG_VERSION;
		outConfig.mode = mode;
		strncpy_s(outConfig.displayName, ioSettings.displayName, _TRUNCATE);
		outConfig.inputDelay = ioSettings.inputDelay;
		outConfig.sessionPort = ioSettings.sessionPort;
		outConfig.ggpoPort = ioSettings.ggpoPort;
		outConfig.editionSelect = ioSettings.editionSelect;
		outConfig.roundCount = ioSettings.roundCount;
		outConfig.roundTimeIntegral = ioSettings.roundTimeIntegral;
		outConfig.useRelay = ioSettings.useRelay;
		outConfig.deviceIdx = 0xff;
		outConfig.deviceType = 0xff;

		if (mode == (int)NetplayMode::Host) {
			char lanIp[64] = { 0 };
			sf4e::DetectLanIPv4(lanIp, sizeof(lanIp));
			strncpy_s(outConfig.sessionHost, lanIp, _TRUNCATE);
			strncpy_s(outConfig.roomKey, "1", _TRUNCATE);
			return true;
		}
		if (mode == (int)NetplayMode::Join) {
			const char* room = joinRoomCodeOrNull && joinRoomCodeOrNull[0]
				? joinRoomCodeOrNull
				: ioSettings.lastJoinHost;
			if (!room || !room[0]) {
				return false;
			}
			const char* envBroker = getenv("SF4E_BROKER_URL");
			if (envBroker && envBroker[0]) {
				strncpy_s(ioSettings.brokerBaseUrl, envBroker, _TRUNCATE);
			}
			if (IsShortRoomCode(room)) {
				JoinRequest req;
				req.roomCode = room;
				StrategyResult sr = ResolveJoinRelayRoom(req, ioSettings.brokerBaseUrl);
				if (!sr.ok) {
					return false;
				}
				strncpy_s(outConfig.sessionHost, sr.endpoint.host, _TRUNCATE);
				outConfig.sessionPort = sr.endpoint.port;
				strncpy_s(ioSettings.lastJoinHost, room, _TRUNCATE);
				return true;
			}
			if (!ParseRoomCode(room, outConfig.sessionHost, NETPLAY_SESSION_HOST_LEN, &outConfig.sessionPort)) {
				return false;
			}
			FormatRoomCode(outConfig.sessionHost, outConfig.sessionPort, ioSettings.lastJoinHost, sizeof(ioSettings.lastJoinHost));
			return true;
		}
		return true;
	}

	bool RunNetplayWizard(HWND parent, NetplayConfig& outConfig, PersistedSettings& ioSettings) {
		NetplayLaunchController controller(ioSettings, outConfig);
#ifdef SF4E_LAUNCHER_QT_UI
		(void)parent;
		if (!RunNetplayQtUi(controller)) {
#else
		if (!RunNetplayWebViewUi(parent, controller)) {
#endif
			return false;
		}
		return !controller.WasCancelled();
	}

} // namespace launcher
} // namespace sf4e
