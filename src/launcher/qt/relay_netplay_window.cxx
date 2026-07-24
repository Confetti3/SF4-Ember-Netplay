#include "relay_netplay_window.hxx"

#include "launcher_common.hxx"
#include "launcher_theme.hxx"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace sf4e {
namespace launcher {

namespace {

// Shared label-column width so form rows line up across separate group boxes.
// Sized for the longest label on the host page ("Internet address").
const int kFormLabelWidth = 104;

// One spacing scale for the whole window. Everything used to sit at 4-6px,
// which read as cramped; these give each control room to breathe without
// pushing content off a short page.
const int kGroupPadding = 12;  // inside a group box
const int kRowSpacing = 10;    // between rows within a group
const int kGroupSpacing = 12;  // between group boxes
const int kLabelGap = 12;      // between a label and its field

// Back buttons should hug their text on the left rather than stretching the
// full page width, which makes them read as a banner instead of a control.
QPushButton* MakeBackButton() {
	auto* btn = new QPushButton(QStringLiteral("←  Back"));
	btn->setObjectName(QStringLiteral("backButton"));
	btn->setCursor(Qt::PointingHandCursor);
	btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	btn->setToolTip(QStringLiteral("Return to the main menu (Esc)"));
	return btn;
}

QString FormatVersionBadge(QString ver) {
	ver = ver.trimmed();
	if (ver.isEmpty()
		|| ver.compare(QStringLiteral("unknown"), Qt::CaseInsensitive) == 0
		|| ver.compare(QStringLiteral("vunknown"), Qt::CaseInsensitive) == 0) {
#ifdef SF4E_APP_VERSION
		ver = QStringLiteral(SF4E_APP_VERSION);
#else
		ver = QStringLiteral("dev");
#endif
	}
	if (!ver.startsWith('v') && ver.compare(QStringLiteral("dev"), Qt::CaseInsensitive) != 0) {
		ver.prepend('v');
	}
	return ver;
}

QString SoftenReleaseNotes(QString notes) {
	notes.replace(QRegularExpression(QStringLiteral("^#+\\s*"), QRegularExpression::MultilineOption), QString());
	notes.replace(QRegularExpression(QStringLiteral("^>\\s?"), QRegularExpression::MultilineOption), QString());
	notes.replace(QStringLiteral("**"), QString());
	notes.replace(QStringLiteral("__"), QString());
	notes.replace(QRegularExpression(QStringLiteral("\\[([^\\]]+)\\]\\([^\\)]+\\)")), QStringLiteral("\\1"));
	notes.replace(QRegularExpression(QStringLiteral("`([^`]+)`")), QStringLiteral("\\1"));
	notes = notes.trimmed();
	constexpr int kMaxChars = 360;
	if (notes.size() > kMaxChars) {
		notes = notes.left(kMaxChars).trimmed() + QStringLiteral("…");
	}
	return notes;
}

} // namespace

RelayNetplayWindow::RelayNetplayWindow(NetplayLaunchController& controller, QWidget* parent)
	: QMainWindow(parent)
	, m_controller(controller)
	, m_bridge(controller, this) {
	setWindowTitle(QStringLiteral("SF4 Netplay Launcher"));
	// Sized for the roomier spacing scale: the host page in Advanced is the
	// tallest layout, and this fits it without forcing a scrollbar.
	setMinimumSize(500, 560);
	resize(560, 760);

	buildUi();
	applyTheme();
	setAdvancedVisible(!m_simpleUi);
	wireSignals();

	QObject::connect(&m_bridge, &ControllerBridge::replyReceived, this, &RelayNetplayWindow::onReply);
	QObject::connect(&m_bridge, &ControllerBridge::launcherFinished, this, &RelayNetplayWindow::onLauncherFinished);
	QObject::connect(&m_bridge, &ControllerBridge::exitForUpdate, this, &RelayNetplayWindow::onExitForUpdate);
	QObject::connect(&m_heartbeatTimer, &QTimer::timeout, this, &RelayNetplayWindow::onRelayHeartbeat);
	m_toastTimer.setSingleShot(true);
	QObject::connect(&m_toastTimer, &QTimer::timeout, this, [this]() {
		if (m_toastLabel) {
			m_toastLabel->hide();
		}
	});

	m_bridge.post({ { "type", "getState" } });
}

void RelayNetplayWindow::keyPressEvent(QKeyEvent* event) {
	// One window-level handler rather than a shortcut on each of the four back
	// buttons: those would be ambiguous across the stacked pages. On Home there
	// is nowhere to go back to, so Esc falls through to the default handling.
	if (event->key() == Qt::Key_Escape && m_stack &&
		m_stack->currentIndex() != static_cast<int>(Screen::Home)) {
		onGoHome();
		event->accept();
		return;
	}
	QMainWindow::keyPressEvent(event);
}

void RelayNetplayWindow::closeEvent(QCloseEvent* event) {
	stopRelayHeartbeat();
	if (!m_controller.IsFinished() && !m_controller.WasCancelled()) {
		m_bridge.post({ { "type", "cancel" } });
	}
	QMainWindow::closeEvent(event);
}

void RelayNetplayWindow::buildUi() {
	auto* central = new QWidget(this);
	central->setObjectName(QStringLiteral("centralWidget"));
	setCentralWidget(central);

	auto* root = new QVBoxLayout(central);
	root->setContentsMargins(12, 10, 12, 10);
	root->setSpacing(6);

	auto* header = new QHBoxLayout();
	auto* titleCol = new QVBoxLayout();
	titleCol->setSpacing(1);
	auto* eyebrow = new QLabel(QStringLiteral("ULTRA STREET FIGHTER IV"));
	eyebrow->setObjectName(QStringLiteral("eyebrow"));
	auto* title = new QLabel(QStringLiteral("SF4 Netplay Launcher"));
	title->setObjectName(QStringLiteral("title"));
	m_subtitleLabel = new QLabel(
		QStringLiteral("USF4 rollback netplay — friends-only experimental build"));
	m_subtitleLabel->setObjectName(QStringLiteral("subtitle"));
	titleCol->addWidget(eyebrow);
	titleCol->addWidget(title);
	titleCol->addWidget(m_subtitleLabel);
	header->addLayout(titleCol);
	header->addStretch();
	m_versionLabel = new QLabel(QStringLiteral("dev"));
	m_versionLabel->setObjectName(QStringLiteral("versionBadge"));
	header->addWidget(m_versionLabel, 0, Qt::AlignTop);
	root->addLayout(header);

	auto* modeRow = new QHBoxLayout();
	modeRow->setSpacing(6);
	m_btnModeSimple = new QPushButton(QStringLiteral("Simple"));
	m_btnModeSimple->setObjectName(QStringLiteral("segmentBtn"));
	m_btnModeSimple->setCheckable(true);
	m_btnModeSimple->setChecked(true);
	m_btnModeAdvanced = new QPushButton(QStringLiteral("Advanced"));
	m_btnModeAdvanced->setObjectName(QStringLiteral("segmentBtn"));
	m_btnModeAdvanced->setCheckable(true);
	modeRow->addWidget(m_btnModeSimple);
	modeRow->addWidget(m_btnModeAdvanced);
	modeRow->addStretch();
	root->addLayout(modeRow);

	auto* warning = new QLabel(
		QStringLiteral("Experimental unofficial port — netplay may fail. Both players need the same build."));
	warning->setObjectName(QStringLiteral("warningBanner"));
	warning->setWordWrap(true);
	m_warningBanner = warning;
	root->addWidget(warning);

	m_statusStrip = new QLabel(QStringLiteral("Ready"));
	m_statusStrip->setObjectName(QStringLiteral("connectionLine"));
	m_statusStrip->setWordWrap(true);
	root->addWidget(m_statusStrip);

	m_toastLabel = new QLabel();
	m_toastLabel->setObjectName(QStringLiteral("toastLabel"));
	m_toastLabel->hide();
	m_toastLabel->setWordWrap(true);
	root->addWidget(m_toastLabel);

	m_stack = new QStackedWidget();

	// --- Home ---
	auto* homePage = new QWidget();
	auto* homeOuter = new QVBoxLayout(homePage);
	homeOuter->setContentsMargins(0, 0, 0, 0);
	auto* homeScroll = new QScrollArea();
	homeScroll->setWidgetResizable(true);
	homeScroll->setFrameShape(QFrame::NoFrame);
	auto* homeInner = new QWidget();
	auto* homeLayout = new QVBoxLayout(homeInner);
	homeLayout->setSpacing(kGroupSpacing);

	auto* cards = new QVBoxLayout();
	cards->setSpacing(6);
	auto* btnHost = MakeModeCard(
		QStringLiteral("Host"),
		QStringLiteral("Create a room and share a code"),
		QStringLiteral("modeCardHost"));
	auto* btnJoin = MakeModeCard(
		QStringLiteral("Join"),
		QStringLiteral("Paste a room code from your host"),
		QStringLiteral("modeCardJoin"));
	auto* btnOffline = MakeModeCard(
		QStringLiteral("Offline"),
		QStringLiteral("Launch without netplay"),
		QStringLiteral("modeCardOffline"));
	cards->addWidget(btnHost);
	cards->addWidget(btnJoin);
	cards->addWidget(btnOffline);
	homeLayout->addLayout(cards);

	// Matchmaking and the room browser are built but not ready to ship, so the
	// entry points stay hidden. The Rooms screen, its list widget and the
	// broker refresh plumbing are all still wired up below — re-showing this
	// panel is all that is needed to bring the feature back.
	m_homeAdvancedPanel = new QGroupBox(QStringLiteral("Quick actions"));
	auto* quickLayout = new QVBoxLayout(m_homeAdvancedPanel);
	m_btnFindMatch = new QPushButton(QStringLiteral("Find match"));
	m_btnFindMatch->setObjectName(QStringLiteral("secondaryButton"));
	m_btnBrowseRooms = new QPushButton(QStringLiteral("Open rooms"));
	m_btnBrowseRooms->setObjectName(QStringLiteral("secondaryButton"));
	quickLayout->addWidget(m_btnFindMatch);
	quickLayout->addWidget(m_btnBrowseRooms);
	homeLayout->addWidget(m_homeAdvancedPanel);
	m_homeAdvancedPanel->hide();

	auto* updateGroup = new QGroupBox(QStringLiteral("Updates"));
	updateGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	auto* updateLayout = new QVBoxLayout(updateGroup);
	updateLayout->setContentsMargins(6, 6, 6, 6);
	updateLayout->setSpacing(4);
	m_updateStatus = new QLabel();
	m_updateStatus->setObjectName(QStringLiteral("updateStatus"));
	m_updateStatus->setWordWrap(true);
	m_updateStatus->setMaximumHeight(72);
	m_updateStatus->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	m_updateStatus->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	m_updateStatus->hide();
	auto* updateButtons = new QHBoxLayout();
	m_btnCheckUpdate = new QPushButton(QStringLiteral("Check for updates"));
	m_btnCheckUpdate->setObjectName(QStringLiteral("secondaryButton"));
	m_btnInstallUpdate = new QPushButton(QStringLiteral("Install update"));
	m_btnInstallUpdate->setObjectName(QStringLiteral("primaryButton"));
	m_btnInstallUpdate->hide();
	m_btnOpenRelease = new QPushButton(QStringLiteral("Open release page"));
	m_btnOpenRelease->setObjectName(QStringLiteral("secondaryButton"));
	m_btnOpenRelease->hide();
	updateButtons->addWidget(m_btnCheckUpdate);
	updateButtons->addWidget(m_btnInstallUpdate);
	updateButtons->addWidget(m_btnOpenRelease);
	updateButtons->addStretch();
	updateLayout->addWidget(m_updateStatus);
	updateLayout->addLayout(updateButtons);
	homeLayout->addWidget(updateGroup);
	homeLayout->addStretch(1);

	homeScroll->setWidget(homeInner);
	homeOuter->addWidget(homeScroll, 1);

	QObject::connect(btnHost, &QPushButton::clicked, this, &RelayNetplayWindow::onGoHost);
	QObject::connect(btnJoin, &QPushButton::clicked, this, &RelayNetplayWindow::onGoJoin);
	QObject::connect(btnOffline, &QPushButton::clicked, this, &RelayNetplayWindow::onGoOffline);

	m_stack->addWidget(homePage);

	// --- Host ---
	auto* hostPage = new QWidget();
	auto* hostOuter = new QVBoxLayout(hostPage);
	hostOuter->setContentsMargins(0, 0, 0, 0);
	hostOuter->setSpacing(kGroupSpacing);
	auto* hostBack = MakeBackButton();
	hostOuter->addWidget(hostBack, 0, Qt::AlignLeft);
	QObject::connect(hostBack, &QPushButton::clicked, this, &RelayNetplayWindow::onGoHome);

	m_hostScroll = new QScrollArea();
	m_hostScroll->setWidgetResizable(true);
	m_hostScroll->setFrameShape(QFrame::NoFrame);
	m_hostScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	m_hostScroll->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	auto* hostInner = new QWidget();
	auto* hostLayout = new QVBoxLayout(hostInner);
	hostLayout->setContentsMargins(0, 0, 0, 0);
	hostLayout->setSpacing(kGroupSpacing);

	// Above the share cards on purpose: this choice decides which of those
	// cards are relevant, so burying it below them reads backwards.
	m_hostConnectMethod = new QComboBox();
	m_hostConnectMethod->addItem(QStringLiteral("Relay room code"), QStringLiteral("relay"));
	m_hostConnectMethod->addItem(QStringLiteral("Direct IP"), QStringLiteral("direct"));
	m_hostConnectMethod->addItem(QStringLiteral("Direct + UPnP"), QStringLiteral("autoNat"));
	m_hostConnectMethod->setToolTip(QStringLiteral(
		"Relay room code works behind most routers. Direct IP needs a forwarded "
		"port. Direct + UPnP asks the router to forward it for you."));
	ConfigureFormField(m_hostConnectMethod);
	m_hostConnectMethodRow = new QGroupBox(QStringLiteral("Connection"));
	m_hostConnectMethodRow->setProperty("advancedOnly", true);
	auto* hostMethodLayout = new QFormLayout(m_hostConnectMethodRow);
	hostMethodLayout->setContentsMargins(kGroupPadding, kGroupPadding, kGroupPadding, kGroupPadding);
	hostMethodLayout->setHorizontalSpacing(kLabelGap);
	hostMethodLayout->setVerticalSpacing(kRowSpacing);
	hostMethodLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	hostMethodLayout->addRow(QStringLiteral("Method"), m_hostConnectMethod);
	hostLayout->addWidget(m_hostConnectMethodRow);

	auto* shareGroup = new QGroupBox(QStringLiteral("Share with opponent"));
	auto* shareLayout = new QVBoxLayout(shareGroup);
	shareLayout->setContentsMargins(kGroupPadding, kGroupPadding, kGroupPadding, kGroupPadding);
	shareLayout->setSpacing(kRowSpacing);
	auto* shareGrid = new QGridLayout();
	shareGrid->setSpacing(kRowSpacing);
	auto* relayCard = MakeShareCard(QStringLiteral("relay"), &m_shareRelayValue, &m_btnCopyRelay);
	m_hostShareLan = MakeShareCard(QStringLiteral("lan"), &m_shareLanValue, &m_btnCopyLan);
	m_hostShareWan = MakeShareCard(QStringLiteral("wan"), &m_shareWanValue, &m_btnCopyWan);
	m_btnCreateRoom = new QPushButton(QStringLiteral("Get code"));
	m_btnCreateRoom->setObjectName(QStringLiteral("secondaryButton"));
	m_btnCreateRoom->setMinimumWidth(88);
	// Wrapped in a widget so the whole row can be hidden when the host is not
	// using relay: a bare layout has no setVisible().
	m_hostShareRelayRow = new QWidget();
	auto* relayRow = new QHBoxLayout(m_hostShareRelayRow);
	relayRow->setContentsMargins(0, 0, 0, 0);
	relayRow->setSpacing(6);
	relayRow->addWidget(relayCard, 1);
	relayRow->addWidget(m_btnCreateRoom, 0, Qt::AlignTop);
	shareGrid->addWidget(m_hostShareRelayRow, 0, 0, 1, 2);
	shareGrid->addWidget(m_hostShareLan, 1, 0);
	shareGrid->addWidget(m_hostShareWan, 1, 1);
	shareLayout->addLayout(shareGrid);

	auto* shareActions = new QWidget();
	auto* shareActionsLayout = new QHBoxLayout(shareActions);
	shareActionsLayout->setContentsMargins(0, 0, 0, 0);
	shareActionsLayout->setSpacing(6);
	m_btnRefreshIp = new QPushButton(QStringLiteral("Refresh public IP"));
	m_btnRefreshIp->setObjectName(QStringLiteral("secondaryButton"));
	m_btnTryUpnp = new QPushButton(QStringLiteral("Try UPnP"));
	m_btnTryUpnp->setObjectName(QStringLiteral("secondaryButton"));
	shareActionsLayout->addWidget(m_btnRefreshIp);
	shareActionsLayout->addWidget(m_btnTryUpnp);
	shareActionsLayout->addStretch();
	m_hostShareActions = shareActions;
	shareLayout->addWidget(shareActions);
	m_hostShareHint = new QLabel(
		QStringLiteral("Get a relay code, share it, then Start game."));
	m_hostShareHint->setObjectName(QStringLiteral("hint"));
	m_hostShareHint->setWordWrap(true);
	shareLayout->addWidget(m_hostShareHint);
	hostLayout->addWidget(shareGroup);

	// Always visible in both UI modes: advanced only adds fields, it never
	// swaps these out, so there is exactly one widget per setting.
	m_hostSimpleSettings = new QGroupBox(QStringLiteral("Host settings"));
	auto* hostSimpleLayout = new QFormLayout(m_hostSimpleSettings);
	hostSimpleLayout->setContentsMargins(kGroupPadding, kGroupPadding, kGroupPadding, kGroupPadding);
	hostSimpleLayout->setHorizontalSpacing(kLabelGap);
	hostSimpleLayout->setVerticalSpacing(kRowSpacing);
	hostSimpleLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	m_hostNameSimple = new QLineEdit();
	m_hostNameSimple->setPlaceholderText(QStringLiteral("Display name"));
	ConfigureFormField(m_hostNameSimple);
	m_hostDelaySimple = new QSpinBox();
	m_hostDelaySimple->setRange(1, 20);
	m_hostDelaySimple->setValue(2);
	m_hostDelaySimple->setToolTip(QStringLiteral(
		"Frames of input delay. Higher hides more lag but feels less responsive; "
		"2 suits most connections."));
	hostSimpleLayout->addRow(QStringLiteral("Display name"), m_hostNameSimple);
	hostSimpleLayout->addRow(QStringLiteral("Input delay"), BuildStepper(m_hostDelaySimple));
	hostLayout->addWidget(m_hostSimpleSettings);

	m_hostAdvancedSettings = new QGroupBox(QStringLiteral("Advanced host settings"));
	m_hostAdvancedSettings->setProperty("advancedOnly", true);
	auto* hostAdvLayout = new QFormLayout(m_hostAdvancedSettings);
	hostAdvLayout->setContentsMargins(kGroupPadding, kGroupPadding, kGroupPadding, kGroupPadding);
	hostAdvLayout->setHorizontalSpacing(kLabelGap);
	hostAdvLayout->setVerticalSpacing(kRowSpacing);
	hostAdvLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	m_hostPort = new QSpinBox();
	m_hostPort->setRange(1024, 65535);
	m_hostPort->setValue(23456);
	m_hostPort->setToolTip(QStringLiteral(
		"UDP port this machine listens on. Only matters for Direct IP; forward "
		"it on your router."));
	// Port needs room for 5 digits, so it gets a wider value box than the
	// delay stepper but the same styled -/+ buttons instead of Qt's native
	// arrows, which render as an unusable sliver at this row height.
	QWidget* hostPortStepper = BuildStepper(m_hostPort);
	m_hostPort->setMinimumWidth(72);
	m_hostPort->setMaximumWidth(88);
	m_hostAdvertise = new QLineEdit();
	m_hostAdvertise->setPlaceholderText(QStringLiteral("Public IP (auto-detect or enter)"));
	m_hostAdvertise->setToolTip(QStringLiteral(
		"The address your opponent connects to. Detected automatically; override "
		"only if detection picks the wrong one."));
	ConfigureFormField(m_hostAdvertise);
	m_hostNatStatus = new QLabel(QStringLiteral("NAT: —"));
	m_hostNatStatus->setObjectName(QStringLiteral("hint"));
	m_brokerUrl = new QLineEdit();
	m_brokerUrl->setPlaceholderText(QStringLiteral("Default room broker"));
	m_brokerUrl->setToolTip(QStringLiteral(
		"Server that hands out relay room codes. Leave blank unless you run "
		"your own."));
	ConfigureFormField(m_brokerUrl);
	hostAdvLayout->addRow(QStringLiteral("Session port"), hostPortStepper);
	hostAdvLayout->addRow(QStringLiteral("Internet address"), m_hostAdvertise);
	hostAdvLayout->addRow(QString(), m_hostNatStatus);
	hostAdvLayout->addRow(QStringLiteral("Room broker URL"), m_brokerUrl);
	hostLayout->addWidget(m_hostAdvancedSettings);

	// Each box should be exactly as tall as its contents. Without this the
	// spare height is divided among them and short boxes (Connection, with a
	// single row) stretch into a large empty void.
	for (QWidget* box : { m_hostConnectMethodRow, static_cast<QWidget*>(shareGroup), m_hostSimpleSettings, m_hostAdvancedSettings }) {
		if (box) {
			box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		}
	}
	// Each QFormLayout sizes its label column from its own longest label, so
	// without a shared floor the labels start at a different x in every box.
	// One width across all of them gives the page a single alignment edge.
	for (QFormLayout* form : { hostMethodLayout, hostSimpleLayout, hostAdvLayout }) {
		for (int i = 0; i < form->rowCount(); ++i) {
			if (QLayoutItem* labelItem = form->itemAt(i, QFormLayout::LabelRole)) {
				if (QWidget* labelWidget = labelItem->widget()) {
					labelWidget->setMinimumWidth(kFormLabelWidth);
				}
			}
		}
	}
	// Absorb the remainder at the bottom so the boxes stay top-aligned.
	hostLayout->addStretch(1);

	m_hostScroll->setWidget(hostInner);
	// Stretch 1 with no trailing spacer: the scroll area must absorb the spare
	// height, otherwise it collapses to its minimum and the settings below the
	// share card become unreachable.
	hostOuter->addWidget(m_hostScroll, 1);

	m_btnStartHost = new QPushButton(QStringLiteral("Start game"));
	m_btnStartHost->setObjectName(QStringLiteral("primaryButton"));
	hostOuter->addWidget(m_btnStartHost);
	m_stack->addWidget(hostPage);

	// --- Join ---
	auto* joinPage = new QWidget();
	auto* joinOuter = new QVBoxLayout(joinPage);
	auto* joinBack = MakeBackButton();
	joinOuter->addWidget(joinBack, 0, Qt::AlignLeft);
	QObject::connect(joinBack, &QPushButton::clicked, this, &RelayNetplayWindow::onGoHome);

	auto* joinScroll = new QScrollArea();
	joinScroll->setWidgetResizable(true);
	joinScroll->setFrameShape(QFrame::NoFrame);
	auto* joinInner = new QWidget();
	auto* joinLayout = new QVBoxLayout(joinInner);
	joinLayout->setSpacing(kGroupSpacing);

	m_joinSimpleSettings = new QGroupBox(QStringLiteral("Join settings"));
	auto* joinSimpleLayout = new QFormLayout(m_joinSimpleSettings);
	joinSimpleLayout->setContentsMargins(kGroupPadding, kGroupPadding, kGroupPadding, kGroupPadding);
	joinSimpleLayout->setHorizontalSpacing(kLabelGap);
	joinSimpleLayout->setVerticalSpacing(kRowSpacing);
	joinSimpleLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	m_joinNameSimple = new QLineEdit();
	m_joinNameSimple->setPlaceholderText(QStringLiteral("Display name"));
	ConfigureFormField(m_joinNameSimple);
	m_joinDelaySimple = new QSpinBox();
	m_joinDelaySimple->setRange(1, 20);
	m_joinDelaySimple->setValue(2);
	m_joinDelaySimple->setToolTip(QStringLiteral(
		"Frames of input delay. Higher hides more lag but feels less responsive; "
		"2 suits most connections."));
	m_joinRoomCode = new QLineEdit();
	m_joinRoomCode->setObjectName(QStringLiteral("roomCodeInput"));
	m_joinRoomCode->setPlaceholderText(QStringLiteral("SF4-XXXX or 203.0.113.42:23456"));
	m_joinRoomCode->setToolTip(QStringLiteral(
		"Paste whatever the host shared: a relay room code, or an address and "
		"port for a direct connection."));
	ConfigureFormField(m_joinRoomCode);
	m_joinRoomCode->setMinimumHeight(30);
	// Pasting is how a code actually arrives (Discord, chat), so give it a
	// first-class button instead of relying on Ctrl+V.
	m_btnPasteRoomCode = new QPushButton(QStringLiteral("Paste"));
	m_btnPasteRoomCode->setObjectName(QStringLiteral("secondaryButton"));
	m_btnPasteRoomCode->setToolTip(QStringLiteral("Paste a room code or address from the clipboard."));
	m_btnPasteRoomCode->setProperty("copied", false);
	m_btnPasteRoomCode->setCursor(Qt::PointingHandCursor);
	m_btnPasteRoomCode->setMaximumWidth(72);
	auto* joinCodeRow = new QWidget();
	auto* joinCodeLayout = new QHBoxLayout(joinCodeRow);
	joinCodeLayout->setContentsMargins(0, 0, 0, 0);
	joinCodeLayout->setSpacing(6);
	joinCodeLayout->addWidget(m_joinRoomCode, 1);
	joinCodeLayout->addWidget(m_btnPasteRoomCode, 0);

	joinSimpleLayout->addRow(QStringLiteral("Display name"), m_joinNameSimple);
	joinSimpleLayout->addRow(QStringLiteral("Input delay"), BuildStepper(m_joinDelaySimple));
	joinSimpleLayout->addRow(QStringLiteral("Room code or IP:port"), joinCodeRow);
	joinLayout->addWidget(m_joinSimpleSettings);

	// Advanced only adds the connection-method override; the name, delay and
	// address fields above stay put so there is one widget per setting.
	m_joinAdvancedSettings = new QGroupBox(QStringLiteral("Advanced join settings"));
	m_joinAdvancedSettings->setProperty("advancedOnly", true);
	auto* joinAdvLayout = new QFormLayout(m_joinAdvancedSettings);
	joinAdvLayout->setContentsMargins(kGroupPadding, kGroupPadding, kGroupPadding, kGroupPadding);
	joinAdvLayout->setHorizontalSpacing(kLabelGap);
	joinAdvLayout->setVerticalSpacing(kRowSpacing);
	joinAdvLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	m_joinConnectMethod = new QComboBox();
	m_joinConnectMethod->addItem(QStringLiteral("Auto-detect"), QStringLiteral("auto"));
	m_joinConnectMethod->addItem(QStringLiteral("Relay room code"), QStringLiteral("relay"));
	m_joinConnectMethod->addItem(QStringLiteral("Direct IP:port"), QStringLiteral("direct"));
	m_joinConnectMethod->setToolTip(QStringLiteral(
		"Auto-detect picks relay or direct from what you typed above. Override "
		"only if you need to force one."));
	ConfigureFormField(m_joinConnectMethod);
	joinAdvLayout->addRow(QStringLiteral("Connection method"), m_joinConnectMethod);
	joinLayout->addWidget(m_joinAdvancedSettings);

	// Same as the host page: content-height boxes, slack at the bottom.
	for (QWidget* box : { m_joinSimpleSettings, m_joinAdvancedSettings }) {
		if (box) {
			box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		}
	}
	joinLayout->addStretch(1);

	joinScroll->setWidget(joinInner);
	joinOuter->addWidget(joinScroll, 1);

	m_btnStartJoin = new QPushButton(QStringLiteral("Start game"));
	m_btnStartJoin->setObjectName(QStringLiteral("primaryButton"));
	joinOuter->addWidget(m_btnStartJoin);
	m_stack->addWidget(joinPage);

	// --- Rooms ---
	auto* roomsPage = new QWidget();
	auto* roomsOuter = new QVBoxLayout(roomsPage);
	auto* roomsBack = MakeBackButton();
	roomsOuter->addWidget(roomsBack, 0, Qt::AlignLeft);
	QObject::connect(roomsBack, &QPushButton::clicked, this, &RelayNetplayWindow::onGoHome);
	auto* roomsHeader = new QHBoxLayout();
	auto* roomsTitle = new QLabel(QStringLiteral("Open rooms"));
	roomsTitle->setObjectName(QStringLiteral("modeCardTitle"));
	auto* btnRefreshRooms = new QPushButton(QStringLiteral("Refresh"));
	btnRefreshRooms->setObjectName(QStringLiteral("secondaryButton"));
	roomsHeader->addWidget(roomsTitle);
	roomsHeader->addStretch();
	roomsHeader->addWidget(btnRefreshRooms);
	roomsOuter->addLayout(roomsHeader);
	m_roomList = new QListWidget();
	m_roomListEmpty = new QLabel(QStringLiteral("No open rooms. Ask a friend to host or create one."));
	m_roomListEmpty->setObjectName(QStringLiteral("hint"));
	m_roomListEmpty->setWordWrap(true);
	m_roomListEmpty->setAlignment(Qt::AlignCenter);
	roomsOuter->addWidget(m_roomList, 1);
	roomsOuter->addWidget(m_roomListEmpty);
	QObject::connect(btnRefreshRooms, &QPushButton::clicked, this, &RelayNetplayWindow::onRefreshRooms);
	m_stack->addWidget(roomsPage);

	// --- Offline ---
	auto* offlinePage = new QWidget();
	auto* offlineOuter = new QVBoxLayout(offlinePage);
	auto* offlineBack = MakeBackButton();
	offlineOuter->addWidget(offlineBack, 0, Qt::AlignLeft);
	QObject::connect(offlineBack, &QPushButton::clicked, this, &RelayNetplayWindow::onGoHome);
	auto* offlineForm = new QGroupBox(QStringLiteral("Offline"));
	auto* offlineLayout = new QVBoxLayout(offlineForm);
	m_offlineName = new QLineEdit();
	m_offlineName->setPlaceholderText(QStringLiteral("Display name"));
	ConfigureFormField(m_offlineName);
	m_offlineDelay = new QSpinBox();
	m_offlineDelay->setRange(1, 20);
	m_offlineDelay->setValue(2);
	offlineLayout->addWidget(new QLabel(QStringLiteral("Display name")));
	offlineLayout->addWidget(m_offlineName);
	offlineLayout->addWidget(new QLabel(QStringLiteral("Input delay (frames)")));
	offlineLayout->addWidget(BuildStepper(m_offlineDelay));
	offlineOuter->addWidget(offlineForm);
	auto* btnStartOffline = new QPushButton(QStringLiteral("Start game"));
	btnStartOffline->setObjectName(QStringLiteral("primaryButton"));
	offlineOuter->addWidget(btnStartOffline);
	offlineOuter->addStretch();
	QObject::connect(btnStartOffline, &QPushButton::clicked, this, &RelayNetplayWindow::onStartOffline);
	m_stack->addWidget(offlinePage);

	root->addWidget(m_stack, 1);

	m_btnToggleLog = new QPushButton(QStringLiteral("Show log"));
	m_btnToggleLog->setObjectName(QStringLiteral("secondaryButton"));
	root->addWidget(m_btnToggleLog);

	m_logPanel = new QWidget();
	auto* logLayout = new QVBoxLayout(m_logPanel);
	logLayout->setContentsMargins(0, 0, 0, 0);
	auto* logHeader = new QHBoxLayout();
	logHeader->addWidget(new QLabel(QStringLiteral("Launcher log")));
	logHeader->addStretch();
	auto* clearLog = new QPushButton(QStringLiteral("Clear"));
	clearLog->setObjectName(QStringLiteral("secondaryButton"));
	logHeader->addWidget(clearLog);
	logLayout->addLayout(logHeader);
	m_log = new QPlainTextEdit();
	m_log->setReadOnly(true);
	m_log->setMaximumBlockCount(500);
	logLayout->addWidget(m_log);
	m_logPanel->setVisible(false);
	root->addWidget(m_logPanel);
	QObject::connect(clearLog, &QPushButton::clicked, this, &RelayNetplayWindow::onClearLog);
}

void RelayNetplayWindow::applyTheme() {
	setStyleSheet(LauncherStylesheet());
}

void RelayNetplayWindow::wireSignals() {
	QObject::connect(m_btnModeSimple, &QPushButton::clicked, this, &RelayNetplayWindow::onModeSimple);
	QObject::connect(m_btnModeAdvanced, &QPushButton::clicked, this, &RelayNetplayWindow::onModeAdvanced);
	QObject::connect(m_btnCreateRoom, &QPushButton::clicked, this, &RelayNetplayWindow::onCreateRelayRoom);
	QObject::connect(m_btnRefreshIp, &QPushButton::clicked, this, &RelayNetplayWindow::onRefreshPublicIp);
	QObject::connect(m_btnTryUpnp, &QPushButton::clicked, this, &RelayNetplayWindow::onTryUpnp);
	QObject::connect(m_btnCopyRelay, &QPushButton::clicked, this, &RelayNetplayWindow::onCopyShare);
	QObject::connect(m_btnCopyLan, &QPushButton::clicked, this, &RelayNetplayWindow::onCopyShare);
	QObject::connect(m_btnCopyWan, &QPushButton::clicked, this, &RelayNetplayWindow::onCopyShare);
	QObject::connect(m_btnStartHost, &QPushButton::clicked, this, &RelayNetplayWindow::onStartHost);
	QObject::connect(m_btnStartJoin, &QPushButton::clicked, this, &RelayNetplayWindow::onStartJoin);
	QObject::connect(m_btnPasteRoomCode, &QPushButton::clicked, this, &RelayNetplayWindow::onPasteRoomCode);
	// Normalise once the user finishes editing rather than on every keystroke,
	// so uppercasing never fights the cursor mid-type.
	QObject::connect(m_joinRoomCode, &QLineEdit::editingFinished, this, [this]() {
		if (!m_joinRoomCode) {
			return;
		}
		const QString normalized = NormalizeRoomCodeText(m_joinRoomCode->text());
		if (normalized != m_joinRoomCode->text()) {
			m_joinRoomCode->setText(normalized);
		}
	});
	QObject::connect(m_btnFindMatch, &QPushButton::clicked, this, &RelayNetplayWindow::onFindMatch);
	QObject::connect(m_btnBrowseRooms, &QPushButton::clicked, this, &RelayNetplayWindow::onBrowseRooms);
	QObject::connect(m_btnCheckUpdate, &QPushButton::clicked, this, &RelayNetplayWindow::onCheckUpdate);
	QObject::connect(m_btnInstallUpdate, &QPushButton::clicked, this, &RelayNetplayWindow::onApplyUpdate);
	QObject::connect(m_btnOpenRelease, &QPushButton::clicked, this, &RelayNetplayWindow::onOpenRelease);
	QObject::connect(m_btnToggleLog, &QPushButton::clicked, this, &RelayNetplayWindow::onToggleLog);
	QObject::connect(m_hostAdvertise, &QLineEdit::textChanged, this, &RelayNetplayWindow::onAdvertiseChanged);
	QObject::connect(m_hostPort, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
		onAdvertiseChanged();
	});
	QObject::connect(m_hostConnectMethod, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		// Relay requires a broker room code, while Direct IP does not. Re-evaluate
		// the Start button immediately when the user changes the transport.
		renderHostShareCards();
	});
	QObject::connect(m_brokerUrl, &QLineEdit::editingFinished, this, &RelayNetplayWindow::onBrokerChanged);
	QObject::connect(m_roomList, &QListWidget::itemClicked, this, &RelayNetplayWindow::onRoomSelected);

	// One sweep instead of a setCursor call per button: every clickable button
	// should read as clickable, and this cannot drift as buttons are added.
	const QList<QPushButton*> buttons = findChildren<QPushButton*>();
	for (QPushButton* btn : buttons) {
		btn->setCursor(Qt::PointingHandCursor);
	}
}

