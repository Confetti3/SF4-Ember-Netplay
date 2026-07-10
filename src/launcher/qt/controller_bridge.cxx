#include "controller_bridge.hxx"

#include <QMetaType>

namespace sf4e {
namespace launcher {

ControllerBridgeWorker::ControllerBridgeWorker(NetplayLaunchController& controller)
	: m_controller(controller) {
}

void ControllerBridgeWorker::handle(const nlohmann::json& msg) {
	nlohmann::json reply;
	// HandleWebMessage performs network and JSON work; a thrown exception here
	// would unwind through the worker's event loop, which is undefined behavior.
	// Turn any failure into an error reply the UI can display instead of crashing.
	try {
		reply = m_controller.HandleWebMessage(msg);
	} catch (const std::exception& ex) {
		reply = { {"type", "error"}, {"message", ex.what()} };
	} catch (...) {
		reply = { {"type", "error"}, {"message", "The launcher hit an unexpected error."} };
	}
	if (!reply.is_null() && !reply.empty()) {
		emit replyReady(reply);
	}
	if (m_controller.ShouldExitForUpdate()) {
		emit exitForUpdate();
	}
	if (m_controller.IsFinished()) {
		emit launcherFinished();
	}
}

ControllerBridge::ControllerBridge(NetplayLaunchController& controller, QObject* parent)
	: QObject(parent)
	, m_controller(controller) {
	// Required so nlohmann::json can cross the thread boundary via queued signals.
	qRegisterMetaType<nlohmann::json>("nlohmann::json");

	m_worker = new ControllerBridgeWorker(controller);
	m_worker->moveToThread(&m_workerThread);
	connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

	// GUI -> worker (queued): run HandleWebMessage off the GUI thread.
	connect(this, &ControllerBridge::dispatch, m_worker, &ControllerBridgeWorker::handle);
	// worker -> GUI (queued): re-emit on the GUI thread so existing connections
	// to replyReceived / launcherFinished / exitForUpdate are unaffected.
	connect(m_worker, &ControllerBridgeWorker::replyReady, this, &ControllerBridge::replyReceived);
	connect(m_worker, &ControllerBridgeWorker::launcherFinished, this, &ControllerBridge::launcherFinished);
	connect(m_worker, &ControllerBridgeWorker::exitForUpdate, this, &ControllerBridge::exitForUpdate);

	m_workerThread.start();

	// The poll timer lives on the GUI thread but only enqueues work (cheap), so
	// it never blocks the GUI.
	connect(&m_pollTimer, &QTimer::timeout, this, &ControllerBridge::onPoll);
}

ControllerBridge::~ControllerBridge() {
	m_workerThread.quit();
	m_workerThread.wait();
}

void ControllerBridge::post(const nlohmann::json& msg) {
	nlohmann::json payload = msg;
	payload["v"] = kProtocolVersion;
	emit dispatch(payload);
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
