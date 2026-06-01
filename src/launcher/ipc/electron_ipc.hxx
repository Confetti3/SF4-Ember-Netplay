#pragma once

namespace sf4e {
namespace launcher {

	class NetplayLaunchController;

	// JSON-lines IPC on stdin/stdout for Electron (or other UI shells). Returns true when
	// the user confirmed netplay setup (same as a completed WebView wizard).
	bool RunElectronIpcBridge(NetplayLaunchController& controller);

} // namespace launcher
} // namespace sf4e
