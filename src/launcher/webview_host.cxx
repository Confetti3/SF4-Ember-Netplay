#include "webview_host.hxx"



#include <stdlib.h>

#include <string>

#include <pathcch.h>
#include <shlobj.h>
#include <strsafe.h>

#include <wrl.h>

#include <wil/com.h>

#include <WebView2.h>



using Microsoft::WRL::ComPtr;

using Microsoft::WRL::Callback;



namespace sf4e {

namespace launcher {



	namespace {



		struct WebViewUiState {

			HWND hwnd = NULL;

			NetplayLaunchController* controller = nullptr;

			ComPtr<ICoreWebView2Controller> webviewController;

			ComPtr<ICoreWebView2> webview;

			bool pendingInitialState = true;

		};



		static WebViewUiState* g_ui = nullptr;



		static void PostJsonToWeb(ICoreWebView2* webview, const nlohmann::json& j) {

			if (!webview || j.is_null() || j.empty()) {

				return;

			}

			std::string s = j.dump();

			int wideLen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);

			if (wideLen <= 0) {

				return;

			}

			std::wstring wide((size_t)wideLen, L'\0');

			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &wide[0], wideLen);

			webview->PostWebMessageAsJson(wide.c_str());

		}



		static void ResizeWebView(WebViewUiState* ui) {

			if (!ui || !ui->webviewController || !ui->hwnd) {

				return;

			}

			RECT bounds;

			GetClientRect(ui->hwnd, &bounds);

			ui->webviewController->put_Bounds(bounds);

		}



		static void HandleWebMessage(WebViewUiState* ui, const std::wstring& jsonWide) {

			if (!ui || !ui->controller) {

				return;

			}

			int utf8Len = WideCharToMultiByte(CP_UTF8, 0, jsonWide.c_str(), -1, NULL, 0, NULL, NULL);

			if (utf8Len <= 0) {

				return;

			}

			std::string utf8((size_t)utf8Len, '\0');

			WideCharToMultiByte(CP_UTF8, 0, jsonWide.c_str(), -1, &utf8[0], utf8Len, NULL, NULL);



			try {

				nlohmann::json msg = nlohmann::json::parse(utf8);

				nlohmann::json reply = ui->controller->HandleWebMessage(msg);

				if (!reply.is_null() && !reply.empty()) {

					PostJsonToWeb(ui->webview.Get(), reply);

				}

				if (ui->controller->ShouldExitForUpdate() || ui->controller->IsFinished()) {

					PostMessage(ui->hwnd, WM_CLOSE, 0, 0);

				}

			}

			catch (...) {

				nlohmann::json err;

				err["v"] = 1;

				err["type"] = "error";

				err["message"] = "Invalid message from UI.";

				PostJsonToWeb(ui->webview.Get(), err);

			}

		}



		static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

			WebViewUiState* ui = (WebViewUiState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

			switch (msg) {

			case WM_SIZE:

				ResizeWebView(ui);

				return 0;

			case WM_CLOSE:

				if (ui && ui->controller && !ui->controller->IsFinished() && !ui->controller->ShouldExitForUpdate()) {

					ui->controller->HandleWebMessage(nlohmann::json{ {"type", "cancel"}, {"v", 1} });

				}

				DestroyWindow(hwnd);

				return 0;

			case WM_DESTROY:

				PostQuitMessage(0);

				return 0;

			}

			return DefWindowProc(hwnd, msg, wParam, lParam);

		}



