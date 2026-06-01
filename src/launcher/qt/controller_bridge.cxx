#include "controller_bridge.hxx"

namespace sf4e {
namespace launcher {

ControllerBridge::ControllerBridge(NetplayLaunchController& controller, QObject* parent)
	: QObject(parent)
	, m_controller(controller) {
	connect(&m_pollTimer, &QTimer::timeout, this, &ControllerBridge::onPoll);
}

nlohmann::json ControllerBridge::post(const nlohmann::json& msg) {
	nlohmann::json payload = msg;
	payload["v"] = kProtocolVersion;
	nlohmann::json reply = m_controller.HandleWebMessage(payload);
	if (!reply.is_null() && !reply.empty()) {
		emit replyReceived(reply);
	}
	if (m_controller.ShouldExitForUpdate()) {
		emit exitForUpdate();
	}
	if (m_controller.IsFinished()) {
		emit launcherFinished();
	}
	return reply;
}

void ControllerBridge::startPoll(int intervalMs) {
	m_pollTimer.start(intervalMs);
}

void ControllerBridge::stopPoll() {
	m_pollTimer.stop();
}

void ControllerBridge::onPoll() {
	post({ {"type", "steamPollMessages"} });
}

} // namespace launcher
} // namespace sf4e