void RelayNetplayWindow::showScreen(Screen screen) {
	if (!m_stack) {
		return;
	}
	m_stack->setCurrentIndex(static_cast<int>(screen));
	if (m_warningBanner) {
		m_warningBanner->setVisible(screen == Screen::Home);
	}
	if (m_subtitleLabel) {
		m_subtitleLabel->setVisible(screen == Screen::Home);
	}
}

void RelayNetplayWindow::applyUiMode(bool fromUser) {
	// Nothing to reconcile between modes: each setting has exactly one widget,
	// and Advanced only reveals additional ones.
	m_btnModeSimple->setChecked(m_simpleUi);
	m_btnModeAdvanced->setChecked(!m_simpleUi);
	setAdvancedVisible(!m_simpleUi);
	if (fromUser) {
		m_bridge.post({ { "type", "setUiMode" }, { "simpleUi", m_simpleUi } });
	}
	renderHostShareCards();
}

void RelayNetplayWindow::setAdvancedVisible(bool advanced) {
	// Deliberately not driven by the advanced toggle: matchmaking and the room
	// browser are deferred, so this panel stays hidden in both modes.
	// The basics boxes stay visible in both modes; advanced is purely additive.
	if (m_hostConnectMethodRow) {
		m_hostConnectMethodRow->setVisible(advanced);
	}
	if (m_hostAdvancedSettings) {
		m_hostAdvancedSettings->setVisible(advanced);
	}
	if (m_joinAdvancedSettings) {
		m_joinAdvancedSettings->setVisible(advanced);
	}
	if (m_btnTryUpnp) {
		m_btnTryUpnp->setVisible(advanced);
	}
	if (m_btnRefreshIp) {
		m_btnRefreshIp->setVisible(advanced);
	}
	if (m_hostShareActions) {
		m_hostShareActions->setVisible(advanced);
	}
	if (m_hostShareLan) {
		m_hostShareLan->setVisible(advanced);
	}
	if (m_hostShareWan) {
		m_hostShareWan->setVisible(advanced);
	}
	if (m_btnToggleLog) {
		m_btnToggleLog->setVisible(advanced);
	}
	if (m_logPanel && !advanced) {
		m_logPanel->setVisible(false);
		if (m_btnToggleLog) {
			m_btnToggleLog->setText(QStringLiteral("Show log"));
		}
	}
}

