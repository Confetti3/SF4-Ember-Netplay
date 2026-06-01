#pragma once



#include <windows.h>



#include "netplay/netplay_launch_controller.hxx"



namespace sf4e {

namespace launcher {



	bool IsWebView2RuntimeAvailable(wchar_t* errorBuf, int errorBufChars);

	// Modal WebView2 window; returns true if user confirmed start (controller finished, not cancelled).

	bool RunNetplayWebViewUi(HWND parent, NetplayLaunchController& controller);



} // namespace launcher

} // namespace sf4e

