#include "RecoverySurface.hxx"
#include "RecoveryMenu.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../platform/ApplicationServices.hxx"
#include <windows.h>
#include <shobjidl.h>
#include <d3d9.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx9.h>
#include <filesystem>

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace sf4e { namespace ui {
namespace {
std::wstring Utf8ToWide(const char* value) {
    if (!value) return {};
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,nullptr,0);
    if(length<=1)return {};
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,&result[0],length))return {};
    result.pop_back();return result;
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
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
bool RunRecovery(std::string message, std::wstring& gameDirectory, bool updates) {
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ApplyTheme(ImGui_ImplWin32_GetDpiScaleForHwnd(window));
    ImGui_ImplWin32_Init(window); ImGui_ImplDX9_Init(device);
    wchar_t executable[32768]={};GetModuleFileNameW(nullptr,executable,32768);
    auto art=std::make_unique<SelectionArt>(device,gameDirectory,(std::filesystem::path(executable).parent_path()/L"assets"/L"selection").wstring());
    SetMenuArt(art.get());
    ShowWindow(window, SW_SHOW); UpdateWindow(window);
    platform::ApplicationServices services;
    if (updates) services.Request(platform::ServiceAction::CheckUpdates);
    bool quit = false, retry = false;
    GameMenu menu;menu.navigation=MenuNavigation("close");
    while (!quit) {
        MSG event;
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
            if (event.message == WM_QUIT) quit = true;
            TranslateMessage(&event); DispatchMessageW(&event);
        }
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
        MenuInput input;
        const ImGuiKey buttons[]={ImGuiKey_GamepadDpadUp,ImGuiKey_GamepadDpadDown,ImGuiKey_GamepadDpadLeft,ImGuiKey_GamepadDpadRight,ImGuiKey_GamepadFaceDown,ImGuiKey_GamepadFaceRight};
        for(unsigned i=0;i<6;++i)if(ImGui::IsKeyDown(buttons[i]))input.held|=1u<<i;
        SetMenuInput(input);SetMenuGlyphs(io.BackendFlags&ImGuiBackendFlags_HasGamepad?3:0,0x40000,0x20000);
        const auto state = services.Snapshot();
        switch(DrawRecoveryMenu(menu,state,message,updates)) {
        case RecoveryChoice::Folder:
            if(ChooseDirectory(window,gameDirectory))message=std::filesystem::exists(std::filesystem::path(gameDirectory)/L"SSFIV.exe")?
                loc::T("recovery.folder_selected"):loc::T("recovery.folder_invalid");
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
    SetMenuArt(nullptr);art.reset();
    ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    device->Release(); d3d->Release(); DestroyWindow(window); if (SUCCEEDED(com)) CoUninitialize();
    return retry;
}
} }