void RelayNetplayWindow::appendLog(const QString& text) {
	if (m_log) {
		m_log->appendPlainText(text);
	}
}

void RelayNetplayWindow::setStatus(const QString& text, const QString& kind) {
	if (!m_statusStrip) {
		return;
	}
	if (text.isEmpty()) {
		m_statusStrip->setText(QStringLiteral("Ready"));
		m_statusStrip->setProperty("statusKind", QString());
	}
	else {
		m_statusStrip->setText(text);
		m_statusStrip->setProperty("statusKind", kind);
	}
	if (m_statusStrip->style()) {
		m_statusStrip->style()->unpolish(m_statusStrip);
		m_statusStrip->style()->polish(m_statusStrip);
	}
}

void RelayNetplayWindow::showToast(const QString& text, const QString& kind) {
	if (!m_toastLabel || text.isEmpty()) {
		return;
	}
	m_toastLabel->setText(text);
	m_toastLabel->setProperty("toastKind", kind);
	m_toastLabel->style()->unpolish(m_toastLabel);
	m_toastLabel->style()->polish(m_toastLabel);
	m_toastLabel->show();
	m_toastTimer.start(3500);
}

QString RelayNetplayWindow::activeRelayRoomCode() const {
	if (IsShortRoomCodeQString(m_sessionRelayCode)) {
		return m_sessionRelayCode.trimmed();
	}
	return QString();
}

