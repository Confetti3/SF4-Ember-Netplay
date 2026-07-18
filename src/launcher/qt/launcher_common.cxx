#include "launcher_common.hxx"

#include <QAbstractSpinBox>
#include <QLabel>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "../netplay/netplay_room_code.hxx"

namespace sf4e {
namespace launcher {

QString JsonString(const nlohmann::json& j, const char* key, const QString& fallback) {
	if (!j.contains(key) || j[key].is_null()) {
		return fallback;
	}
	if (j[key].is_string()) {
		return QString::fromStdString(j[key].get<std::string>());
	}
	if (j[key].is_number_unsigned() || j[key].is_number_integer()) {
		return QString::number(j[key].get<long long>());
	}
	if (j[key].is_boolean()) {
		return j[key].get<bool>() ? QStringLiteral("true") : QStringLiteral("false");
	}
	return fallback;
}

QWidget* BuildStepper(QSpinBox* spinBox) {
	spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
	spinBox->setMinimumWidth(48);
	spinBox->setMaximumWidth(64);
	spinBox->setObjectName(QStringLiteral("stepperValue"));

	auto* stepper = new QWidget();
	stepper->setObjectName(QStringLiteral("stepperControl"));
	auto* row = new QHBoxLayout(stepper);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(3);

	auto* minus = new QPushButton(QStringLiteral("-"));
	minus->setObjectName(QStringLiteral("stepperButton"));
	minus->setFixedSize(24, 24);
	auto* plus = new QPushButton(QStringLiteral("+"));
	plus->setObjectName(QStringLiteral("stepperButton"));
	plus->setFixedSize(24, 24);

	QObject::connect(minus, &QPushButton::clicked, spinBox, [spinBox]() { spinBox->stepDown(); });
	QObject::connect(plus, &QPushButton::clicked, spinBox, [spinBox]() { spinBox->stepUp(); });

	row->addWidget(spinBox);
	row->addWidget(minus);
	row->addWidget(plus);
	row->addStretch();
	stepper->setMinimumHeight(26);
	ConfigureFormField(spinBox);
	return stepper;
}

void ConfigureFormField(QWidget* field) {
	if (!field) {
		return;
	}
	field->setMinimumHeight(24);
	auto policy = field->sizePolicy();
	policy.setVerticalPolicy(QSizePolicy::Fixed);
	field->setSizePolicy(policy);
}

void ConfigureModeCard(QPushButton* btn) {
	if (!btn) {
		return;
	}
	btn->setMinimumHeight(52);
	btn->setMaximumHeight(56);
	auto policy = btn->sizePolicy();
	policy.setVerticalPolicy(QSizePolicy::Fixed);
	policy.setHorizontalPolicy(QSizePolicy::Expanding);
	btn->setSizePolicy(policy);
}

bool IsShortRoomCodeQString(const QString& code) {
	const QByteArray utf8 = code.trimmed().toUtf8();
	return IsShortRoomCode(utf8.constData());
}

QPushButton* MakeModeCard(const QString& title, const QString& desc, const QString& objectName) {
	auto* btn = new QPushButton();
	btn->setObjectName(objectName);
	btn->setCursor(Qt::PointingHandCursor);
	auto* layout = new QVBoxLayout(btn);
	layout->setContentsMargins(12, 8, 12, 8);
	layout->setSpacing(2);
	auto* titleLabel = new QLabel(title);
	titleLabel->setObjectName(QStringLiteral("modeCardTitle"));
	titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto* descLabel = new QLabel(desc);
	descLabel->setObjectName(QStringLiteral("modeCardDesc"));
	descLabel->setWordWrap(true);
	descLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	layout->addWidget(titleLabel);
	layout->addWidget(descLabel);
	ConfigureModeCard(btn);
	return btn;
}

QWidget* MakeShareCard(const QString& shareId, QLabel** valueOut, QPushButton** copyOut) {
	auto* card = new QWidget();
	card->setObjectName(QStringLiteral("shareCard"));
	card->setProperty("shareId", shareId);
	auto* layout = new QVBoxLayout(card);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(2);

	auto* title = new QLabel(shareId == QStringLiteral("relay") ? QStringLiteral("Relay code")
		: shareId == QStringLiteral("lan") ? QStringLiteral("LAN address")
										   : QStringLiteral("Public address"));
	title->setObjectName(QStringLiteral("shareCardTitle"));
	auto* value = new QLabel(QStringLiteral("-"));
	value->setObjectName(QStringLiteral("shareCardValue"));
	value->setWordWrap(true);
	auto* copy = new QPushButton(QStringLiteral("Copy"));
	copy->setObjectName(QStringLiteral("secondaryButton"));
	copy->setProperty("shareId", shareId);
	copy->setEnabled(false);
	copy->setMaximumWidth(72);

	auto* valueRow = new QHBoxLayout();
	valueRow->setContentsMargins(0, 0, 0, 0);
	valueRow->setSpacing(6);
	valueRow->addWidget(value, 1);
	valueRow->addWidget(copy, 0, Qt::AlignTop);

	layout->addWidget(title);
	layout->addLayout(valueRow);

	if (valueOut) {
		*valueOut = value;
	}
	if (copyOut) {
		*copyOut = copy;
	}
	return card;
}

} // namespace launcher
} // namespace sf4e
