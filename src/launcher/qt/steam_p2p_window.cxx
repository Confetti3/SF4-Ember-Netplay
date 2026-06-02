#include "steam_p2p_window.hxx"

#include "../../common/agent_debug_log.hxx"

#include <cstring>



#include <QAbstractSpinBox>

#include <QCloseEvent>

#include <QDateTime>

#include <QHBoxLayout>

#include <QSettings>



namespace sf4e {

namespace launcher {



namespace {



QString JsonString(const nlohmann::json& j, const char* key, const QString& fallback = QString()) {

	if (!j.contains(key) || j[key].is_null()) {

		return fallback;

	}

	if (j[key].is_string()) {

		return QString::fromStdString(j[key].get<std::string>());

	}

	if (j[key].is_number_unsigned() || j[key].is_number_integer()) {

		return QString::number(j[key].get<unsigned long long>());

	}

	return fallback;

}



bool JsonBool(const nlohmann::json& j, const char* key, bool fallback = false) {

	if (!j.contains(key) || j[key].is_null()) {

		return fallback;

	}

	return j[key].get<bool>();

}

unsigned long long ParseSteamId64(const std::string& text) {
	if (text.empty()) {
		return 0;
	}
	try {
		return std::stoull(text);
	}
	catch (...) {
		return 0;
	}
}

bool SteamIdsMatch(const std::string& a, const std::string& b) {
	const unsigned long long idA = ParseSteamId64(a);
	const unsigned long long idB = ParseSteamId64(b);
	if (idA != 0 && idB != 0) {
		return idA == idB;
	}
	return a == b;
}

QString LaunchReadySessionTokenFromMessage(const nlohmann::json& m) {
	if (m.contains("launchReady") && m["launchReady"].is_object()) {
		return JsonString(m["launchReady"], "sessionToken", QString());
	}
	return QString();
}

QString LaunchCommitSessionTokenFromMessage(const nlohmann::json& m) {
	if (m.contains("launchCommit") && m["launchCommit"].is_object()) {
		return JsonString(m["launchCommit"], "sessionToken", QString());
	}
	return QString();
}



QWidget* BuildStepper(QSpinBox* spinBox) {
	spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
	spinBox->setMinimumWidth(78);
	spinBox->setObjectName(QStringLiteral("stepperValue"));

	auto* stepper = new QWidget();
	stepper->setObjectName(QStringLiteral("stepperControl"));
	auto* row = new QHBoxLayout(stepper);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(4);

	auto* minus = new QPushButton(QStringLiteral("-"));
	minus->setObjectName(QStringLiteral("stepperButton"));
	minus->setFixedWidth(32);
	minus->setToolTip(QStringLiteral("Decrease"));
	auto* plus = new QPushButton(QStringLiteral("+"));
	plus->setObjectName(QStringLiteral("stepperButton"));
	plus->setFixedWidth(32);
	plus->setToolTip(QStringLiteral("Increase"));

	QObject::connect(minus, &QPushButton::clicked, spinBox, [spinBox]() { spinBox->stepDown(); });
	QObject::connect(plus, &QPushButton::clicked, spinBox, [spinBox]() { spinBox->stepUp(); });

	row->addWidget(spinBox);
	row->addWidget(minus);
	row->addWidget(plus);
	row->addStretch();
	return stepper;
}



} // namespace



SteamP2pMainWindow::SteamP2pMainWindow(NetplayLaunchController& controller, QWidget* parent)

	: QMainWindow(parent)

	, m_controller(controller)