QString RelayNetplayWindow::getDisplayName(bool hostScreen) const {
	const QLineEdit* field = hostScreen ? m_hostNameSimple : m_joinNameSimple;
	const QString name = field ? field->text().trimmed() : QString();
	return name.isEmpty() ? QStringLiteral("Player") : name;
}

int RelayNetplayWindow::getInputDelay(bool hostScreen) const {
	const QSpinBox* box = hostScreen ? m_hostDelaySimple : m_joinDelaySimple;
	return box ? box->value() : 2;
}

QString RelayNetplayWindow::getHostConnectMethod() const {
	if (m_simpleUi) {
		return QStringLiteral("relay");
	}
	return m_hostConnectMethod ? m_hostConnectMethod->currentData().toString() : QStringLiteral("relay");
}

QString RelayNetplayWindow::getJoinConnectMethod(const QString& trimmedCode) const {
	// Simple mode always infers from the entered text. Advanced exposes the
	// same inference as "auto" plus explicit overrides.
	const QString selected =
		(!m_simpleUi && m_joinConnectMethod)
			? m_joinConnectMethod->currentData().toString()
			: QStringLiteral("auto");
	if (selected == QStringLiteral("auto")) {
		return IsShortRoomCodeQString(trimmedCode) ? QStringLiteral("relay") : QStringLiteral("direct");
	}
	return selected;
}

