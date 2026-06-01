#include "qt_launcher_app.hxx"



#include <QApplication>

#include <QCoreApplication>

#include <QDir>

#include <QFileInfo>



#include "../../common/install_paths.hxx"

#include "steam_p2p_window.hxx"



namespace sf4e {

namespace launcher {



namespace {



void ConfigureQtPluginPaths() {

	wchar_t installRoot[MAX_PATH] = { 0 };

	if (!install::GetInstallRoot(installRoot, MAX_PATH)) {

		return;

	}

	const QString rootDir = QDir::fromNativeSeparators(QString::fromWCharArray(installRoot));

	// Plugins and qt.conf live at package root; LauncherApp.exe runs from dll/.

	const QString pluginsDir = rootDir + QStringLiteral("/plugins");

	const QString platformsDir = pluginsDir + QStringLiteral("/platforms");



	QStringList libPaths{ rootDir, pluginsDir };

	wchar_t dllDir[MAX_PATH] = { 0 };

	if (install::GetPackageDllDirectory(dllDir, MAX_PATH) && install::UsesDllSubdirectory()) {

		libPaths.prepend(QDir::fromNativeSeparators(QString::fromWCharArray(dllDir)));

	}



	QCoreApplication::setLibraryPaths(libPaths);

	qputenv("QT_PLUGIN_PATH", QDir::toNativeSeparators(pluginsDir).toUtf8());

	qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", QDir::toNativeSeparators(platformsDir).toUtf8());

}



} // namespace



bool RunNetplayQtUi(NetplayLaunchController& controller) {

	ConfigureQtPluginPaths();



	static int argc = 1;

	static char arg0[] = "Launcher.exe";

	static char* argv[] = { arg0, nullptr };

	QApplication app(argc, argv);

	QApplication::setApplicationName("SF4 Netplay Launcher");

	QApplication::setOrganizationName("sf4e");



	SteamP2pMainWindow window(controller);

	window.show();

	const int code = app.exec();

	(void)code;

	return controller.IsFinished() && !controller.WasCancelled();

}



} // namespace launcher

} // namespace sf4e

