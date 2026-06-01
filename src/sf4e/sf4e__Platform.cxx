#include <memory>
#include <vector>

#include <windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <strsafe.h>

#include <detours/detours.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/wincolor_sink.h"

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "sf4e.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__UserApp.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__NetplayFacade.hxx"

namespace rPlatform = Dimps::Platform;
using rD3D = rPlatform::D3D;
using rGFxApp = rPlatform::GFxApp;
using rMain = rPlatform::Main;
using rSound = rPlatform::Sound;

namespace fPlatform = sf4e::Platform;
using fD3D = fPlatform::D3D;
using fGFxApp = fPlatform::GFxApp;
using fMain = fPlatform::Main;
using fUserApp = sf4e::UserApp;
using fSound = fPlatform::Sound;

template <int N>
using fSoundObjectPoolEntry = fPlatform::SoundObjectPoolEntry<N>;

template <int N>
using fSoundObjectPool = fPlatform::SoundObjectPool<N>;

bool fSound::bAllowNewPlayers = true;

void fPlatform::Install() {
    D3D::Install();
    Main::Install();
    Sound::Install();
}

void fD3D::Install() {
    void (fD3D:: * _fDestroy)() = &Destroy;
    DWORD(fD3D:: * _fReset)() = &Reset;
    void(fD3D:: * _fRunScene_Render)(void*) = &RunScene_Render;
    DetourAttach((PVOID*)&rD3D::privateMethods.Destroy, *(PVOID*)&_fDestroy);
    DetourAttach((PVOID*)&rD3D::privateMethods.Reset, *(PVOID*)&_fReset);
    DetourAttach((PVOID*)&rD3D::privateMethods.RunScene_Render, *(PVOID*)&_fRunScene_Render);
}

void fD3D::RunScene_Render(void* sceneCommandList) {
    (this->*rD3D::privateMethods.RunScene_Render)(sceneCommandList);
    Overlay::DrawOverlay();
}

void fD3D::Destroy() {
    Overlay::FreeOverlay();
    (this->*rD3D::privateMethods.Destroy)();
}

DWORD fD3D::Reset() {
    Overlay::FreeOverlay();
    DWORD out = (this->*rD3D::privateMethods.Reset)();
    Overlay::InitializeOverlay(
        (*rMain::GetWindowData(rMain::staticMethods.GetSingleton()))->hWnd,
        Dimps::Platform::D3D::staticMethods.GetSingleton()->lpD3DDevice
    );
    return out;
}

void fGFxApp::RecordToAdditionalMemento(rGFxApp* a, AdditionalMemento& m) {
    int i;

    rGFxApp::ObjectPool<Dimps::Eva::IEmSpriteAction>* actionPool = rGFxApp::GetActionPool(a);
    for (i = 0; i < NUM_GFX_ACTIONS; i++) {
        m.actions[i].first = actionPool->useIndex[i];
        if (actionPool->useIndex[i]) {
            sf4e::Eva::IEmSpriteAction::RecordToAdditionalMemento(&actionPool->raw[i], m.actions[i].second);
        }
    }
}

void fGFxApp::RestoreFromAdditionalMemento(rGFxApp* a, const AdditionalMemento& m) {
    int i;

    rGFxApp::ObjectPool<Dimps::Eva::IEmSpriteAction>* actionPool = rGFxApp::GetActionPool(a);
    for (i = 0; i < NUM_GFX_ACTIONS; i++) {
        actionPool->useIndex[i] = m.actions[i].first;
        if (actionPool->useIndex[i]) {
            sf4e::Eva::IEmSpriteAction::RestoreFromAdditionalMemento(&actionPool->raw[i], m.actions[i].second);
        }
    }
}