void RelayNetplayWindow::setShareCard(const QString& id, const QString& value) {
	QLabel* label = nullptr;
	QPushButton* copy = nullptr;
	QWidget* card = nullptr;
	if (id == QStringLiteral("relay")) {
		label = m_shareRelayValue;
		copy = m_btnCopyRelay;
	}
	else if (id == QStringLiteral("lan")) {
		label = m_shareLanValue;
		copy = m_btnCopyLan;
		card = m_hostShareLan;
	}
	else if (id == QStringLiteral("wan")) {
		label = m_shareWanValue;
		copy = m_btnCopyWan;
		card = m_hostShareWan;
	}
	if (label) {
		label->setText(value.isEmpty() ? QStringLiteral("-") : value);
	}
	if (copy) {
		copy->setEnabled(!value.isEmpty());
	}
	if (card && card->style()) {
		card->setProperty("shareEmpty", value.isEmpty());
		card->style()->unpolish(card);
		card->style()->polish(card);
	}
	if (id == QStringLiteral("relay")) {
		m_shareRelay = value;
	}
	else if (id == QStringLiteral("lan")) {
		m_shareLan = value;
	}
	else if (id == QStringLiteral("wan")) {
		m_shareWan = value;
	}
}

void RelayNetplayWindow::renderHostShareCards() {
	const int port = m_state.value("sessionPort", 23456);
	const QString lanAddr = JsonString(m_state, "lanAddress");
	const QString advertiseHost =
		m_hostAdvertise && !m_hostAdvertise->text().trimmed().isEmpty()
		? m_hostAdvertise->text().trimmed()
		: JsonString(m_state, "advertiseHost");
	QString wanAddr = JsonString(m_state, "wanAddress");
	if (wanAddr.isEmpty() && !advertiseHost.isEmpty()) {
		wanAddr = advertiseHost + QStringLiteral(":") + QString::number(port);
	}

	setShareCard(QStringLiteral("relay"), activeRelayRoomCode());

	// Show only what the chosen method actually needs: a room code for relay,
	// an IP:port for direct. Simple mode is always relay.
	const bool usingRelay = getHostConnectMethod() == QStringLiteral("relay");
	if (m_hostShareRelayRow) {
		m_hostShareRelayRow->setVisible(usingRelay);
	}

	const bool showDirectShare = !m_simpleUi && !usingRelay;
	if (m_hostShareLan) {
		m_hostShareLan->setVisible(showDirectShare);
	}
	if (m_hostShareWan) {
		m_hostShareWan->setVisible(showDirectShare);
	}
	if (m_btnRefreshIp) {
		m_btnRefreshIp->setVisible(showDirectShare);
	}
	if (m_hostShareActions) {
		m_hostShareActions->setVisible(showDirectShare);
	}

	setShareCard(QStringLiteral("lan"), showDirectShare ? lanAddr : QString());
	setShareCard(QStringLiteral("wan"), showDirectShare ? wanAddr : QString());

	if (m_hostShareHint) {
		const QString relayCode = activeRelayRoomCode();
		const QString method = getHostConnectMethod();
		if (method != QStringLiteral("relay")) {
			m_hostShareHint->setText(QStringLiteral(
				"Direct IP does not use a room code. Share the LAN or Internet IP:port and start the host first."));
		}
		else if (!relayCode.isEmpty()) {
			m_hostShareHint->setText(
				m_forceVpsRelay
					? QStringLiteral(
						  "Share the relay code. Click Start game — joiners paste SF4-XXXX (no port forward on your PC).")
					: QStringLiteral(
						  "Share the relay code. Click Start game first, then have your opponent join."));
		}
		else if (m_simpleUi && m_forceVpsRelay) {
			m_hostShareHint->setText(QStringLiteral("Click Get code to create a relay room, then share it with your opponent."));
		}
		else {
			m_hostShareHint->setText(
				QStringLiteral("Create a relay room or refresh public IP to get shareable addresses."));
		}
	}
	updateHostControls();
}

