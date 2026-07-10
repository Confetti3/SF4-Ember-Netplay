#pragma once

#include <QObject>
#include <QThread>
#include <QTimer>

#include <nlohmann/json.hpp>

#include "../netplay/netplay_launch_controller.hxx"

namespace sf4e {
namespace launcher {

// Runs NetplayLaunchController::HandleWebMessage on a dedicated worker thread.
// HandleWebMessage performs blocking network and process I/O (broker calls,
// public-IP lookup, UPnP, update download); running it on the GUI thread froze
// the whole window. This object owns a single worker thread so all controller
// calls stay serialized (preserving message ordering) while the GUI thread
// stays responsive. Replies are delivered back on the GUI thread via
// replyReceived (a queued connection), so callers see no ordering change.
class ControllerBridgeWorker : public QObject {
	Q_OBJECT

public:
	explicit ControllerBridgeWorker(NetplayLaunchController& controller);

public slots:
	void handle(const nlohmann::json& msg);

signals:
	void replyReady(const nlohmann::json& reply);
	void launcherFinished();
	void exitForUpdate();

private:
	NetplayLaunchController& m_controller;
};

class ControllerBridge : public QObject {
	Q_OBJECT

public:
	static const int kProtocolVersion = 1;

	explicit ControllerBridge(NetplayLaunchController& controller, QObject* parent = nullptr);
	~ControllerBridge() override;

	// Enqueue a message for the worker thread. Returns immediately; the reply
	// arrives asynchronously via replyReceived. (No caller reads a return value.)
	void post(const nlohmann::json& msg);
	NetplayLaunchController& controller() { return m_controller; }

	void startPoll(int intervalMs = 1000);
	void stopPoll();

signals:
	void replyReceived(const nlohmann::json& reply);
	void launcherFinished();
	void exitForUpdate();

	// Internal: queued to the worker thread. Do not connect from outside.
	void dispatch(const nlohmann::json& msg);

private:
	void onPoll();

	NetplayLaunchController& m_controller;
	QThread m_workerThread;
	ControllerBridgeWorker* m_worker = nullptr;
	QTimer m_pollTimer;
};

} // namespace launcher
} // namespace sf4e
