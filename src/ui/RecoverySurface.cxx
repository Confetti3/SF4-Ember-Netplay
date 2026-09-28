#include "RecoverySurface.hxx"
#include "RecoveryMenu.hxx"
#include "RecoveryController.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../platform/Utf8.hxx"
#include <windows.h>
#include <shobjidl.h>
#include <d3d9.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx9.h>
#include <filesystem>
#include <memory>
#include <utility>

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace sf4e { namespace ui {
namespace {
using platform::Utf8ToWide;
// Set by WM_DEVICECHANGE, which top-level windows receive without registering;
// the message loop hands it to the controller poller.
bool devicesChanged = false;
// Losing activation or being minimized disarms the pads (RecoveryController::
// Deactivate), including while a frame is skipped and nothing is polled.
bool deactivated = false;
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_DEVICECHANGE) devicesChanged = true;
    if ((message == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || (message == WM_ACTIVATEAPP && !w) ||
        (message == WM_SIZE && w == SIZE_MINIMIZED)) deactivated = true;
    if (ImGui::GetCurrentContext()) {
        const auto handled = ImGui_ImplWin32_WndProcHandler(window, message, w, l);
        if (handled) return handled;
    }
    if (message == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, w, l);
}
bool ChooseDirectory(HWND owner, std::wstring& path) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const auto title=Utf8ToWide(loc::T("recovery.choose_folder_title"));
    dialog->SetTitle(title.c_str());
    bool selected = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) { path = name; CoTaskMemFree(name); selected = true; }
            item->Release();
        }
    }
    dialog->Release(); return selected;
}
}
bool RunRecovery(std::string message, std::wstring& gameDirectory, bool updates, std::function<void(const std::string&)> artLog) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SF4EmberRecovery"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"SF4 Ember Netplay", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 940, 720, nullptr, nullptr, wc.hInstance, nullptr);
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION); IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS params{}; params.Windowed = TRUE; params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window; params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (!window || !d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &device))) {
        const auto text=Utf8ToWide(loc::T("recovery.d3d_failed"));
        const auto title=Utf8ToWide(loc::T("app.name"));
        MessageBoxW(window, text.c_str(), title.c_str(), MB_ICONERROR);
        if (d3d) d3d->Release(); if (window) DestroyWindow(window); if (SUCCEEDED(com)) CoUninitialize(); return false;
    }
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    // Pads reach the menu only through RecoveryController; the backend is
    // built without gamepad support, so ImGui gamepad navigation stays off.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyTheme(ImGui_ImplWin32_GetDpiScaleForHwnd(window));
    ImGui_ImplWin32_Init(window); ImGui_ImplDX9_Init(device);
    wchar_t executable[32768]={};GetModuleFileNameW(nullptr,executable,32768);
    auto art=std::make_unique<SelectionArt>(device,gameDirectory,(std::filesystem::path(executable).parent_path()/L"assets"/L"selection").wstring(),std::move(artLog));
    SetMenuArt(art.get());
    ShowWindow(window, SW_SHOW); UpdateWindow(window);
    platform::ApplicationServices services;
    if (updates) services.Request(platform::ServiceAction::CheckUpdates);
    bool quit = false, retry = false;
    Tone messageTone = Tone::Error;  // the launch failure that opened recovery
    GameMenu menu;menu.navigation=RecoveryNavigation(updates);
    // Released before the window it is bound to is destroyed.
    auto controller=std::make_unique<RecoveryController>(window);
    while (!quit) {
        MSG event;
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
            if (event.message == WM_QUIT) quit = true;
            TranslateMessage(&event); DispatchMessageW(&event);
        }
        // Before any frame is skipped below, so a minimized or lost window disarms too.
        if (std::exchange(deactivated, false)) controller->Deactivate();
        if (quit) { services.Cancel(); break; }
        RECT client{}; GetClientRect(window, &client);
        if (IsIconic(window) || client.right == 0 || client.bottom == 0) { MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT); continue; }
        const auto deviceState = device->TestCooperativeLevel();
        if (deviceState == D3DERR_DEVICELOST) { MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT); continue; }
        if (deviceState == D3DERR_DEVICENOTRESET ||
            params.BackBufferWidth != static_cast<UINT>(client.right) || params.BackBufferHeight != static_cast<UINT>(client.bottom)) {
            ImGui_ImplDX9_InvalidateDeviceObjects(); params.BackBufferWidth = client.right; params.BackBufferHeight = client.bottom;
            if (FAILED(device->Reset(&params))) { MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT); continue; }
        }
        if (ApplyTheme(ImGui_ImplWin32_GetDpiScaleForHwnd(window))) ImGui_ImplDX9_InvalidateDeviceObjects();
        art->Pump();
        ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        // ReadMenuInput adds these keys to the pad bits itself; here they only
        // tell the legend that the keyboard was used last.
        unsigned keyboard=0;
        const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
        for(unsigned i=0;i<6;++i)if(ImGui::IsKeyDown(keys[i]))keyboard|=1u<<i;
        if(std::exchange(devicesChanged,false))controller->DevicesChanged();
        // Pads are read whatever window has focus, so they drive the menu only
        // while it is in front.
        MenuInput input;input.held=controller->Poll(keyboard,GetForegroundWindow()==window);
        SetMenuInput(input);
        switch(controller->Family()){
        case PadFamily::Xbox:SetMenuGlyphs(3,0x40000,0x20000);break;
        case PadFamily::DirectInput:SetMenuGlyphs(4,0,0,"1","2");break;
        default:SetMenuGlyphs(0,0,0);break;
        }
        const auto state = services.Snapshot();
        switch(DrawRecoveryMenu(menu,state,message,updates,messageTone)) {
        case RecoveryChoice::Folder:
            // The picker blocks this loop, so the pads are disarmed before it
            // opens: a button held while it was up cannot press on return.
            controller->Deactivate();
            if(ChooseDirectory(window,gameDirectory)){
                const bool valid=std::filesystem::exists(std::filesystem::path(gameDirectory)/L"SSFIV.exe");
                message=valid?loc::T("recovery.folder_selected"):loc::T("recovery.folder_invalid");
                messageTone=valid?Tone::Success:Tone::Error;
            }
            break;
        case RecoveryChoice::Retry:retry=true;quit=true;break;
        case RecoveryChoice::CheckUpdates:services.Request(platform::ServiceAction::CheckUpdates);break;
        case RecoveryChoice::Install:services.Request(platform::ServiceAction::InstallUpdate);break;
        case RecoveryChoice::Cancel:services.Cancel();break;
        case RecoveryChoice::Close:services.Cancel();quit=true;break;
        default:break;
        }
        if (state.installed) quit = true;
        ImGui::Render();
        device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(20,19,18),1,0);
        if (SUCCEEDED(device->BeginScene())) { ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); device->EndScene(); }
        device->Present(nullptr,nullptr,nullptr,nullptr);
    }
    controller.reset();SetMenuArt(nullptr);art.reset();
    ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    device->Release(); d3d->Release(); DestroyWindow(window); if (SUCCEEDED(com)) CoUninitialize();
    return retry;
}
} }