void RelayNetplayWindow::updateHostControls() {
	const bool hasCode = !activeRelayRoomCode().isEmpty();
	const QString method = getHostConnectMethod();
	const bool needsRelayCode = method == QStringLiteral("relay");
	if (m_btnStartHost) {
		m_btnStartHost->setEnabled(!needsRelayCode || hasCode);
	}
}

void RelayNetplayWindow::updateJoinControls() {
	const QString lastJoin = JsonString(m_state, "lastJoinHost");
	if (lastJoin.isEmpty()) {
		return;
	}
	if (m_joinRoomCode && m_joinRoomCode->text().trimmed().isEmpty()) {
		m_joinRoomCode->setText(lastJoin);
	}
}

void RelayNetplayWindow::syncFromState(const nlohmann::json& state) {
	m_state = state;
	if (state.contains("simpleUi")) {
		m_simpleUi = state.value("simpleUi", true);
		applyUiMode(false);
	}
	if (state.contains("displayName")) {
		const QString name = JsonString(state, "displayName");
		if (m_hostNameSimple && m_hostNameSimple->text().isEmpty()) {
			m_hostNameSimple->setText(name);
		}
		if (m_joinNameSimple && m_joinNameSimple->text().isEmpty()) {
			m_joinNameSimple->setText(name);
		}
		if (m_offlineName && (m_offlineName->text().isEmpty() || m_offlineName->text() == QStringLiteral("Offline Tester"))) {
			m_offlineName->setText(name);
		}
	}
	if (state.contains("inputDelay")) {
		const int delay = state.value("inputDelay", 2);
		if (m_hostDelaySimple) {
			m_hostDelaySimple->setValue(delay);
		}
		if (m_joinDelaySimple) {
			m_joinDelaySimple->setValue(delay);
		}
	}
	if (state.contains("sessionPort") && m_hostPort) {
		m_hostPort->setValue(state.value("sessionPort", 23456));
	}
	if (state.contains("advertiseHost") && m_hostAdvertise) {
		const QString adv = JsonString(state, "advertiseHost");
		if (m_hostAdvertise->text().trimmed().isEmpty()) {
			m_hostAdvertise->setText(adv);
		}
	}
	if (state.contains("brokerBaseUrl") && m_brokerUrl) {
		const QString broker = JsonString(state, "brokerBaseUrl");
		if (m_brokerUrl->text().trimmed().isEmpty()) {
			m_brokerUrl->setText(broker);
		}
	}
	if (state.contains("installedVersion")) {
		const QString ver = FormatVersionBadge(JsonString(state, "installedVersion"));
		if (m_versionLabel) {
			m_versionLabel->setText(ver);
		}
	}
	if (state.contains("forceVpsRelay")) {
		m_forceVpsRelay = state.value("forceVpsRelay", false);
	}
	if (state.contains("roomCodePreview")) {
		const QString preview = JsonString(state, "roomCodePreview");
		if (IsShortRoomCodeQString(preview)) {
			m_sessionRelayCode = preview;
		}
	}
	updateJoinControls();
	renderHostShareCards();
}

void RelayNetplayWindow::finalizeRelayRoomResponse(const nlohmann::json& msg) {
	if (msg.contains("roomCodePreview")) {
		const QString preview = JsonString(msg, "roomCodePreview");
		m_sessionRelayCode = IsShortRoomCodeQString(preview) ? preview : QString();
	}
	if (msg.contains("forceVpsRelay")) {
		m_forceVpsRelay = msg.value("forceVpsRelay", false);
	}
	if (msg.contains("connectionStatus")) {
		setStatus(JsonString(msg, "connectionStatus"), QStringLiteral("success"));
	}
	renderHostShareCards();
	startRelayHeartbeat();
	if (m_btnCreateRoom) {
		m_btnCreateRoom->setEnabled(true);
	}
}

void RelayNetplayWindow::applyPartialState(const nlohmann::json& msg) {
	if (msg.contains("advertiseHost") && m_hostAdvertise) {
		m_hostAdvertise->setText(JsonString(msg, "advertiseHost"));
	}
	if (msg.contains("natStatus") && m_hostNatStatus) {
		const QString detail = JsonString(msg, "natDetail");
		const QString status = JsonString(msg, "natStatus");
		m_hostNatStatus->setText(
			detail.isEmpty() ? QStringLiteral("NAT: %1").arg(status)
							 : QStringLiteral("NAT: %1 — %2").arg(status, detail));
	}
	if (msg.contains("roomCodePreview") || msg.contains("relayPort") || msg.contains("relayHost")) {
		finalizeRelayRoomResponse(msg);
	}
	else {
		renderHostShareCards();
	}
	if (m_btnRefreshIp) {
		m_btnRefreshIp->setEnabled(true);
	}
}

void RelayNetplayWindow::renderRoomList(const nlohmann::json& rooms, const QString& listError) {
	if (!m_roomList || !m_roomListEmpty) {
		return;
	}
	m_roomList->clear();
	if (!listError.isEmpty()) {
		m_roomListEmpty->setText(listError);
		m_roomListEmpty->show();
		m_roomList->hide();
		return;
	}
	if (!rooms.is_array() || rooms.empty()) {
		m_roomListEmpty->setText(QStringLiteral("No open rooms. Ask a friend to host or create one."));
		m_roomListEmpty->show();
		m_roomList->hide();
		return;
	}
	m_roomListEmpty->hide();
	m_roomList->show();
	for (const auto& room : rooms) {
		const QString name = JsonString(room, "displayName", QStringLiteral("Host"));
		const QString code = JsonString(room, "code");
		auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(name, code));
		item->setData(Qt::UserRole, code);
		m_roomList->addItem(item);
	}
}