		static HRESULT InitWebView(WebViewUiState* ui) {

			wchar_t userData[MAX_PATH] = { 0 };

			if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, userData))) {

				return E_FAIL;

			}

			wchar_t dataDir[MAX_PATH] = { 0 };

			PathCchCombine(dataDir, MAX_PATH, userData, L"sf4e\\launcher-webview2");



			ComPtr<ICoreWebView2Environment> env;

			HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(

				nullptr,

				dataDir,

				nullptr,

				Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(

					[ui](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {

						if (FAILED(result) || !environment) {

							return result;

						}

						return environment->CreateCoreWebView2Controller(

							ui->hwnd,

							Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(

								[ui](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {

									if (FAILED(result2) || !controller) {

										return result2;

									}

									ui->webviewController = controller;

									ui->webviewController->put_ZoomFactor(1.0);

									ui->webviewController->get_CoreWebView2(&ui->webview);

									wil::com_ptr<ICoreWebView2Controller2> controller2;
									if (SUCCEEDED(ui->webviewController->QueryInterface(IID_PPV_ARGS(&controller2)))) {
										COREWEBVIEW2_COLOR bgColor = { 255, 22, 24, 29 };
										controller2->put_DefaultBackgroundColor(bgColor);
									}

									wil::com_ptr<ICoreWebView2Settings> settings;

									ui->webview->get_Settings(&settings);

									settings->put_IsStatusBarEnabled(FALSE);

									settings->put_AreDefaultContextMenusEnabled(FALSE);

									settings->put_IsWebMessageEnabled(TRUE);



									ui->webview->add_WebMessageReceived(

										Callback<ICoreWebView2WebMessageReceivedEventHandler>(

											[ui](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {

												(void)sender;

												wil::unique_cotaskmem_string jsonWide;

												args->get_WebMessageAsJson(jsonWide.put());

												if (jsonWide) {

													HandleWebMessage(ui, jsonWide.get());

												}

												return S_OK;

											}

										).Get(),

										nullptr

									);



									ui->webview->add_NavigationCompleted(

										Callback<ICoreWebView2NavigationCompletedEventHandler>(

											[ui](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {

												(void)sender;

												BOOL success = FALSE;

												args->get_IsSuccess(&success);

												if (success && ui->pendingInitialState && ui->controller) {

													ui->pendingInitialState = false;

													PostJsonToWeb(ui->webview.Get(), ui->controller->BuildStateJson());

												}

												return S_OK;

											}

										).Get(),

										nullptr

									);



									ResizeWebView(ui);

									wchar_t url[MAX_PATH * 2] = { 0 };

									if (GetLauncherUiIndexUrl(url, (int)(sizeof(url) / sizeof(url[0])))) {

										ui->webview->Navigate(url);

									}

									return S_OK;

								}

							).Get()

						);

					}

				).Get()

			);

			return hr;

		}



	} // namespace



	bool IsWebView2RuntimeAvailable(wchar_t* errorBuf, int errorBufChars) {
		wchar_t* version = nullptr;
		HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
		if (SUCCEEDED(hr) && version) {
			CoTaskMemFree(version);
			return true;
		}
		if (errorBuf && errorBufChars > 0) {
			StringCchPrintfW(
				errorBuf,
				errorBufChars,
				L"Microsoft Edge WebView2 Runtime is required.\n\n"
				L"Install from: https://go.microsoft.com/fwlink/p/?LinkId=2124703"
			);
		}
		return false;
	}

	bool RunNetplayWebViewUi(HWND parent, NetplayLaunchController& controller) {

		CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

		wchar_t err[512] = { 0 };

		if (!IsWebView2RuntimeAvailable(err, (int)(sizeof(err) / sizeof(err[0])))) {

			MessageBoxW(parent, err, L"sf4e", MB_OK | MB_ICONWARNING);

			return false;

		}



		WebViewUiState ui;

		ui.controller = &controller;

		g_ui = &ui;



		WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };

		wc.lpfnWndProc = WndProc;

		wc.hInstance = GetModuleHandleW(NULL);

		wc.hCursor = LoadCursor(NULL, IDC_ARROW);

		wc.lpszClassName = L"sf4eLauncherWebView";

		RegisterClassExW(&wc);



		ui.hwnd = CreateWindowExW(

			0,

			wc.lpszClassName,

			L"SF4 Netplay Launcher (unofficial port)",

			WS_OVERLAPPEDWINDOW,

			CW_USEDEFAULT,

			CW_USEDEFAULT,

			500,

			700,

			parent,

			NULL,

			wc.hInstance,

			NULL

		);

		if (!ui.hwnd) {

			g_ui = nullptr;

			return false;

		}

		SetWindowLongPtr(ui.hwnd, GWLP_USERDATA, (LONG_PTR)&ui);

		ShowWindow(ui.hwnd, SW_SHOW);

		UpdateWindow(ui.hwnd);



		if (FAILED(InitWebView(&ui))) {

			MessageBoxW(ui.hwnd,

				L"Could not initialize WebView2. Reinstall the WebView2 Runtime.",

				L"sf4e",

				MB_OK | MB_ICONERROR);

			DestroyWindow(ui.hwnd);

			g_ui = nullptr;

			return false;

		}



		MSG msg;

		while (GetMessage(&msg, NULL, 0, 0) > 0) {

			TranslateMessage(&msg);

			DispatchMessage(&msg);

		}



		g_ui = nullptr;

		return controller.IsFinished() && !controller.WasCancelled();

	}



} // namespace launcher

} // namespace sf4e

