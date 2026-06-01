#pragma once

#include <QObject>
#include <QTimer>

#include <nlohmann/json.hpp>

#include "../netplay/netplay_launch_controller.hxx"

namespace sf4e {
namespace launcher {

class ControllerBridge : public QObject {
	Q_OBJECT

public:
	static const int kProtocolVersion = 1;

	explicit ControllerBridge(NetplayLaunchController& controller, QObject* parent = nullptr);

	nlohmann::json post(const nlohmann::json& msg);
	NetplayLaunchController& controller() { return m_controller; }

	void startPoll(int intervalMs = 1000);
	void stopPoll();

signals:
	void replyReceived(const nlohmann::json& reply);
	void launcherFinished();
	void exitForUpdate();

private:
	void onPoll();

	NetplayLaunchController& m_controller;
	QTimer m_pollTimer;
};

} // namespace launcher
} // namespace sf4e