void RelayNetplayWindow::handleMessage(const nlohmann::json& msg) {
	const std::string type = msg.value("type", "");
	if (type == "error") {
		const QString text = JsonString(msg, "message", QStringLiteral("Something went wrong."));
		if (m_updateBusy) {
			m_updateBusy = false;
			if (m_btnCheckUpdate) {
				m_btnCheckUpdate->setEnabled(true);
			}
			if (m_btnInstallUpdate) {
				m_btnInstallUpdate->setEnabled(true);
			}
			if (m_updateStatus) {
				m_updateStatus->setText(text);
				m_updateStatus->setProperty("updateKind", QStringLiteral("error"));
				m_updateStatus->show();
			}
		}
		else {
			showToast(text, QStringLiteral("error"));
		}
		appendLog(text);
		if (m_btnCreateRoom) {
			m_btnCreateRoom->setEnabled(true);
		}
		if (m_btnStartJoin) {
			m_btnStartJoin->setEnabled(true);
		}
		updateHostControls();
		return;
	}

	if (type == "copied") {
		showToast(QStringLiteral("Copied to clipboard"), QStringLiteral("success"));
		return;
	}

	if (type == "updateCheck") {
		m_updateBusy = false;
		if (m_btnCheckUpdate) {
			m_btnCheckUpdate->setEnabled(true);
		}
		if (!msg.value("ok", false)) {
			const QString err = JsonString(msg, "error", QStringLiteral("Update check failed"));
			if (m_updateStatus) {
				m_updateStatus->setText(err);
				m_updateStatus->setProperty("updateKind", QStringLiteral("error"));
				m_updateStatus->show();
			}
			showToast(err, QStringLiteral("error"));
			return;
		}
		m_releaseUrl = JsonString(msg, "releaseUrl");
		m_zipDownloadUrl = JsonString(msg, "zipDownloadUrl");
		m_updateAvailable = msg.value("updateAvailable", false);
		if (m_btnOpenRelease) {
			m_btnOpenRelease->setVisible(false);
		}
		if (m_updateAvailable) {
			const QString notes = SoftenReleaseNotes(JsonString(msg, "releaseNotes"));
			const QString ver = FormatVersionBadge(JsonString(msg, "latestVersion"));
			m_updateLatestVersion = ver;
			if (m_updateStatus) {
				m_updateStatus->setText(
					notes.isEmpty() ? QStringLiteral("Update available: %1").arg(ver)
									: QStringLiteral("Update available: %1\n\n%2").arg(ver, notes));
				m_updateStatus->setProperty("updateKind", QStringLiteral("success"));
				if (m_updateStatus->style()) {
					m_updateStatus->style()->unpolish(m_updateStatus);
					m_updateStatus->style()->polish(m_updateStatus);
				}
				m_updateStatus->show();
			}
			if (m_btnInstallUpdate) {
				m_btnInstallUpdate->show();
			}
		}
		else {
			const QString ver = FormatVersionBadge(
				JsonString(msg, "latestVersion", JsonString(msg, "installedVersion")));
			if (m_updateStatus) {
				m_updateStatus->setText(QStringLiteral("Up to date (%1)").arg(ver));
				m_updateStatus->setProperty("updateKind", QString());
				if (m_updateStatus->style()) {
					m_updateStatus->style()->unpolish(m_updateStatus);
					m_updateStatus->style()->polish(m_updateStatus);
				}
				m_updateStatus->show();
			}
			if (m_btnInstallUpdate) {
				m_btnInstallUpdate->hide();
			}
		}
		return;
	}

	if (type == "updateApply") {
		m_updateBusy = true;
		const QString text = JsonString(msg, "message", QStringLiteral("Installing update..."));
		if (m_updateStatus) {
			m_updateStatus->setText(text);
			m_updateStatus->setProperty("updateKind", QStringLiteral("success"));
			m_updateStatus->show();
		}
		if (m_btnInstallUpdate) {
			m_btnInstallUpdate->hide();
		}
		showToast(text, QStringLiteral("success"));
		return;
	}

	if (type == "state") {
		if (msg.contains("rooms") || msg.contains("listError")) {
			syncFromState(msg);
			renderRoomList(
				msg.contains("rooms") ? msg["rooms"] : nlohmann::json::array(),
				JsonString(msg, "listError"));
			return;
		}
		if (msg.contains("heartbeatOk")) {
			if (msg.value("heartbeatOk", true) == false) {
				setStatus(QStringLiteral("Room connection lost — create a new relay room."), QStringLiteral("error"));
				stopRelayHeartbeat();
				m_sessionRelayCode.clear();
				renderHostShareCards();
			}
			else if (m_btnStartHost) {
				m_btnStartHost->setEnabled(true);
			}
		}
		if (msg.contains("roomCodePreview") || msg.contains("relayPort") || msg.contains("relayHost")) {
			syncFromState(msg);
			finalizeRelayRoomResponse(msg);
			return;
		}
		syncFromState(msg);
		if (msg.contains("connectionStatus")) {
			setStatus(JsonString(msg, "connectionStatus"), QStringLiteral("success"));
		}
		else if (!msg.contains("heartbeatOk")) {
			setStatus(QStringLiteral("Ready"));
		}
		return;
	}

	if (msg.contains("roomCodePreview") || msg.contains("relayPort") || msg.contains("relayHost")
		|| msg.contains("advertiseHost") || msg.contains("natStatus")) {
		applyPartialState(msg);
	}
}

void RelayNetplayWindow::onReply(const nlohmann::json& msg) {
	handleMessage(msg);
}

void RelayNetplayWindow::onLauncherFinished() {
	stopRelayHeartbeat();
	close();
}

void RelayNetplayWindow::onExitForUpdate() {
	stopRelayHeartbeat();
	QApplication::quit();
}

void RelayNetplayWindow::onCreateRelayRoom() {
	if (m_btnCreateRoom) {
		m_btnCreateRoom->setEnabled(false);
	}
	setStatus(QStringLiteral("Creating relay room..."));
	m_bridge.post({
		{ "type", "createRelayRoom" },
		{ "displayName", getDisplayName(true).toStdString() },
	});
}

void RelayNetplayWindow::onCopyShare() {
	auto* btn = qobject_cast<QPushButton*>(sender());
	if (!btn) {
		return;
	}
	copyShareValue(btn->property("shareId").toString());
	flashCopied(btn);
}

// Briefly relabel a copy button so the confirmation appears where the user
// clicked, not just in the corner toast. Re-clicking restarts the timer rather
// than stacking, and the QPointer guard covers the button outliving the timer.
void RelayNetplayWindow::flashCopied(QPushButton* btn, const QString& confirmText) {
	if (!btn) {
		return;
	}
	if (btn->property("copyOriginalText").isNull()) {
		btn->setProperty("copyOriginalText", btn->text());
	}
	btn->setText(confirmText);
	btn->setProperty("copied", true);
	if (btn->style()) {
		btn->style()->unpolish(btn);
		btn->style()->polish(btn);
	}
	QPointer<QPushButton> guarded(btn);
	QTimer::singleShot(1500, this, [guarded]() {
		if (!guarded) {
			return;
		}
		const QVariant original = guarded->property("copyOriginalText");
		if (original.isValid() && !original.isNull()) {
			guarded->setText(original.toString());
		}
		guarded->setProperty("copied", false);
		if (guarded->style()) {
			guarded->style()->unpolish(guarded);
			guarded->style()->polish(guarded);
		}
	});
}

void RelayNetplayWindow::copyShareValue(const QString& id) {
	QString value;
	if (id == QStringLiteral("relay")) {
		value = m_shareRelay;
	}
	else if (id == QStringLiteral("lan")) {
		value = m_shareLan;
	}
	else if (id == QStringLiteral("wan")) {
		value = m_shareWan;
	}
	if (value.isEmpty()) {
		return;
	}
	QApplication::clipboard()->setText(value);
	appendLog(QStringLiteral("Copied: ") + value);
	showToast(QStringLiteral("Copied to clipboard"), QStringLiteral("success"));
}

void RelayNetplayWindow::onRefreshPublicIp() {
	if (m_btnRefreshIp) {
		m_btnRefreshIp->setEnabled(false);
	}
	setStatus(QStringLiteral("Detecting public IP..."));
	m_bridge.post({ { "type", "fetchPublicIp" } });
}

void RelayNetplayWindow::onTryUpnp() {
	setStatus(QStringLiteral("Trying UPnP port mapping..."));
	m_bridge.post({
		{ "type", "tryUpnp" },
		{ "sessionPort", m_hostPort ? m_hostPort->value() : 23456 },
	});
}

void RelayNetplayWindow::postPreviewRoomCode() {
	if (!m_hostAdvertise || !m_hostPort) {
		return;
	}
	m_bridge.post({
		{ "type", "previewRoomCode" },
		{ "advertiseHost", m_hostAdvertise->text().trimmed().toStdString() },
		{ "sessionPort", m_hostPort->value() },
	});
	renderHostShareCards();
}

void RelayNetplayWindow::onAdvertiseChanged() {
	if (!m_simpleUi) {
		postPreviewRoomCode();
	}
}

void RelayNetplayWindow::onBrokerChanged() {
	if (m_brokerUrl && !m_brokerUrl->text().trimmed().isEmpty()) {
		m_bridge.post({
			{ "type", "saveSettings" },
			{ "brokerBaseUrl", m_brokerUrl->text().trimmed().toStdString() },
		});
	}
}

void RelayNetplayWindow::startGameHost() {
	if (m_brokerUrl && !m_brokerUrl->text().trimmed().isEmpty()) {
		m_bridge.post({
			{ "type", "saveSettings" },
			{ "brokerBaseUrl", m_brokerUrl->text().trimmed().toStdString() },
		});
	}
	const QString method = getHostConnectMethod();
	const QString relayCode = activeRelayRoomCode();
	if (method == QStringLiteral("relay") && relayCode.isEmpty()) {
		showToast(QStringLiteral("Click Get code before starting."), QStringLiteral("error"));
		return;
	}
	nlohmann::json payload;
	payload["type"] = "start";
	payload["mode"] = "host";
	payload["connectMethod"] = method.toStdString();
	payload["displayName"] = getDisplayName(true).toStdString();
	payload["inputDelay"] = getInputDelay(true);
	payload["sessionPort"] = m_hostPort ? m_hostPort->value() : 23456;
	payload["advertiseHost"] = m_hostAdvertise ? m_hostAdvertise->text().trimmed().toStdString() : std::string();
	if (method == QStringLiteral("relay") && !relayCode.isEmpty()) {
		payload["relayRoomCode"] = relayCode.toStdString();
	}
	if (method == QStringLiteral("autoNat")) {
		payload["tryUpnp"] = true;
	}
	setStatus(QStringLiteral("Starting game..."));
	if (m_btnStartHost) {
		m_btnStartHost->setEnabled(false);
	}
	stopRelayHeartbeat();
	m_bridge.post(payload);
}