void fMain::Install() {
    int (fMain:: * _fInitialize)(void*, void*, void*) = &Initialize;
    void (fMain:: * _fDestroy)() = &Destroy;
    DetourAttach((PVOID*)&rMain::publicMethods.Initialize, *(PVOID*)&_fInitialize);
    DetourAttach((PVOID*)&rMain::publicMethods.Destroy, *(PVOID*)&_fDestroy);
    DetourAttach((PVOID*)&rMain::staticMethods.RunWindowFunc, &RunWindowFunc);
}

int fMain::Initialize(void* a, void* b, void* c) {
    if (sf4e::hSyncEvent != NULL) {
        SetEvent(sf4e::hSyncEvent);
        CloseHandle(sf4e::hSyncEvent);
        sf4e::hSyncEvent = NULL;
    }

    int rval = (this->*(rMain::publicMethods.Initialize))(a, b, c);

    BOOL hasConsole = false;
    if (sf4e::args.bShowConsole) {
        hasConsole = AllocConsole();
        if (!hasConsole) {
            MessageBox(NULL, TEXT("Could not allocate console!"), NULL, MB_OK);
        }
    }

    // Set up spdlog
    PWSTR path;
    HRESULT queryResult = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &path);
    if (queryResult == S_OK) {
        try
        {
            wchar_t logpath[MAX_PATH];
            PathCombineW(logpath, path, L"sf4e/logs/sf4e.log");
            int max_size = 1048576 * 5; // 5MB
            int max_files = 10;

            std::vector<spdlog::sink_ptr> sinks;
            sinks.push_back(std::shared_ptr<spdlog::sinks::rotating_file_sink_mt>(
                new spdlog::sinks::rotating_file_sink_mt(logpath, max_size, max_files, true)
            ));
            if (hasConsole) {
                sinks.push_back(std::shared_ptr<spdlog::sinks::wincolor_stdout_sink_mt>(
                    new spdlog::sinks::wincolor_stdout_sink_mt()
                ));
            }
            std::shared_ptr<spdlog::logger> logger(new spdlog::logger("sf4e", sinks.begin(), sinks.end()));
            spdlog::set_default_logger(logger);
            spdlog::flush_on(spdlog::level::info);
            spdlog::info("Welcome to sf4e");
            spdlog::info("Sidecar logging initialized; install hooks are active");
        }
        catch (const spdlog::spdlog_ex& ex)
        {
            MessageBoxA(NULL, ex.what(), NULL, MB_OK);
            DebugBreak();
        }
    }
    else {
        MessageBox(NULL, TEXT("Could not get appdata path for logs!"), NULL, MB_OK);
    }
    CoTaskMemFree(path);

    Overlay::InitializeOverlay(
        (*rMain::GetWindowData(rMain::staticMethods.GetSingleton()))->hWnd,
        Dimps::Platform::D3D::staticMethods.GetSingleton()->lpD3DDevice
    );

    SteamDatagramErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        spdlog::error("GameNetworkingSockets_Init failed.  {}", errMsg);
    }

    sf4e::NetplayFacade::NotifyGameReady();

    return rval;
}

void fMain::Destroy() {
    if (fUserApp::netplay) {
        delete fUserApp::netplay.release();
    }
    if (fUserApp::server) {
        delete fUserApp::server.release();
    }
    GameNetworkingSockets_Kill();
    spdlog::shutdown();
    (this->*rMain::publicMethods.Destroy)();
}

void WINAPI fMain::RunWindowFunc(rMain* lpMain, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (Overlay::OverlayWindowFunc(hwnd, uMsg, wParam, lParam)) {
        return;
    }
    rMain::staticMethods.RunWindowFunc(lpMain, hwnd, uMsg, wParam, lParam);
}


void fSound::Install() {
    DetourAttach((PVOID*)&rSound::staticMethods.GetNewPlayerHandle, &GetNewPlayerHandle);
}

uint32_t fSound::GetNewPlayerHandle() {
    if (!bAllowNewPlayers) {
        return 0xffffffff;
    }

    return rSound::staticMethods.GetNewPlayerHandle();
}