	, m_bridge(controller, this) {

	setWindowTitle(QStringLiteral("SF4 Steam P2P Netplay"));

	resize(920, 720);



	QSettings settings(QStringLiteral("sf4e"), QStringLiteral("launcher"));

	const bool logCollapsed = settings.value(QStringLiteral("log-collapsed"), true).toBool();



	buildUi(logCollapsed);

	applyTheme();

	wireSignals();



	renderInviteCard();

	updateStartButtons();

	m_bridge.post({ {"type", "getState"} });

	m_bridge.startPoll(250);

	m_launchWaitTimer.setSingleShot(true);

	m_launchWaitTimer.setInterval(90000);

	connect(&m_launchWaitTimer, &QTimer::timeout, this, &SteamP2pMainWindow::onLaunchWaitTimeout);

}



void SteamP2pMainWindow::buildUi(bool logCollapsed) {

	auto* central = new QWidget(this);

	auto* root = new QVBoxLayout(central);

	root->setSpacing(10);

	root->setContentsMargins(12, 12, 12, 12);



	root->addWidget(buildHeader());

	root->addWidget(buildWarningBanner());



	m_statusStrip = new QLabel();

	m_statusStrip->setWordWrap(true);

	m_statusStrip->hide();

	root->addWidget(m_statusStrip);



	auto* tabs = new QTabWidget();

	tabs->addTab(buildHostTab(), QStringLiteral("Host"));

	tabs->addTab(buildJoinTab(), QStringLiteral("Join"));

	tabs->addTab(buildOfflineTab(), QStringLiteral("Offline Test"));

	root->addWidget(tabs, 1);



	buildLogPanel(root, logCollapsed);

	setCentralWidget(central);

}



QWidget* SteamP2pMainWindow::buildHeader() {

	auto* header = new QWidget();

	auto* row = new QHBoxLayout(header);

	row->setContentsMargins(0, 0, 0, 0);



	auto* titleCol = new QVBoxLayout();

	auto* eyebrow = new QLabel(QStringLiteral("Experimental branch"));

	eyebrow->setObjectName(QStringLiteral("eyebrow"));

	auto* title = new QLabel(QStringLiteral("Steam P2P Netplay"));

	title->setObjectName(QStringLiteral("title"));

	auto* subtitle = new QLabel(QStringLiteral(

		"Host or join via Steam friends and invites. Session and GGPO rollback use a Steam P2P tunnel (no VPS room code)."));

	subtitle->setWordWrap(true);

	subtitle->setObjectName(QStringLiteral("subtitle"));

	titleCol->addWidget(eyebrow);

	titleCol->addWidget(title);

	titleCol->addWidget(subtitle);



	m_versionLabel = new QLabel(QStringLiteral("build: unknown"));

	m_versionLabel->setObjectName(QStringLiteral("versionBadge"));

	m_versionLabel->setAlignment(Qt::AlignCenter);



	row->addLayout(titleCol, 1);

	row->addWidget(m_versionLabel, 0, Qt::AlignTop);

	return header;

}



QWidget* SteamP2pMainWindow::buildWarningBanner() {

	auto* warning = new QLabel(QStringLiteral(

		"<b>Steam experiment build.</b> Both players need the same launcher zip and Steam running. "
		"The <b>joiner must open this launcher on the Join tab</b> before the host sends an invite (Steam delivers invites only while the launcher is open). "
		"Matching Sidecar hash required. Use VPS relay on <code>main</code> if this path fails."));

	warning->setWordWrap(true);

	warning->setObjectName(QStringLiteral("warningBanner"));

	return warning;

}



QWidget* SteamP2pMainWindow::buildHostTab() {

	auto* hostPage = new QWidget();

	auto* hostLayout = new QHBoxLayout(hostPage);

	hostLayout->setSpacing(12);



	auto* hostSetup = new QGroupBox(QStringLiteral("Host setup"));

	auto* hostForm = new QFormLayout(hostSetup);

	hostForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_hostName = new QLineEdit();

	m_hostName->setPlaceholderText(QStringLiteral("Name shown in lobby"));

	m_hostDelay = new QSpinBox();

	m_hostDelay->setRange(1, 10);

	m_hostDelay->setValue(2);

	m_hostDelay->setToolTip(QStringLiteral("Frames of input delay (1–10)"));

	m_virtualPort = new QSpinBox();

	m_virtualPort->setRange(0, 999);

	m_virtualPort->setValue(7);

	m_virtualPort->setToolTip(QStringLiteral("Steam virtual port for this session (must match invite)"));

	m_hostBuildHint = new QLabel(QStringLiteral("Both players must use the same build."));

	m_hostBuildHint->setWordWrap(true);

	m_hostBuildHint->setObjectName(QStringLiteral("hint"));

	hostForm->addRow(QStringLiteral("Display name"), m_hostName);

	hostForm->addRow(QStringLiteral("Input delay"), BuildStepper(m_hostDelay));

	hostForm->addRow(QStringLiteral("Virtual port"), BuildStepper(m_virtualPort));

	hostForm->addRow(m_hostBuildHint);



	m_btnPrepareHost = new QPushButton(QStringLiteral("Send invite + listen"));

	m_btnPrepareHost->setObjectName(QStringLiteral("primaryButton"));

	m_btnPrepareHost->setToolTip(QStringLiteral("Sends a Steam invite to the selected friend and starts listening for P2P"));

	hostForm->addRow(m_btnPrepareHost);



	m_hostConnection = new QLabel(QStringLiteral("P2P: not connected"));

	m_hostConnection->setObjectName(QStringLiteral("connectionLine"));

	hostForm->addRow(m_hostConnection);



	m_btnStartHost = new QPushButton(QStringLiteral("Ready to launch game"));

	m_btnStartHost->setEnabled(false);

	m_btnStartHost->setObjectName(QStringLiteral("primaryButton"));

	hostForm->addRow(m_btnStartHost);

	m_hostStartHint = new QLabel(QStringLiteral("Select a friend, send invite, then wait for P2P connected."));

	m_hostStartHint->setWordWrap(true);

	m_hostStartHint->setObjectName(QStringLiteral("hint"));

	hostForm->addRow(m_hostStartHint);



	auto* friendsBox = new QGroupBox(QStringLiteral("Steam Friends"));

	auto* friendsLayout = new QVBoxLayout(friendsBox);

	auto* friendsToolbar = new QHBoxLayout();

	m_btnRefreshFriends = new QPushButton(QStringLiteral("Refresh"));

	m_onlySf4 = new QCheckBox(QStringLiteral("Only USF4"));

	m_onlySf4->setToolTip(QStringLiteral("Show only friends currently in Ultra Street Fighter IV"));

	friendsToolbar->addWidget(m_btnRefreshFriends);

	friendsToolbar->addWidget(m_onlySf4);

	friendsToolbar->addStretch();

	friendsLayout->addLayout(friendsToolbar);

	m_friendSearch = new QLineEdit();

	m_friendSearch->setPlaceholderText(QStringLiteral("Filter by name or SteamID64…"));

	auto* searchRow = new QHBoxLayout();

	searchRow->addWidget(m_friendSearch, 1);

	m_btnClearSearch = new QPushButton(QStringLiteral("Clear"));

	searchRow->addWidget(m_btnClearSearch);

	friendsLayout->addLayout(searchRow);

	m_friendCount = new QLabel(QStringLiteral("0 / 0 friends"));

	m_friendCount->setObjectName(QStringLiteral("hint"));

	friendsLayout->addWidget(m_friendCount);

	m_friendsList = new QListWidget();

	m_friendsList->setMinimumHeight(200);

	m_friendsList->setAlternatingRowColors(true);

	friendsLayout->addWidget(m_friendsList, 1);

	auto* peerLabel = new QLabel(QStringLiteral("Peer SteamID64 (from selection)"));

	peerLabel->setObjectName(QStringLiteral("hint"));

	friendsLayout->addWidget(peerLabel);

	m_peerSteamId = new QLineEdit();

	m_peerSteamId->setPlaceholderText(QStringLiteral("7656119…"));

	m_peerSteamId->setReadOnly(false);

	friendsLayout->addWidget(m_peerSteamId);



	hostLayout->addWidget(hostSetup, 1);

	hostLayout->addWidget(friendsBox, 1);

	return hostPage;

}



QWidget* SteamP2pMainWindow::buildJoinTab() {

	auto* joinPage = new QWidget();

	auto* joinLayout = new QHBoxLayout(joinPage);

	joinLayout->setSpacing(12);



	auto* joinSetup = new QGroupBox(QStringLiteral("Join setup"));

	auto* joinForm = new QFormLayout(joinSetup);

	m_joinName = new QLineEdit();

	m_joinName->setPlaceholderText(QStringLiteral("Name shown in lobby"));

	m_joinDelay = new QSpinBox();

	m_joinDelay->setRange(1, 10);

	m_joinDelay->setValue(2);

	m_inviteSummary = new QLabel(QStringLiteral("No invite yet — ask the host to send one, or watch the activity log."));

	m_inviteSummary->setWordWrap(true);

	m_inviteSummary->setObjectName(QStringLiteral("inviteCard"));

	m_btnAcceptInvite = new QPushButton(QStringLiteral("Accept invite + connect"));

	m_btnAcceptInvite->setEnabled(false);

	m_btnAcceptInvite->setObjectName(QStringLiteral("primaryButton"));

	m_btnAcceptInvite->setToolTip(QStringLiteral("Connects to the host after you receive their Steam invite"));

	joinForm->addRow(QStringLiteral("Display name"), m_joinName);

	joinForm->addRow(QStringLiteral("Input delay"), BuildStepper(m_joinDelay));

	joinForm->addRow(QStringLiteral("Invite"), m_inviteSummary);

	joinForm->addRow(m_btnAcceptInvite);

	m_joinConnection = new QLabel(QStringLiteral("P2P: waiting for invite"));

	m_joinConnection->setObjectName(QStringLiteral("connectionLine"));

	joinForm->addRow(m_joinConnection);

	m_btnStartJoin = new QPushButton(QStringLiteral("Ready to launch game"));

	m_btnStartJoin->setEnabled(false);

	m_btnStartJoin->setObjectName(QStringLiteral("primaryButton"));

	joinForm->addRow(m_btnStartJoin);

	m_joinStartHint = new QLabel(QStringLiteral("Accept the invite and wait for P2P connected before starting."));

	m_joinStartHint->setWordWrap(true);

	m_joinStartHint->setObjectName(QStringLiteral("hint"));

	joinForm->addRow(m_joinStartHint);



	auto* statusBox = new QGroupBox(QStringLiteral("Steam status"));

	auto* statusForm = new QFormLayout(statusBox);

	m_steamInit = new QLabel(QStringLiteral("—"));

	m_steamId = new QLabel(QStringLiteral("—"));

	m_steamId->setTextInteractionFlags(Qt::TextSelectableByMouse);

	m_persona = new QLabel(QStringLiteral("—"));

	m_p2pState = new QLabel(QStringLiteral("—"));

	statusForm->addRow(QStringLiteral("Steam"), m_steamInit);

	statusForm->addRow(QStringLiteral("SteamID"), m_steamId);

	statusForm->addRow(QStringLiteral("Persona"), m_persona);

	statusForm->addRow(QStringLiteral("P2P"), m_p2pState);

	m_lastEvent = new QLabel(QStringLiteral("No events yet."));

	m_lastEvent->setWordWrap(true);

	m_lastEvent->setObjectName(QStringLiteral("hint"));

	statusForm->addRow(QStringLiteral("Last event"), m_lastEvent);

	m_btnRefreshStatus = new QPushButton(QStringLiteral("Refresh status"));

	statusForm->addRow(m_btnRefreshStatus);



	joinLayout->addWidget(joinSetup, 1);

	joinLayout->addWidget(statusBox, 1);

	return joinPage;

}



QWidget* SteamP2pMainWindow::buildOfflineTab() {

	auto* page = new QWidget();

	auto* layout = new QHBoxLayout(page);

	layout->setSpacing(12);



	auto* setup = new QGroupBox(QStringLiteral("Offline tester launch"));

	auto* form = new QFormLayout(setup);

	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_offlineName = new QLineEdit();

	m_offlineName->setPlaceholderText(QStringLiteral("Tester display name"));

	m_offlineDelay = new QSpinBox();

	m_offlineDelay->setRange(1, 10);

	m_offlineDelay->setValue(2);

	form->addRow(QStringLiteral("Display name"), m_offlineName);

	form->addRow(QStringLiteral("Input delay"), BuildStepper(m_offlineDelay));

	m_btnStartOffline = new QPushButton(QStringLiteral("Launch offline test"));

	m_btnStartOffline->setObjectName(QStringLiteral("primaryButton"));

	m_btnStartOffline->setToolTip(QStringLiteral("Launches USF4 with Sidecar and the test overlay, but no Steam P2P session."));

	form->addRow(m_btnStartOffline);

	m_offlineHint = new QLabel(QStringLiteral(
		"Use this before group testing to verify the game launches, Sidecar injects, the overlay appears, "
		"and logs are written. Steam P2P and GGPO remain inactive in this mode."));

	m_offlineHint->setWordWrap(true);

	m_offlineHint->setObjectName(QStringLiteral("hint"));

	form->addRow(m_offlineHint);



	auto* checklist = new QGroupBox(QStringLiteral("Tester checklist"));

	auto* checklistLayout = new QVBoxLayout(checklist);

	auto* text = new QLabel(QStringLiteral(
		"1. Click Launch offline test.\n"
		"2. Confirm USF4 opens.\n"
		"3. Confirm the sf4e overlay shows Offline Test.\n"
		"4. Check %APPDATA%\\sf4e\\logs\\sf4e.log if anything fails."));

	text->setWordWrap(true);

	text->setObjectName(QStringLiteral("inviteCard"));

	checklistLayout->addWidget(text);

	checklistLayout->addStretch();



	layout->addWidget(setup, 1);

	layout->addWidget(checklist, 1);

	return page;

}



void SteamP2pMainWindow::buildLogPanel(QVBoxLayout* root, bool logCollapsed) {

	m_logPanel = new QWidget();

	auto* logLayout = new QVBoxLayout(m_logPanel);

	logLayout->setContentsMargins(0, 0, 0, 0);

	auto* logHead = new QHBoxLayout();

	m_btnToggleLog = new QPushButton(logCollapsed ? QStringLiteral("Show activity log") : QStringLiteral("Hide activity log"));

	m_btnClearLog = new QPushButton(QStringLiteral("Clear log"));

	logHead->addWidget(m_btnToggleLog);

	logHead->addStretch();

	logHead->addWidget(m_btnClearLog);

	logLayout->addLayout(logHead);

	m_log = new QPlainTextEdit();

	m_log->setReadOnly(true);

	m_log->setMaximumBlockCount(500);

	m_log->setPlaceholderText(QStringLiteral("Steam events, invites, and errors appear here…"));

	m_log->setVisible(!logCollapsed);

	logLayout->addWidget(m_log);

	root->addWidget(m_logPanel);

}



void SteamP2pMainWindow::applyTheme() {

	setStyleSheet(QStringLiteral(

		"QMainWindow, QWidget {"
		"  background: #050606;"
		"  color: #d6d9dc;"
		"  font-family: 'Segoe UI Variable Text', 'Segoe UI', Arial, sans-serif;"
		"  font-size: 13px;"
		"}"
		"QWidget#centralWidget { background: #050606; }"
		"QLabel { color: #d6d9dc; }"
		"QLabel#eyebrow { color: #929aa2; font-size: 11px; font-weight: 600; letter-spacing: 1px; text-transform: uppercase; }"
		"QLabel#title { color: #f5f7f8; font-size: 25px; font-weight: 700; }"
		"QLabel#subtitle { color: #a9afb5; }"
		"QLabel#versionBadge {"
		"  background: #0c0f10;"
		"  color: #aeb5bb;"
		"  border: 1px solid #22272a;"
		"  padding: 6px 12px;"
		"  border-radius: 3px;"
		"}"
		"QLabel#warningBanner {"
		"  background: #121006;"
		"  color: #d8c27a;"
		"  border: 1px solid #302811;"
		"  padding: 10px;"
		"  border-radius: 3px;"
		"}"
		"QLabel#hint { color: #7c838a; font-size: 12px; }"
		"QLabel#connectionLine { color: #d7dce0; font-weight: 700; }"
		"QLabel#inviteCard {"
		"  background: #090b0c;"
		"  color: #d7dce0;"
		"  border: 1px solid #22272a;"
		"  border-left: 3px solid #d14a4a;"
		"  padding: 8px;"
		"  border-radius: 2px;"
		"}"
		"QGroupBox {"
		"  background: #080909;"
		"  color: #cdd2d6;"
		"  font-weight: 700;"
		"  border: 1px solid #22272a;"
		"  border-radius: 2px;"
		"  margin-top: 12px;"
		"  padding: 12px 8px 8px 8px;"
		"}"
		"QGroupBox::title {"
		"  subcontrol-origin: margin;"
		"  left: 8px;"
		"  padding: 0 4px;"
		"  color: #c6cbd0;"
		"  background: #050606;"
		"}"
		"QLineEdit, QSpinBox, QListWidget, QPlainTextEdit {"
		"  background: #070808;"
		"  color: #d6d9dc;"
		"  border: 1px solid #24292c;"
		"  border-radius: 2px;"
		"  padding: 6px;"
		"  selection-background-color: #8e2f32;"
		"  selection-color: #fff3f3;"
		"}"
		"QLineEdit:focus, QSpinBox:focus, QListWidget:focus, QPlainTextEdit:focus {"
		"  border: 1px solid #d14a4a;"
		"}"
		"QLineEdit::placeholder { color: #555d63; }"
		"QWidget#stepperControl {"
		"  background: transparent;"
		"}"
		"QSpinBox#stepperValue {"
		"  min-width: 70px;"
		"  padding-right: 6px;"
		"}"
		"QPushButton#stepperButton {"
		"  background: #252b2e;"
		"  color: #ffffff;"
		"  border: 1px solid #68747b;"
		"  border-radius: 3px;"
		"  font-size: 16px;"
		"  font-weight: 800;"
		"  min-width: 30px;"
		"  max-width: 30px;"
		"  min-height: 28px;"
		"  padding: 0;"
		"}"
		"QPushButton#stepperButton:hover:enabled { background: #353e43; border-color: #9aa6ad; color: #ffffff; }"
		"QPushButton#stepperButton:pressed:enabled { background: #15191b; border-color: #6c777e; }"
		"QListWidget { outline: 0; }"
		"QListWidget::item { padding: 5px 6px; border-bottom: 1px solid #121516; }"
		"QListWidget::item:alternate { background: #0b0d0e; }"
		"QListWidget::item:selected { background: #3a1719; color: #fff3f3; }"
		"QPlainTextEdit, QListWidget { font-family: 'Cascadia Mono', Consolas, 'Courier New', monospace; }"
		"QPlainTextEdit { line-height: 120%; }"
		"QPushButton {"
		"  background: #1c2023;"
		"  color: #f1f4f6;"
		"  border: 1px solid #4a5359;"
		"  border-radius: 3px;"
		"  font-weight: 600;"
		"  min-height: 24px;"
		"  padding: 7px 16px;"
		"}"
		"QPushButton:hover:enabled { background: #252b2e; border-color: #68747b; color: #ffffff; }"
		"QPushButton:pressed:enabled { background: #121517; border-color: #5b656b; }"
		"QPushButton:disabled { color: #8a9298; background: #101213; border-color: #2d3337; }"
		"QPushButton#primaryButton {"
		"  color: #ffffff;"
		"  font-weight: 700;"
		"  background: #a1272d;"
		"  border: 1px solid #df5a5f;"
		"}"
		"QPushButton#primaryButton:hover:enabled { background: #bd3339; border-color: #ff777b; color: #ffffff; }"
		"QPushButton#primaryButton:pressed:enabled { background: #7e1e24; border-color: #bf454b; }"
		"QPushButton#primaryButton:disabled { color: #b29295; background: #241315; border-color: #4a2428; }"
		"QTabWidget::pane { border: 1px solid #22272a; top: -1px; }"
		"QTabBar::tab {"
		"  background: #0a0c0d;"
		"  color: #9da4aa;"
		"  border: 1px solid #22272a;"
		"  border-bottom: none;"
		"  padding: 8px 18px;"
		"  margin-right: 2px;"
		"}"
		"QTabBar::tab:hover { color: #d6d9dc; background: #101314; }"
		"QTabBar::tab:selected { color: #f0f2f4; background: #050606; border-top: 2px solid #d14a4a; }"
		"QCheckBox { spacing: 7px; color: #c8cdd1; }"
		"QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #333a3d; background: #070808; }"
		"QCheckBox::indicator:checked { background: #d14a4a; border-color: #d14a4a; }"
		"QScrollBar:vertical { background: #050606; width: 10px; margin: 0; }"
		"QScrollBar::handle:vertical { background: #202629; min-height: 24px; border-radius: 2px; }"
		"QScrollBar::handle:vertical:hover { background: #30383c; }"
		"QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));

}



void SteamP2pMainWindow::wireSignals() {

	connect(&m_bridge, &ControllerBridge::replyReceived, this, &SteamP2pMainWindow::onReply);

	connect(&m_bridge, &ControllerBridge::launcherFinished, this, &SteamP2pMainWindow::onLauncherFinished);

	connect(&m_bridge, &ControllerBridge::exitForUpdate, this, &SteamP2pMainWindow::onLauncherFinished);



	connect(m_btnPrepareHost, &QPushButton::clicked, this, &SteamP2pMainWindow::onPrepareHost);

	connect(m_btnAcceptInvite, &QPushButton::clicked, this, &SteamP2pMainWindow::onAcceptInvite);

	connect(m_btnStartHost, &QPushButton::clicked, this, &SteamP2pMainWindow::onStartHost);

	connect(m_btnStartJoin, &QPushButton::clicked, this, &SteamP2pMainWindow::onStartJoin);

	connect(m_btnStartOffline, &QPushButton::clicked, this, &SteamP2pMainWindow::onStartOfflineTest);

	connect(m_btnRefreshStatus, &QPushButton::clicked, this, &SteamP2pMainWindow::onRefreshStatus);

	connect(m_btnRefreshFriends, &QPushButton::clicked, this, &SteamP2pMainWindow::onRefreshFriends);

	connect(m_onlySf4, &QCheckBox::toggled, this, &SteamP2pMainWindow::onOnlySf4Changed);

	connect(m_friendSearch, &QLineEdit::textChanged, this, &SteamP2pMainWindow::onFriendSearchChanged);

	connect(m_btnClearSearch, &QPushButton::clicked, this, &SteamP2pMainWindow::onClearSearch);

	connect(m_friendsList, &QListWidget::currentRowChanged, this, &SteamP2pMainWindow::onFriendSelectionChanged);

	connect(m_friendsList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {

		if (item && (item->flags() & Qt::ItemIsSelectable)) {

			m_peerSteamId->setText(item->data(Qt::UserRole).toString());

			updateStartButtons();

			appendLog(QStringLiteral("Selected ") + item->text());

		}

	});

	connect(m_peerSteamId, &QLineEdit::textChanged, this, [this](const QString&) { updateStartButtons(); });

	connect(m_btnToggleLog, &QPushButton::clicked, this, &SteamP2pMainWindow::onToggleLog);

	connect(m_btnClearLog, &QPushButton::clicked, this, &SteamP2pMainWindow::onClearLog);

}



void SteamP2pMainWindow::closeEvent(QCloseEvent* event) {

	m_bridge.stopPoll();

	m_bridge.post({ {"type", "steamClose"} });

	sendCancelIfNeeded();

	QSettings settings(QStringLiteral("sf4e"), QStringLiteral("launcher"));

	settings.setValue(QStringLiteral("log-collapsed"), !m_log->isVisible());

	event->accept();

}



void SteamP2pMainWindow::sendCancelIfNeeded() {

	if (!m_controller.IsFinished() && !m_controller.ShouldExitForUpdate()) {

		m_bridge.post({ {"type", "cancel"} });

	}

}



void SteamP2pMainWindow::onLauncherFinished() {

	m_bridge.stopPoll();

	close();

}



void SteamP2pMainWindow::onReply(const nlohmann::json& msg) {

	handleMessage(msg);

}



void SteamP2pMainWindow::appendLog(const QString& text) {

	const QString line = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"))

		+ QStringLiteral("  ") + text;

	m_log->appendPlainText(line);

}



void SteamP2pMainWindow::setStatus(const QString& text, const QString& kind) {

	if (text.isEmpty()) {

		m_statusStrip->hide();

		m_statusStrip->clear();

		return;

	}

	m_statusStrip->show();

	m_statusStrip->setText(text);

	QString style = QStringLiteral("padding: 10px; border-radius: 6px; font-weight: 500;");

	if (kind == QStringLiteral("error")) {

		style += QStringLiteral("background: #4a1515; color: #ffb0b0; border: 1px solid #6a2020;");

	}

	else if (kind == QStringLiteral("success")) {

		style += QStringLiteral("background: #153a20; color: #b0ffb8; border: 1px solid #205a30;");

	}

	else {

		style += QStringLiteral("background: #1a2a4a; color: #b0d0ff; border: 1px solid #2a3a5a;");

	}

	m_statusStrip->setStyleSheet(style);

}



QString SteamP2pMainWindow::peerSteamId() const {

	return m_peerSteamId->text().trimmed();

}



std::string SteamP2pMainWindow::expectedLaunchPeerSteamId() const {

	if (m_launchRole == "join" && m_pendingInvite.valid && !m_pendingInvite.senderSteamId.empty()) {

		return m_pendingInvite.senderSteamId;

	}

	const QString peer = peerSteamId();

	return peer.isEmpty() ? std::string() : peer.toStdString();

}



void SteamP2pMainWindow::updateLaunchWaitPoll() {

	if (m_localLaunchReady && !m_peerLaunchReady && !m_launchTriggered) {

		m_bridge.startPoll(100);

	}

	else {

		m_bridge.startPoll(250);

	}

}



void SteamP2pMainWindow::disarmLaunchWaitTimer() {

	m_launchWaitTimer.stop();

}



void SteamP2pMainWindow::armLaunchWaitTimer() {

	if (m_localLaunchReady && !m_peerLaunchReady && !m_launchTriggered) {

		m_launchWaitTimer.start();

	}

	else {

		disarmLaunchWaitTimer();

	}

}



void SteamP2pMainWindow::recoverLaunchHandshake(const QString& reason, bool clearLocalReady) {

	if (m_controller.IsFinished()) {

		return;

	}

	disarmLaunchWaitTimer();

	m_launchTriggered = false;

	m_peerLaunchReady = false;

	if (clearLocalReady) {

		m_localLaunchReady = false;

	}

	appendLog(reason);

	setStatus(reason, QStringLiteral("error"));

	updateLaunchButtonLabels();

	updateStartButtons();

	updateLaunchWaitPoll();

}



void SteamP2pMainWindow::resendLaunchReady() {

	if (m_launchTriggered || m_launchRole.empty()) {

		return;

	}

	const std::string peer = expectedLaunchPeerSteamId();

	if (peer.empty()) {

		setStatus(QStringLiteral("Missing peer SteamID — cannot resend launch ready"), QStringLiteral("error"));

		return;

	}

	nlohmann::json req;

	req["type"] = "steamFlushLaunchReady";

	req["targetSteamId"] = peer;

	if (!m_launchSessionToken.empty()) {

		req["sessionToken"] = m_launchSessionToken;

	}

	appendLog(QStringLiteral("Resending launch-ready to opponent..."));

	m_bridge.post(req);

}



void SteamP2pMainWindow::onLaunchWaitTimeout() {

	if (!m_localLaunchReady || m_peerLaunchReady || m_launchTriggered) {

		return;

	}

	appendLog(QStringLiteral("Timed out waiting for opponent launch-ready — resending signal"));

	resendLaunchReady();

	setStatus(

		QStringLiteral("Still waiting for opponent. Ask them to press Ready to launch, or click your button again to resend."),

		QStringLiteral("error")

	);

	armLaunchWaitTimer();

}



void SteamP2pMainWindow::resetLaunchHandshake() {

	m_localLaunchReady = false;

	m_peerLaunchReady = false;

	m_localLaunchCommit = false;

	m_peerLaunchCommit = false;

	m_launchTriggered = false;

	m_launchRole.clear();

	m_launchSessionToken.clear();

	m_bridge.post({ {"type", "steamDrainLaunchReady"} });

	disarmLaunchWaitTimer();

	updateLaunchButtonLabels();

	updateStartButtons();

	updateLaunchWaitPoll();

}



void SteamP2pMainWindow::updateLaunchButtonLabels() {

	if (m_launchTriggered) {

		const QString launching = QStringLiteral("Launching...");

		if (m_btnStartHost) {

			m_btnStartHost->setText(launching);

		}

		if (m_btnStartJoin) {

			m_btnStartJoin->setText(launching);

		}

		return;

	}

	if (m_localLaunchReady && !m_peerLaunchReady) {

		const QString waiting = QStringLiteral("Waiting for opponent...");

		if (m_btnStartHost) {

			m_btnStartHost->setText(waiting);

		}

		if (m_btnStartJoin) {

			m_btnStartJoin->setText(waiting);

		}

		return;

	}

	const QString ready = QStringLiteral("Ready to launch game");

	if (m_btnStartHost) {

		m_btnStartHost->setText(ready);

	}

	if (m_btnStartJoin) {

		m_btnStartJoin->setText(ready);

	}

}



void SteamP2pMainWindow::updateStartButtons() {

	const bool hasPeer = !peerSteamId().isEmpty();

	const bool canLaunch = !m_launchTriggered && !m_launchRole.empty();

	const bool canSignalReady = canLaunch && (m_p2pConnected || m_localLaunchReady || m_p2pEverConnected);

	m_btnStartHost->setEnabled(canSignalReady && hasPeer && m_launchRole == "host");

	m_btnStartJoin->setEnabled(canSignalReady && m_pendingInvite.valid && m_launchRole == "join");



	if (m_launchTriggered) {

		m_hostStartHint->setText(QStringLiteral("Launching USF4..."));

		m_joinStartHint->setText(QStringLiteral("Launching USF4..."));

	}

	else if (m_localLaunchReady && m_peerLaunchReady && m_localLaunchCommit && m_peerLaunchCommit) {

		m_hostStartHint->setText(QStringLiteral("Both committed — launching now."));

		m_joinStartHint->setText(QStringLiteral("Both committed — launching now."));

	}

	else if (m_localLaunchReady && m_peerLaunchReady) {

		m_hostStartHint->setText(QStringLiteral("Both ready — confirming launch commit..."));

		m_joinStartHint->setText(QStringLiteral("Both ready — confirming launch commit..."));

	}

	else if (m_localLaunchReady) {

		m_hostStartHint->setText(QStringLiteral("You are ready — waiting for opponent to press Ready to launch."));

		m_joinStartHint->setText(QStringLiteral("You are ready — waiting for opponent to press Ready to launch."));

	}

	else if (m_btnStartHost->isEnabled()) {

		m_hostStartHint->setText(QStringLiteral("P2P connected — press Ready to launch when you are both set."));

	}

	else if (!hasPeer) {

		m_hostStartHint->setText(QStringLiteral("Select a friend (or enter SteamID64) before sending an invite."));

	}

	else if (!m_p2pConnected) {

		m_hostStartHint->setText(QStringLiteral("Send invite + listen, then wait until P2P shows connected."));

	}



	if (m_btnStartJoin->isEnabled()) {

		m_joinStartHint->setText(QStringLiteral("P2P connected — press Ready to launch when you are both set."));

	}

	else if (!m_pendingInvite.valid) {

		m_joinStartHint->setText(QStringLiteral("Wait for a Steam invite from the host (see activity log)."));

	}

	else if (!m_p2pConnected) {

		m_joinStartHint->setText(QStringLiteral("Click Accept invite + connect, then wait for P2P connected."));

	}

}



void SteamP2pMainWindow::updateConnectionLines() {

	if (m_p2pConnected) {

		m_hostConnection->setText(QStringLiteral("P2P: connected"));

		m_joinConnection->setText(QStringLiteral("P2P: connected"));

		m_hostConnection->setStyleSheet(QStringLiteral("color: #8fd48f;"));

		m_joinConnection->setStyleSheet(QStringLiteral("color: #8fd48f;"));

	}

	else {

		m_hostConnection->setText(QStringLiteral("P2P: not connected"));

		m_joinConnection->setText(m_pendingInvite.valid

			? QStringLiteral("P2P: invite received — accept to connect")

			: QStringLiteral("P2P: waiting for invite"));

		m_hostConnection->setStyleSheet(QStringLiteral("color: #d0a060;"));

		m_joinConnection->setStyleSheet(QStringLiteral("color: #d0a060;"));

	}

}



void SteamP2pMainWindow::applySteamStatus(const nlohmann::json& s) {

	if (s.contains("connected")) {

		const bool wasConnected = m_p2pConnected;

		m_p2pConnected = JsonBool(s, "connected", false);

		if (m_p2pConnected) {

			m_p2pEverConnected = true;

		}

		if (wasConnected && !m_p2pConnected && m_localLaunchReady && !m_launchTriggered) {

			appendLog(QStringLiteral("P2P socket closed — launch handshake uses Steam messages; press Ready again if needed"));

		}

		updateStartButtons();

		updateConnectionLines();

	}

	m_steamInit->setText(JsonBool(s, "initialized", false) ? QStringLiteral("initialized") : QStringLiteral("not initialized"));

	m_steamId->setText(JsonString(s, "steamId", QStringLiteral("—")));

	m_persona->setText(JsonString(s, "persona", QStringLiteral("—")));

	if (JsonBool(s, "connected", false)) {

		m_p2pState->setText(QStringLiteral("connected"));

	}

	else if (JsonBool(s, "failed", false)) {

		m_p2pState->setText(QStringLiteral("failed"));

	}

	else {

		m_p2pState->setText(QStringLiteral("idle / listening"));

	}

	const QString err = JsonString(s, "lastError");

	const QString evt = JsonString(s, "lastEvent");

	if (!err.isEmpty()) {

		m_lastEvent->setText(QStringLiteral("Error: ") + err);

		appendLog(QStringLiteral("ERROR: ") + err);

	}

	else if (!evt.isEmpty()) {

		m_lastEvent->setText(evt);

		appendLog(evt);

	}

	if (s.contains("messages")) {

		processMessages(s["messages"]);

	}

}



void SteamP2pMainWindow::processMessages(const nlohmann::json& messages) {

	if (!messages.is_array()) {

		return;

	}

	for (const auto& m : messages) {

		if (m.contains("kind") && m["kind"] == "launch_ready") {

			const std::string expectedPeer = expectedLaunchPeerSteamId();

			std::string sender;

			if (m.contains("launchReady") && m["launchReady"].is_object()) {

				const auto& lr = m["launchReady"];

				if (lr.contains("senderSteamId")) {

					if (lr["senderSteamId"].is_string()) {

						sender = lr["senderSteamId"].get<std::string>();

					}

					else {

						sender = std::to_string(lr["senderSteamId"].get<unsigned long long>());

					}

				}

			}

			if (sender.empty() && m.contains("fromSteamId")) {

				if (m["fromSteamId"].is_string()) {

					sender = m["fromSteamId"].get<std::string>();

				}

				else {

					sender = std::to_string(m["fromSteamId"].get<unsigned long long>());

				}

			}

			if (!expectedPeer.empty() && !sender.empty() && !SteamIdsMatch(sender, expectedPeer)) {

				appendLog(QStringLiteral("Ignored launch-ready from unexpected SteamID ") + QString::fromStdString(sender));

				continue;

			}

			const QString readyToken = LaunchReadySessionTokenFromMessage(m);

			if (m_launchSessionToken.empty() || readyToken.isEmpty()
				|| readyToken.toStdString() != m_launchSessionToken) {
				appendLog(QStringLiteral("Ignored stale launch-ready (session token mismatch)"));
				sf4e::agent_debug::Log(
					"H2",
					"steam_p2p_window.cxx:processMessages",
					"launch_ready_rejected",
					{
						{ "reason", "token_mismatch" },
						{ "hasLocalToken", !m_launchSessionToken.empty() },
						{ "hasReadyToken", !readyToken.isEmpty() },
						{ "sender", sender },
						{ "expectedPeer", expectedPeer }
					}
				);
				continue;
			}

			m_peerLaunchReady = true;
			sf4e::agent_debug::Log(
				"H4",
				"steam_p2p_window.cxx:processMessages",
				"peer_launch_ready_set",
				{
					{ "sender", sender },
					{ "localReady", m_localLaunchReady },
					{ "launchRole", m_launchRole }
				}
			);

			appendLog(QStringLiteral("Opponent is ready to launch"));

			setStatus(QStringLiteral("Opponent ready — launching when you are too"), QStringLiteral("info"));

			updateLaunchWaitPoll();

			armLaunchWaitTimer();

			trySynchronizedLaunch();

			updateLaunchButtonLabels();

			updateStartButtons();

			continue;

		}

		if (m.contains("kind") && m["kind"] == "launch_commit") {

			const std::string expectedPeer = expectedLaunchPeerSteamId();

			std::string sender;

			if (m.contains("launchCommit") && m["launchCommit"].is_object()) {

				const auto& lc = m["launchCommit"];

				if (lc.contains("senderSteamId")) {

					if (lc["senderSteamId"].is_string()) {

						sender = lc["senderSteamId"].get<std::string>();

					}

					else {

						sender = std::to_string(lc["senderSteamId"].get<unsigned long long>());

					}

				}

			}

			if (sender.empty() && m.contains("fromSteamId")) {

				if (m["fromSteamId"].is_string()) {

					sender = m["fromSteamId"].get<std::string>();

				}

				else {

					sender = std::to_string(m["fromSteamId"].get<unsigned long long>());

				}

			}

			if (!expectedPeer.empty() && !sender.empty() && !SteamIdsMatch(sender, expectedPeer)) {

				appendLog(QStringLiteral("Ignored launch-commit from unexpected SteamID ") + QString::fromStdString(sender));

				continue;

			}

			const QString commitToken = LaunchCommitSessionTokenFromMessage(m);

			if (m_launchSessionToken.empty() || commitToken.isEmpty()
				|| commitToken.toStdString() != m_launchSessionToken) {

				appendLog(QStringLiteral("Ignored stale launch-commit (session token mismatch)"));

				sf4e::agent_debug::Log(
					"H2",
					"steam_p2p_window.cxx:processMessages",
					"launch_commit_rejected",
					{ { "reason", "token_mismatch" } }
				);

				continue;

			}

			m_peerLaunchCommit = true;

			appendLog(QStringLiteral("Opponent committed to launch"));

			sf4e::agent_debug::Log(
				"H5",
				"steam_p2p_window.cxx:processMessages",
				"peer_launch_commit_set",
				{ { "sender", sender } }
			);

			trySynchronizedLaunch();

			updateLaunchButtonLabels();

			updateStartButtons();

			continue;

		}

		if (!m.contains("kind") || m["kind"] != "invite") {

			continue;

		}

		nlohmann::json invite = m.contains("invite") ? m["invite"] : m;

		m_pendingInvite.valid = true;

		m_peerLaunchReady = false;

		m_pendingInvite.senderSteamId.clear();

		if (invite.contains("senderSteamId")) {

			if (invite["senderSteamId"].is_string()) {

				m_pendingInvite.senderSteamId = invite["senderSteamId"].get<std::string>();

			}

			else {

				m_pendingInvite.senderSteamId = std::to_string(invite["senderSteamId"].get<unsigned long long>());

			}

		}

		if (m_pendingInvite.senderSteamId.empty() && m.contains("fromSteamId")) {

			m_pendingInvite.senderSteamId = std::to_string(m["fromSteamId"].get<unsigned long long>());

		}

		m_pendingInvite.virtualPort = invite.value("virtualPort", 7);

		m_pendingInvite.role = invite.value("role", "host");

		m_pendingInvite.sidecarHash = invite.value("sidecarHash", "");

		m_pendingInvite.buildGit = invite.value("buildGit", "");

		m_pendingInvite.sessionToken = invite.value("sessionToken", "");

		if (!m_pendingInvite.sessionToken.empty()) {

			m_launchSessionToken = m_pendingInvite.sessionToken;

		}

		m_peerSteamId->setText(QString::fromStdString(m_pendingInvite.senderSteamId));

		appendLog(QStringLiteral("Invite received from ") + QString::fromStdString(m_pendingInvite.senderSteamId));

		renderInviteCard();

		setStatus(QStringLiteral("Steam invite received — open Join tab and accept"), QStringLiteral("info"));

		updateStartButtons();

		updateConnectionLines();

	}

}



void SteamP2pMainWindow::renderInviteCard() {

	if (!m_pendingInvite.valid) {

		m_inviteSummary->setText(QStringLiteral("No invite yet — ask the host to send one, or watch the activity log."));

		m_btnAcceptInvite->setEnabled(false);

		return;

	}

	const QString build = m_pendingInvite.buildGit.empty()

		? QStringLiteral("(unknown build)")

		: QString::fromStdString(m_pendingInvite.buildGit);

	m_inviteSummary->setText(QStringLiteral("<b>Invite from host</b><br>SteamID %1 · port %2 · build %3")

		.arg(QString::fromStdString(m_pendingInvite.senderSteamId))

		.arg(m_pendingInvite.virtualPort)

		.arg(build));

	m_btnAcceptInvite->setEnabled(true);

}



void SteamP2pMainWindow::syncNamesFromState(const nlohmann::json& s) {

	const QString name = JsonString(s, "displayName");

	if (!name.isEmpty()) {

		m_hostName->setText(name);

		m_joinName->setText(name);

		if (m_offlineName) {

			m_offlineName->setText(name);

		}

	}

	int delay = s.value("inputDelay", 2);

	m_hostDelay->setValue(delay);

	m_joinDelay->setValue(delay);

	if (m_offlineDelay) {

		m_offlineDelay->setValue(delay);

	}

	const QString ver = JsonString(s, "installedVersion", QStringLiteral("unknown"));

	m_versionLabel->setText(QStringLiteral("build: ") + ver);

	m_hostBuildHint->setText(QStringLiteral("Both players must use the same build (%1).").arg(ver));

}



void SteamP2pMainWindow::renderFriends() {

	m_friendsList->clear();

	const int total = m_allFriends.is_array() ? (int)m_allFriends.size() : 0;

	const QString query = m_friendSearch->text().trimmed().toLower();

	const bool onlySf4 = m_onlySf4->isChecked();

	int shown = 0;



	if (!m_allFriends.is_array() || total == 0) {

		m_friendCount->setText(QStringLiteral("0 / 0 friends — click Refresh"));

		auto* item = new QListWidgetItem(QStringLiteral("(no friends loaded — click Refresh)"));

		item->setFlags(Qt::NoItemFlags);

		m_friendsList->addItem(item);

		return;

	}



	for (const auto& f : m_allFriends) {

		const bool inSf4 = JsonBool(f, "inSf4", false);

		if (onlySf4 && !inSf4) {

			continue;

		}

		const QString name = JsonString(f, "name", QStringLiteral("Unknown"));

		const QString steamId = JsonString(f, "steamId");

		const QString status = inSf4 ? QStringLiteral("in USF4") : QStringLiteral("state %1").arg(f.value("personaState", 0));

		if (!query.isEmpty()) {

			if (!name.toLower().contains(query) && !steamId.toLower().contains(query) && !status.toLower().contains(query)) {

				continue;

			}

		}

		auto* item = new QListWidgetItem(QStringLiteral("%1  ·  %2  [%3]").arg(name, steamId, status));

		item->setData(Qt::UserRole, steamId);

		m_friendsList->addItem(item);

		shown++;

	}

	m_friendCount->setText(QStringLiteral("%1 shown / %2 total").arg(shown).arg(total));

	if (shown == 0) {

		auto* item = new QListWidgetItem(query.isEmpty()

			? QStringLiteral("(no friends match filters)")

			: QStringLiteral("(no friends match search)"));

		item->setFlags(Qt::NoItemFlags);

		m_friendsList->addItem(item);

	}

}



void SteamP2pMainWindow::handleMessage(const nlohmann::json& msg) {

	if (!msg.contains("type")) {

		return;

	}

	const std::string type = msg["type"].get<std::string>();



	if (type == "state") {

		m_state = msg;

		syncNamesFromState(msg);

		m_bridge.post({ {"type", "steamBuildInfo"} });

		m_bridge.post({ {"type", "steamStatus"} });

		m_bridge.post({ {"type", "steamRefreshFriends"}, {"onlySf4", false} });

		return;

	}

	if (type == "steamBuildInfo") {

		if (msg.contains("sidecarHash")) {

			m_steamBuild.sidecarHash = msg["sidecarHash"].get<std::string>();

		}

		if (msg.contains("buildGit")) {

			m_steamBuild.buildGit = msg["buildGit"].get<std::string>();

		}

		if (JsonBool(msg, "ok", true) == false) {

			appendLog(QStringLiteral("Build info: ") + JsonString(msg, "message", QStringLiteral("unavailable")));

		}

		return;

	}

	if (type == "steamLaunchReady") {

		if (m_launchTriggered) {

			return;

		}

		if (JsonBool(msg, "ok", false)) {

			m_localLaunchReady = true;
			sf4e::agent_debug::Log(
				"H1",
				"steam_p2p_window.cxx:steamLaunchReady",
				"local_ready_set",
				{
					{ "peerLaunchReadyFromPoll", JsonBool(msg, "peerLaunchReady", false) },
					{ "hasSessionToken", !m_launchSessionToken.empty() },
					{ "launchRole", m_launchRole },
					{ "expectedPeer", expectedLaunchPeerSteamId() }
				}
			);

			if (JsonBool(msg, "peerLaunchReady", false)) {

				m_peerLaunchReady = true;

				appendLog(QStringLiteral("Opponent already ready — starting launch"));

			}

			else {

				appendLog(QStringLiteral("You are ready to launch — waiting for opponent"));

			}

			updateLaunchButtonLabels();

			updateStartButtons();

			updateLaunchWaitPoll();

			armLaunchWaitTimer();

			trySynchronizedLaunch();

		}

		else {

			const QString err = JsonString(msg, "message", QStringLiteral("Could not send launch ready"));

			recoverLaunchHandshake(QStringLiteral("Could not send launch ready: ") + err, false);

		}

		return;

	}

	if (type == "error") {

		const QString err = JsonString(msg, "message", QStringLiteral("unknown"));

		appendLog(QStringLiteral("ERROR: ") + err);

		if (m_launchTriggered || m_localLaunchReady) {

			recoverLaunchHandshake(

				QStringLiteral("Launch failed: ") + err,

				true

			);

		}

		else {

			setStatus(err, QStringLiteral("error"));

		}

		return;

	}

	if (type == "steamFriends") {

		applySteamStatus(msg);

		m_allFriends = msg.contains("friends") ? msg["friends"] : nlohmann::json::array();

		renderFriends();

		return;

	}

	if (type == "steamStatus" || type == "steamListen" || type == "steamConnect" || type == "steamClosed"

		|| type == "steamMessages" || type == "steamPrepareHost" || type == "steamPrepareJoin") {

		applySteamStatus(msg);

		if (type == "steamPrepareHost") {

			const bool inviteOk = JsonBool(msg, "inviteOk", JsonBool(msg, "ok", false));

			const bool listenOk = JsonBool(msg, "listenOk", JsonBool(msg, "ok", false));

			if (inviteOk && listenOk) {

				resetLaunchHandshake();

				m_launchRole = "host";

				const QString token = JsonString(msg, "sessionToken", QString());

				if (!token.isEmpty()) {

					m_launchSessionToken = token.toStdString();

				}

			}

			if (!inviteOk) {

				const QString detail = JsonString(msg, "message", JsonString(msg, "lastError", QStringLiteral("Steam invite send failed")));

				setStatus(detail, QStringLiteral("error"));

				appendLog(detail);

			}

			else if (!listenOk) {

				const QString detail = JsonString(msg, "message", JsonString(msg, "lastError", QStringLiteral("Could not listen for P2P")));

				setStatus(detail, QStringLiteral("error"));

				appendLog(detail);

			}

			else {

				setStatus(QStringLiteral("Invite sent — waiting for joiner (they need this launcher open on Join)"), QStringLiteral("success"));

				appendLog(QStringLiteral("Invite sent; listening on port ") + QString::number(m_virtualPort->value()));

			}

		}

		if (type == "steamPrepareJoin") {

			if (JsonBool(msg, "ok", false)) {

				resetLaunchHandshake();

				m_launchRole = "join";

				const QString token = JsonString(msg, "sessionToken", QString());

				if (!token.isEmpty()) {

					m_launchSessionToken = token.toStdString();

				}

				else if (!m_pendingInvite.sessionToken.empty()) {

					m_launchSessionToken = m_pendingInvite.sessionToken;

				}

				setStatus(QStringLiteral("Connected to host via Steam P2P"), QStringLiteral("success"));

				appendLog(QStringLiteral("Joined host Steam P2P session"));

			}

			else {

				setStatus(JsonString(msg, "message", JsonString(msg, "lastError", QStringLiteral("Join failed"))), QStringLiteral("error"));

			}

		}

		if (type == "steamMessages" && msg.contains("messages")) {

			processMessages(msg["messages"]);

		}

	}

}



void SteamP2pMainWindow::onRefreshStatus() {

	m_bridge.post({ {"type", "steamStatus"} });

}



void SteamP2pMainWindow::onRefreshFriends() {

	m_bridge.post({ {"type", "steamRefreshFriends"}, {"onlySf4", m_onlySf4->isChecked()} });

}



void SteamP2pMainWindow::onOnlySf4Changed() {

	renderFriends();

}



void SteamP2pMainWindow::onFriendSearchChanged() {

	renderFriends();

}



void SteamP2pMainWindow::onClearSearch() {

	m_friendSearch->clear();

	renderFriends();

}



void SteamP2pMainWindow::onFriendSelectionChanged() {

	auto* item = m_friendsList->currentItem();

	if (item && item->flags() & Qt::ItemIsSelectable) {

		m_peerSteamId->setText(item->data(Qt::UserRole).toString());

		updateStartButtons();

	}

}



void SteamP2pMainWindow::onPrepareHost() {

	if (peerSteamId().isEmpty()) {

		setStatus(QStringLiteral("Select a friend before sending an invite"), QStringLiteral("error"));

		appendLog(QStringLiteral("Select a friend first."));

		return;

	}

	nlohmann::json req;

	req["type"] = "steamPrepareHost";

	req["targetSteamId"] = peerSteamId().toStdString();

	req["virtualPort"] = m_virtualPort->value();

	req["sidecarHash"] = m_steamBuild.sidecarHash;

	req["buildGit"] = m_steamBuild.buildGit;

	m_bridge.post(req);

}



void SteamP2pMainWindow::onAcceptInvite() {

	if (!m_pendingInvite.valid) {

		return;

	}

	nlohmann::json req;

	req["type"] = "steamPrepareJoin";

	req["senderSteamId"] = m_pendingInvite.senderSteamId;

	req["virtualPort"] = m_pendingInvite.virtualPort;

	req["role"] = m_pendingInvite.role;

	req["sidecarHash"] = m_pendingInvite.sidecarHash;

	req["buildGit"] = m_pendingInvite.buildGit;

	req["inviteVersion"] = 1;

	m_bridge.post(req);

}



void SteamP2pMainWindow::startGame(const char* mode) {

	const bool isHost = (strcmp(mode, "host") == 0);

	nlohmann::json req;

	req["type"] = "steamStart";

	req["mode"] = mode;

	req["displayName"] = isHost ? m_hostName->text().toStdString() : m_joinName->text().toStdString();

	req["inputDelay"] = isHost ? m_hostDelay->value() : m_joinDelay->value();

	req["virtualPort"] = m_virtualPort->value();

	if (isHost) {

		req["peerSteamId"] = peerSteamId().toStdString();

	}

	else {

		req["peerSteamId"] = m_pendingInvite.valid ? m_pendingInvite.senderSteamId : peerSteamId().toStdString();

	}

	req["connectMethod"] = "steam";

	m_bridge.post(req);

}



void SteamP2pMainWindow::markLaunchReady(const char* mode) {

	if (m_launchTriggered || m_launchRole.empty()) {

		return;

	}

	if (!m_p2pConnected && !m_localLaunchReady && !m_p2pEverConnected) {

		setStatus(QStringLiteral("P2P is not connected yet"), QStringLiteral("error"));

		return;

	}

	nlohmann::json req;

	req["type"] = m_localLaunchReady ? "steamFlushLaunchReady" : "steamMarkLaunchReady";

	req["targetSteamId"] = (strcmp(mode, "host") == 0)

		? peerSteamId().toStdString()

		: (m_pendingInvite.valid ? m_pendingInvite.senderSteamId : peerSteamId().toStdString());

	if (!m_launchSessionToken.empty()) {

		req["sessionToken"] = m_launchSessionToken;

	}

	sf4e::agent_debug::Log(
		"H1",
		"steam_p2p_window.cxx:markLaunchReady",
		"send",
		{
			{ "mode", mode },
			{ "hasSessionToken", !m_launchSessionToken.empty() },
			{ "targetSteamId", req["targetSteamId"].get<std::string>() },
			{ "p2pConnected", m_p2pConnected },
			{ "localReadyAlready", m_localLaunchReady }
		}
	);
	m_bridge.post(req);

}



void SteamP2pMainWindow::trySynchronizedLaunch() {

	sf4e::agent_debug::Log(
		"H3",
		"steam_p2p_window.cxx:trySynchronizedLaunch",
		"entry",
		{
			{ "launchTriggered", m_launchTriggered },
			{ "localReady", m_localLaunchReady },
			{ "peerReady", m_peerLaunchReady },
			{ "launchRole", m_launchRole },
			{ "hasSessionToken", !m_launchSessionToken.empty() }
		}
	);

	if (m_launchTriggered || !m_localLaunchReady || !m_peerLaunchReady || m_launchRole.empty()) {

		return;

	}

	if (m_launchSessionToken.empty()) {

		sf4e::agent_debug::Log("H1", "steam_p2p_window.cxx:trySynchronizedLaunch", "abort_no_session_token", {});
		setStatus(QStringLiteral("Session token missing — resend invite and connect again"), QStringLiteral("error"));

		return;

	}

	const std::string peer = expectedLaunchPeerSteamId();

	if (peer.empty()) {

		setStatus(QStringLiteral("Missing peer SteamID — cannot launch"), QStringLiteral("error"));

		return;

	}

	disarmLaunchWaitTimer();

	if (!m_peerLaunchCommit || !m_localLaunchCommit) {

		appendLog(QStringLiteral("Confirming launch commit with opponent..."));

		nlohmann::json commit;

		commit["type"] = "steamConfirmPeerLaunchCommit";

		commit["targetSteamId"] = peer;

		commit["sessionToken"] = m_launchSessionToken;

		commit["pumpAttempts"] = 24;

		const nlohmann::json reply = m_bridge.post(commit);

		const bool peerCommit = JsonBool(reply, "peerLaunchCommit", false);

		if (JsonBool(reply, "localLaunchCommit", false)) {

			m_localLaunchCommit = true;

		}

		if (peerCommit) {

			m_peerLaunchCommit = true;

		}

		sf4e::agent_debug::Log(
			"H3",
			"steam_p2p_window.cxx:trySynchronizedLaunch",
			"confirm_commit_result",
			{
				{ "peerLaunchCommit", peerCommit },
				{ "localLaunchCommit", m_localLaunchCommit },
				{ "launchRole", m_launchRole },
				{ "targetPeer", peer }
			}
		);

		if (!m_peerLaunchCommit || !m_localLaunchCommit) {

			appendLog(QStringLiteral("Waiting for opponent launch commit — press Ready again on both PCs"));

			setStatus(

				QStringLiteral("Waiting for opponent to confirm launch"),

				QStringLiteral("error")

			);

			armLaunchWaitTimer();

			return;

		}

	}

	m_launchTriggered = true;

	updateLaunchButtonLabels();

	updateStartButtons();

	updateLaunchWaitPoll();

	appendLog(QStringLiteral("Both players ready — launching USF4"));

	setStatus(QStringLiteral("Launching USF4 on both PCs..."), QStringLiteral("success"));

	nlohmann::json flush;

	flush["type"] = "steamFlushLaunchReady";

	flush["targetSteamId"] = peer;

	flush["sessionToken"] = m_launchSessionToken;

	m_bridge.post(flush);

	sf4e::agent_debug::Log(
		"H5",
		"steam_p2p_window.cxx:trySynchronizedLaunch",
		"start_game",
		{ { "launchRole", m_launchRole } }
	);
	startGame(m_launchRole.c_str());

}



void SteamP2pMainWindow::onStartHost() {

	markLaunchReady("host");

}



void SteamP2pMainWindow::onStartJoin() {

	markLaunchReady("join");

}



void SteamP2pMainWindow::onStartOfflineTest() {

	nlohmann::json req;

	req["v"] = 1;

	req["type"] = "offlineStart";

	req["displayName"] = m_offlineName ? m_offlineName->text().toStdString() : "";

	req["inputDelay"] = m_offlineDelay ? m_offlineDelay->value() : 2;

	req["devOverlay"] = true;

	appendLog(QStringLiteral("Launching offline tester mode with Sidecar overlay enabled"));

	m_bridge.post(req);

}



void SteamP2pMainWindow::onToggleLog() {

	const bool show = !m_log->isVisible();

	m_log->setVisible(show);

	m_btnToggleLog->setText(show ? QStringLiteral("Hide activity log") : QStringLiteral("Show activity log"));

}



void SteamP2pMainWindow::onClearLog() {

	m_log->clear();

}



} // namespace launcher

} // namespace sf4e