void RelayNetplayWindow::startGameJoin() {
	const QString target = m_joinRoomCode ? m_joinRoomCode->text().trimmed() : QString();
	if (target.isEmpty()) {
		showToast(QStringLiteral("Enter a room code or host address."), QStringLiteral("error"));
		return;
	}
	const QString method = getJoinConnectMethod(target);
	// Only complain when the user explicitly overrode the method; "auto"
	// derives from the same text and cannot contradict it.
	if (!m_simpleUi && m_joinConnectMethod &&
		m_joinConnectMethod->currentData().toString() != QStringLiteral("auto")) {
		if (method == QStringLiteral("relay") && !IsShortRoomCodeQString(target)) {
			showToast(QStringLiteral("Relay mode needs a room code like SF4-XXXX."), QStringLiteral("error"));
			return;
		}
		if (method == QStringLiteral("direct") && IsShortRoomCodeQString(target)) {
			showToast(QStringLiteral("Direct IP mode needs an address like 203.0.113.42:23456."), QStringLiteral("error"));
			return;
		}
	}
	setStatus(
		method == QStringLiteral("relay")
			? (m_forceVpsRelay ? QStringLiteral("Resolving room...") : QStringLiteral("Resolving room and checking host..."))
			: QStringLiteral("Connecting via direct IP..."));
	if (m_btnStartJoin) {
		m_btnStartJoin->setEnabled(false);
	}
	stopRelayHeartbeat();
	nlohmann::json payload = {
		{ "type", "start" },
		{ "mode", "join" },
		{ "connectMethod", method.toStdString() },
		{ "displayName", getDisplayName(false).toStdString() },
		{ "inputDelay", getInputDelay(false) },
	};
	if (method == QStringLiteral("relay")) {
		payload["roomCode"] = target.toStdString();
	}
	else {
		payload["joinAddress"] = target.toStdString();
	}
	m_bridge.post(payload);
}

void RelayNetplayWindow::onStartHost() {
	startGameHost();
}

void RelayNetplayWindow::onStartJoin() {
	startGameJoin();
}

void RelayNetplayWindow::onStartOffline() {
	stopRelayHeartbeat();
	m_bridge.post({ { "type", "start" }, { "mode", "offline" } });
}

void RelayNetplayWindow::onFindMatch() {
	setStatus(QStringLiteral("Searching for an opponent..."));
	m_bridge.post({
		{ "type", "start" },
		{ "mode", "join" },
		{ "connectMethod", "matchmaking" },
		{ "displayName", getDisplayName(false).toStdString() },
		{ "inputDelay", 2 },
		{ "joinAddress", "" },
	});
}

// Tidy up what people actually paste: chat clients add stray whitespace, and
// codes get retyped in lower case. Only the SF4- form is touched -- addresses
// and hostnames are left exactly as entered, since case can matter there.
QString RelayNetplayWindow::NormalizeRoomCodeText(const QString& raw) {
	QString cleaned = raw.simplified();
	cleaned.remove(QLatin1Char(' '));
	if (cleaned.startsWith(QStringLiteral("sf4-"), Qt::CaseInsensitive)) {
		return cleaned.toUpper();
	}
	return cleaned;
}

void RelayNetplayWindow::onPasteRoomCode() {
	const QClipboard* clip = QApplication::clipboard();
	if (!clip || !m_joinRoomCode) {
		return;
	}
	const QString pasted = NormalizeRoomCodeText(clip->text());
	if (pasted.isEmpty()) {
		showToast(QStringLiteral("Clipboard is empty."), QStringLiteral("error"));
		return;
	}
	m_joinRoomCode->setText(pasted);
	m_joinRoomCode->setFocus();
	if (m_btnPasteRoomCode) {
		flashCopied(m_btnPasteRoomCode, QStringLiteral("Pasted"));
	}
}

void RelayNetplayWindow::onBrowseRooms() {
	showScreen(Screen::Rooms);
	onRefreshRooms();
}

void RelayNetplayWindow::onRefreshRooms() {
	m_bridge.post({ { "type", "listRooms" } });
}

void RelayNetplayWindow::onRoomSelected(QListWidgetItem* item) {
	if (!item) {
		return;
	}
	const QString code = item->data(Qt::UserRole).toString();
	if (m_joinRoomCode) {
		m_joinRoomCode->setText(code);
	}
	// Rooms in the browser are always relay rooms. In advanced mode the connect
	// method dropdown may still be on "Direct IP:port" from an earlier session,
	// which would make Start fail with a confusing "needs an IP:port" error.
	// Force it to relay so the selected code works without the user hunting for
	// the dropdown.
	if (m_joinConnectMethod) {
		const int idx = m_joinConnectMethod->findData(QStringLiteral("relay"));
		if (idx >= 0) {
			m_joinConnectMethod->setCurrentIndex(idx);
		}
	}
	showScreen(Screen::Join);
}

void RelayNetplayWindow::onCheckUpdate() {
	m_updateBusy = true;
	if (m_btnCheckUpdate) {
		m_btnCheckUpdate->setEnabled(false);
	}
	m_bridge.post({ { "type", "checkUpdate" } });
}

void RelayNetplayWindow::onApplyUpdate() {
	// Installing is destructive and irreversible: it replaces the install folder
	// (removing files that are not part of the package) and then closes the
	// launcher to restart. Confirm before proceeding so a stray click can't wipe
	// an in-progress host/join session.
	const QString ver = m_updateLatestVersion.isEmpty()
		? QStringLiteral("the latest version")
		: m_updateLatestVersion;
	QMessageBox box(this);
	box.setWindowTitle(QStringLiteral("Install update"));
	box.setIcon(QMessageBox::Warning);
	box.setText(QStringLiteral("Install %1?").arg(ver));
	box.setInformativeText(QStringLiteral(
		"The launcher will close and replace files in its install folder. "
		"Any files you added there may be removed.\n\n"
		"Close Ultra Street Fighter IV first if it is running."));
	box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
	box.setDefaultButton(QMessageBox::Cancel);
	box.button(QMessageBox::Yes)->setText(QStringLiteral("Install and restart"));
	if (box.exec() != QMessageBox::Yes) {
		return;
	}

	m_updateBusy = true;
	if (m_btnInstallUpdate) {
		m_btnInstallUpdate->setEnabled(false);
	}
	m_bridge.post({ { "type", "applyUpdate" } });
}

void RelayNetplayWindow::onOpenRelease() {
	if (m_releaseUrl.isEmpty()) {
		return;
	}
	m_bridge.post({ { "type", "openUrl" }, { "url", m_releaseUrl.toStdString() } });
	QDesktopServices::openUrl(QUrl(m_releaseUrl));
}

void RelayNetplayWindow::startRelayHeartbeat() {
	stopRelayHeartbeat();
	if (activeRelayRoomCode().isEmpty()) {
		return;
	}
	m_heartbeatTimer.start(60000);
	onRelayHeartbeat();
}

void RelayNetplayWindow::stopRelayHeartbeat() {
	m_heartbeatTimer.stop();
}

void RelayNetplayWindow::onRelayHeartbeat() {
	const QString code = activeRelayRoomCode();
	if (code.isEmpty()) {
		stopRelayHeartbeat();
		return;
	}
	m_bridge.post({
		{ "type", "relayHeartbeat" },
		{ "roomCode", code.toStdString() },
	});
}

void RelayNetplayWindow::onToggleLog() {
	if (!m_logPanel || !m_btnToggleLog) {
		return;
	}
	const bool show = !m_logPanel->isVisible();
	m_logPanel->setVisible(show);
	m_btnToggleLog->setText(show ? QStringLiteral("Hide log") : QStringLiteral("Show log"));
}

void RelayNetplayWindow::onClearLog() {
	if (m_log) {
		m_log->clear();
	}
}

void RelayNetplayWindow::onModeSimple() {
	m_simpleUi = true;
	applyUiMode(true);
}

void RelayNetplayWindow::onModeAdvanced() {
	m_simpleUi = false;
	applyUiMode(true);
}

void RelayNetplayWindow::onGoHome() {
	setStatus(QString());
	showScreen(Screen::Home);
}

void RelayNetplayWindow::onGoHost() {
	showScreen(Screen::Host);
}

void RelayNetplayWindow::onGoJoin() {
	showScreen(Screen::Join);
}

void RelayNetplayWindow::onGoOffline() {
	showScreen(Screen::Offline);
}

} // namespace launcher
} // namespace sf4e
