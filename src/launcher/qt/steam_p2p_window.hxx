#pragma once



#include <QCheckBox>

#include <QFormLayout>

#include <QGroupBox>

#include <QLabel>

#include <QLineEdit>

#include <QListWidget>

#include <QMainWindow>

#include <QPlainTextEdit>

#include <QPushButton>

#include <QSpinBox>

#include <QTabWidget>

#include <QTimer>

#include <QVBoxLayout>

#include <QWidget>



#include <nlohmann/json.hpp>

#include <string>



#include "../netplay/netplay_launch_controller.hxx"

#include "controller_bridge.hxx"



namespace sf4e {

namespace launcher {



struct PendingInvite {

	std::string senderSteamId;

	int virtualPort = 7;

	std::string role;

	std::string sidecarHash;

	std::string buildGit;

	std::string sessionToken;

	bool valid = false;

};



class SteamP2pMainWindow : public QMainWindow {

	Q_OBJECT



public:

	explicit SteamP2pMainWindow(NetplayLaunchController& controller, QWidget* parent = nullptr);



protected:

	void closeEvent(QCloseEvent* event) override;



private slots:

	void onReply(const nlohmann::json& msg);

	void onLauncherFinished();

	void onRefreshStatus();

	void onRefreshFriends();

	void onPrepareHost();

	void onAcceptInvite();

	void onStartHost();

	void onStartJoin();

	void onStartOfflineTest();

	void onFriendSelectionChanged();

	void onFriendSearchChanged();

	void onClearSearch();

	void onToggleLog();

	void onClearLog();

	void onLaunchWaitTimeout();

	void onOnlySf4Changed();



private:

	void buildUi(bool logCollapsed);

	QWidget* buildHeader();

	QWidget* buildWarningBanner();

	QWidget* buildHostTab();

	QWidget* buildJoinTab();

	QWidget* buildOfflineTab();

	void buildLogPanel(QVBoxLayout* root, bool logCollapsed);

	void applyTheme();

	void wireSignals();



	void appendLog(const QString& text);

	void setStatus(const QString& text, const QString& kind = QString());

	void updateStartButtons();

	void updateConnectionLines();

	void applySteamStatus(const nlohmann::json& msg);

	void processMessages(const nlohmann::json& messages);

	void renderFriends();

	void renderInviteCard();

	void syncNamesFromState(const nlohmann::json& state);

	void handleMessage(const nlohmann::json& msg);

	QString peerSteamId() const;

	std::string expectedLaunchPeerSteamId() const;

	void updateLaunchWaitPoll();

	void armLaunchWaitTimer();

	void disarmLaunchWaitTimer();

	void recoverLaunchHandshake(const QString& reason, bool clearLocalReady);

	void resendLaunchReady();

	void startGame(const char* mode);

	void resetLaunchHandshake();

	void markLaunchReady(const char* mode);

	void trySynchronizedLaunch();

	void updateLaunchButtonLabels();

	void sendCancelIfNeeded();



	NetplayLaunchController& m_controller;

	ControllerBridge m_bridge;

	nlohmann::json m_state;

	nlohmann::json m_allFriends = nlohmann::json::array();

	struct SteamBuildInfo {

		std::string sidecarHash;

		std::string buildGit;

	} m_steamBuild;

	PendingInvite m_pendingInvite;

	bool m_p2pConnected = false;

	bool m_localLaunchReady = false;

	bool m_peerLaunchReady = false;

	bool m_launchTriggered = false;

	std::string m_launchRole;

	std::string m_launchSessionToken;

	QTimer m_launchWaitTimer;



	QLabel* m_versionLabel = nullptr;

	QLabel* m_statusStrip = nullptr;

	QLineEdit* m_hostName = nullptr;

	QSpinBox* m_hostDelay = nullptr;

	QSpinBox* m_virtualPort = nullptr;

	QLabel* m_hostBuildHint = nullptr;

	QLabel* m_hostConnection = nullptr;

	QLabel* m_hostStartHint = nullptr;

	QPushButton* m_btnPrepareHost = nullptr;

	QPushButton* m_btnStartHost = nullptr;

	QLineEdit* m_joinName = nullptr;

	QSpinBox* m_joinDelay = nullptr;

	QLineEdit* m_offlineName = nullptr;

	QSpinBox* m_offlineDelay = nullptr;

	QPushButton* m_btnStartOffline = nullptr;

	QLabel* m_offlineHint = nullptr;

	QLabel* m_inviteSummary = nullptr;

	QPushButton* m_btnAcceptInvite = nullptr;

	QLabel* m_joinConnection = nullptr;

	QLabel* m_joinStartHint = nullptr;

	QPushButton* m_btnStartJoin = nullptr;

	QLabel* m_steamInit = nullptr;

	QLabel* m_steamId = nullptr;

	QLabel* m_persona = nullptr;

	QLabel* m_p2pState = nullptr;

	QLabel* m_lastEvent = nullptr;

	QPushButton* m_btnRefreshStatus = nullptr;

	QPushButton* m_btnRefreshFriends = nullptr;

	QPushButton* m_btnClearSearch = nullptr;

	QPushButton* m_btnClearLog = nullptr;

	QLineEdit* m_friendSearch = nullptr;

	QCheckBox* m_onlySf4 = nullptr;

	QLabel* m_friendCount = nullptr;

	QListWidget* m_friendsList = nullptr;

	QLineEdit* m_peerSteamId = nullptr;

	QPlainTextEdit* m_log = nullptr;

	QWidget* m_logPanel = nullptr;

	QPushButton* m_btnToggleLog = nullptr;

};



} // namespace launcher

} // namespace sf4e

