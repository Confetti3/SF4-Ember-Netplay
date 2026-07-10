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
	nlohmann::json reply;
	// HandleWebMessage performs network and JSON work; a thrown exception here
	// would unwind through the Qt event loop, which is undefined behavior. Turn
	// any failure into an error reply the UI can display instead of crashing.
	try {
		reply = m_controller.HandleWebMessage(payload);
	} catch (const std::exception& ex) {
		reply = { {"type", "error"}, {"message", ex.what()} };
	} catch (...) {
		reply = { {"type", "error"}, {"message", "The launcher hit an unexpected error."} };
	}
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
