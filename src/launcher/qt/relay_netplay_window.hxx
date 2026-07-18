#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

#include <nlohmann/json.hpp>

#include "../netplay/netplay_launch_controller.hxx"
#include "controller_bridge.hxx"

namespace sf4e {
namespace launcher {

class RelayNetplayWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit RelayNetplayWindow(NetplayLaunchController& controller, QWidget* parent = nullptr);

protected:
	void closeEvent(QCloseEvent* event) override;

private slots:
	void onReply(const nlohmann::json& msg);
	void onLauncherFinished();
	void onExitForUpdate();
	void onCreateRelayRoom();
	void onCopyShare();
	void onRefreshPublicIp();
	void onTryUpnp();
	void onStartHost();
	void onStartJoin();
	void onStartOffline();
	void onFindMatch();
	void onBrowseRooms();
	void onRefreshRooms();
	void onRoomSelected(QListWidgetItem* item);
	void onCheckUpdate();
	void onApplyUpdate();
	void onOpenRelease();
	void onRelayHeartbeat();
	void onToggleLog();
	void onClearLog();
	void onModeSimple();
	void onModeAdvanced();
	void onGoHome();
	void onGoHost();
	void onGoJoin();
	void onGoOffline();
	void onAdvertiseChanged();
	void onBrokerChanged();

private:
	enum class Screen { Home, Host, Join, Rooms, Offline };

	void buildUi();
	void applyTheme();
	void wireSignals();
	void showScreen(Screen screen);
	void applyUiMode(bool fromUser);
	void setAdvancedVisible(bool advanced);
	void appendLog(const QString& text);
	void setStatus(const QString& text, const QString& kind = QString());
	void showToast(const QString& text, const QString& kind = QString());
	void syncFromState(const nlohmann::json& state);
	void handleMessage(const nlohmann::json& msg);
	void applyPartialState(const nlohmann::json& msg);
	void finalizeRelayRoomResponse(const nlohmann::json& msg);
	void renderHostShareCards();
	void setShareCard(const QString& id, const QString& value);
	void copyShareValue(const QString& id);
	void updateHostControls();
	void updateJoinControls();
	void renderRoomList(const nlohmann::json& rooms, const QString& listError);
	void startRelayHeartbeat();
	void stopRelayHeartbeat();
	QString activeRelayRoomCode() const;
	QString getDisplayName(bool hostScreen) const;
	int getInputDelay(bool hostScreen) const;
	QString getHostConnectMethod() const;
	QString getJoinConnectMethod(const QString& trimmedCode) const;
	void startGameHost();
	void startGameJoin();
	void postPreviewRoomCode();

	NetplayLaunchController& m_controller;
	ControllerBridge m_bridge;
	nlohmann::json m_state;
	QString m_sessionRelayCode;
	QString m_shareRelay;
	QString m_shareLan;
	QString m_shareWan;
	bool m_simpleUi = true;
	bool m_forceVpsRelay = false;
	bool m_updateBusy = false;
	QString m_releaseUrl;
	QString m_zipDownloadUrl;
	QString m_updateLatestVersion;
	bool m_updateAvailable = false;

	QTimer m_heartbeatTimer;
	QTimer m_toastTimer;

	QLabel* m_versionLabel = nullptr;
	QLabel* m_subtitleLabel = nullptr;
	QLabel* m_warningBanner = nullptr;
	QLabel* m_statusStrip = nullptr;
	QLabel* m_toastLabel = nullptr;
	QStackedWidget* m_stack = nullptr;
	QPushButton* m_btnModeSimple = nullptr;
	QPushButton* m_btnModeAdvanced = nullptr;

	// Home
	QWidget* m_homeAdvancedPanel = nullptr;
	QPushButton* m_btnFindMatch = nullptr;
	QPushButton* m_btnBrowseRooms = nullptr;
	QLabel* m_updateStatus = nullptr;
	QPushButton* m_btnCheckUpdate = nullptr;
	QPushButton* m_btnInstallUpdate = nullptr;
	QPushButton* m_btnOpenRelease = nullptr;

	// Host
	QScrollArea* m_hostScroll = nullptr;
	QWidget* m_hostShareLan = nullptr;
	QWidget* m_hostShareWan = nullptr;
	QWidget* m_hostShareActions = nullptr;
	QPushButton* m_btnRefreshIp = nullptr;
	QLabel* m_shareRelayValue = nullptr;
	QLabel* m_shareLanValue = nullptr;
	QLabel* m_shareWanValue = nullptr;
	QPushButton* m_btnCopyRelay = nullptr;
	QPushButton* m_btnCopyLan = nullptr;
	QPushButton* m_btnCopyWan = nullptr;
	QPushButton* m_btnCreateRoom = nullptr;
	QPushButton* m_btnTryUpnp = nullptr;
	QLabel* m_hostShareHint = nullptr;
	QWidget* m_hostSimpleSettings = nullptr;
	QLineEdit* m_hostNameSimple = nullptr;
	QSpinBox* m_hostDelaySimple = nullptr;
	QWidget* m_hostAdvancedSettings = nullptr;
	QLineEdit* m_hostNameAdv = nullptr;
	QSpinBox* m_hostDelayAdv = nullptr;
	QComboBox* m_hostConnectMethod = nullptr;
	QSpinBox* m_hostPort = nullptr;
	QLineEdit* m_hostAdvertise = nullptr;
	QLabel* m_hostNatStatus = nullptr;
	QLineEdit* m_brokerUrl = nullptr;
	QPushButton* m_btnStartHost = nullptr;

	// Join
	QWidget* m_joinSimpleSettings = nullptr;
	QLineEdit* m_joinNameSimple = nullptr;
	QSpinBox* m_joinDelaySimple = nullptr;
	QLineEdit* m_joinRoomCode = nullptr;
	QWidget* m_joinAdvancedSettings = nullptr;
	QLineEdit* m_joinNameAdv = nullptr;
	QSpinBox* m_joinDelayAdv = nullptr;
	QComboBox* m_joinConnectMethod = nullptr;
	QLineEdit* m_joinAddress = nullptr;
	QPushButton* m_btnStartJoin = nullptr;

	// Rooms
	QListWidget* m_roomList = nullptr;
	QLabel* m_roomListEmpty = nullptr;

	// Offline
	QLineEdit* m_offlineName = nullptr;
	QSpinBox* m_offlineDelay = nullptr;

	QPlainTextEdit* m_log = nullptr;
	QWidget* m_logPanel = nullptr;
	QPushButton* m_btnToggleLog = nullptr;
};

} // namespace launcher
} // namespace sf4e
