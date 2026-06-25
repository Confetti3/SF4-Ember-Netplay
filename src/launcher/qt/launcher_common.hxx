#pragma once

#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

#include <nlohmann/json.hpp>

namespace sf4e {
namespace launcher {

QString JsonString(const nlohmann::json& j, const char* key, const QString& fallback = QString());

QWidget* BuildStepper(QSpinBox* spinBox);

bool IsShortRoomCodeQString(const QString& code);

QPushButton* MakeModeCard(const QString& title, const QString& desc, const QString& objectName);

void ConfigureFormField(QWidget* field);

void ConfigureModeCard(QPushButton* btn);

QWidget* MakeShareCard(
	const QString& shareId,
	QLabel** valueOut,
	QPushButton** copyOut);

} // namespace launcher
} // namespace sf4e
