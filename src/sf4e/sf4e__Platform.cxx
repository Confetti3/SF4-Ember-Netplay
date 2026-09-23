#include <memory>
#include <vector>

#include <windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <strsafe.h>

#include <detours/detours.h>

#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/wincolor_sink.h"

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "sf4e.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__UserApp.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "../common/sf4e__PacingController.hxx"
#include "../common/FrameShiftMailbox.hxx"
#include "../common/EnvFlag.hxx"

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

unsigned long long fPlatform::AsyncLogDropped() {
    const auto pool = spdlog::thread_pool();
    return pool ? pool->overrun_counter() : 0;
}

void fPlatform::Install() {
    D3D::Install();
    Main::Install();
    Sound::Install();
}

void fD3D::Install() {
    void (fD3D:: * _fDestroy)() = &Destroy;
    DWORD(fD3D:: * _fReset)() = &Reset;
    void(fD3D:: * _fRunScene_Render)(void*) = &RunScene_Render;
    int(fD3D:: * _fLimitFrame)(float) = &LimitFrame;
    void(fD3D:: * _fBuildPresentParameters)() = &BuildPresentParameters;
    DetourAttach((PVOID*)&rD3D::privateMethods.Destroy, *(PVOID*)&_fDestroy);
    DetourAttach((PVOID*)&rD3D::privateMethods.Reset, *(PVOID*)&_fReset);
    DetourAttach((PVOID*)&rD3D::privateMethods.RunScene_Render, *(PVOID*)&_fRunScene_Render);
    DetourAttach((PVOID*)&rD3D::privateMethods.LimitFrame, *(PVOID*)&_fLimitFrame);
    DetourAttach((PVOID*)&rD3D::privateMethods.BuildPresentParameters, *(PVOID*)&_fBuildPresentParameters);
}

namespace {
// The pacing tick's request and the limiter's applied shift. Reset fences a
// limiter call already in its spin (FrameShiftMailbox).
sf4e::pacing::FrameShiftMailbox s_frameShift;

// Present interval the game asked for when it was last forced; 0 when never.
// The first device is created before logging starts, so Main::Initialize
// reports it once the log is up.
DWORD s_vsyncForcedFrom = 0;

void LogVSyncForced() {
    if (s_vsyncForcedFrom) {
        spdlog::info("Display: VSync forced off (game setting asked for interval {:#x})", s_vsyncForcedFrom);
    }
}

// Dev check for the limiter hook: SF4E_PACING_TEST_SHIFT_MS (2, -2, or 0 for a
// baseline) replaces every pacing request with that shift and logs the
// measured frame every 600 frames. Nothing reaches the pacing controller.
struct LimiterTest {
    bool enabled = false;
    double shiftMs = 0.0;
    int frames = 0;
    double frameMs = 0.0, appliedMs = 0.0;

    void Record(double measuredFrameMs, double measuredAppliedMs) {
        frameMs += measuredFrameMs;
        appliedMs += measuredAppliedMs;
        if (++frames < 600) return;
        spdlog::info("PacingTest: shift={:.2f}ms frames={} meanFrameMs={:.3f} fps={:.2f} appliedMs={:.1f}",
            shiftMs, frames, frameMs / frames, 1000.0 * frames / frameMs, appliedMs);
        frames = 0;
        frameMs = appliedMs = 0.0;
    }
};

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Sleeps on a high-resolution waitable timer, only with SF4E_LIMITER_SLEEP=1
// until frame-time captures show a late wake never costs a frame. Otherwise,
// or where that timer is missing (older Windows, some Wine builds), it returns
// at once and the game's limiter spins the whole slack as before.
void SleepBeforeLimiter(double ms) {
    static const bool enabled = sf4e::EnvFlag("SF4E_LIMITER_SLEEP");
    if (ms <= 0.0 || !enabled) return;
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(ms * 10000.0); // relative, in 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, (DWORD)ms + 5);
}

LimiterTest& Test() {
    static LimiterTest test = [] {
        LimiterTest t;
        char value[16] = {};
        const DWORD length = GetEnvironmentVariableA("SF4E_PACING_TEST_SHIFT_MS", value, sizeof(value));
        if (length > 0 && length < sizeof(value)) {
            t.enabled = true;
            t.shiftMs = atof(value);
            spdlog::warn("PacingTest: shifting every frame by {:.2f} ms; netplay pacing is off", t.shiftMs);
        }
        return t;
    }();
    return test;
}
}

void fD3D::RequestFrameShift(double ms) {
    s_frameShift.Request((int)(ms * 1000.0));
}

double fD3D::TakeAppliedShift() {
    return s_frameShift.TakeApplied() / 1000.0;
}

void fD3D::CancelFrameShift() {
    s_frameShift.Reset();
}

// The limiter waits until one period after its own previous exit, so a wait
// anywhere else in the frame only eats into that spin and the frame rate never
// changes. Rift pacing therefore moves the limiter's period for one frame.
int fD3D::LimitFrame(float frameDelta) {
    LimiterTest& test = Test();
    const auto taken = s_frameShift.Take();
    const double shiftMs = test.enabled ? test.shiftMs : taken.requestUs / 1000.0;
    float* period = rD3D::GetFramePeriodSeconds(this);
    const float savedPeriod = *period;
    // The first call has no previous exit to measure from.
    const unsigned long long previousExit = *rD3D::GetLastLimiterExit(this);
    if (savedPeriod <= 0.0f || previousExit == 0) {
        return (this->*rD3D::privateMethods.LimitFrame)(frameDelta);
    }
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double tickMs = 1000.0 / (double)frequency.QuadPart;
    const double periodMs = savedPeriod * 1000.0;
    const double elapsedMs = (double)(long long)(now.QuadPart - previousExit) * tickMs;
    if (shiftMs == 0.0 && !test.enabled) {
        SleepBeforeLimiter(sf4e::pacing::LimiterSleepMs(periodMs, elapsedMs));
        return (this->*rD3D::privateMethods.LimitFrame)(frameDelta);
    }
    const double shiftedPeriodMs = sf4e::pacing::ShiftedPeriodMs(periodMs, elapsedMs, shiftMs);
    const float shiftedPeriod = (float)(shiftedPeriodMs / 1000.0);
    *period = shiftedPeriod;
    SleepBeforeLimiter(sf4e::pacing::LimiterSleepMs(shiftedPeriodMs, elapsedMs));
    const int result = (this->*rD3D::privateMethods.LimitFrame)(frameDelta);
    // A display-settings change made meanwhile wins over the restore.
    if (*period == shiftedPeriod) {
        *period = savedPeriod;
    }
    const double frameMs = (double)(long long)(*rD3D::GetLastLimiterExit(this) - previousExit) * tickMs;
    const double appliedMs = sf4e::pacing::AppliedShiftMs(periodMs, frameMs, shiftMs);
    if (test.enabled) {
        test.Record(frameMs, appliedMs);
    }
    else {
        s_frameShift.Complete(taken.generation, (int)(appliedMs * 1000.0));
    }
    return result;
}

// Builds the present parameters for device creation and every Reset, so this
// is the one place VSync takes effect. It is forced off: with VSync on, Present
// waits for a vblank, so a pacing shift of a few milliseconds costs a whole
// refresh and the controller overcorrects. It also adds input latency.
void fD3D::BuildPresentParameters() {
    (this->*rD3D::privateMethods.BuildPresentParameters)();
    D3DPRESENT_PARAMETERS* parameters = rD3D::GetPresentParameters(this);
    if (parameters->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        s_vsyncForcedFrom = parameters->PresentationInterval;
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        LogVSyncForced();
    }
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
	// This hook is outside DllMain: worker creation and named-pipe IPC are safe.
	sf4e::NetplayFacade::StartHelper();
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
            spdlog::init_thread_pool(8192, 1);
            std::shared_ptr<spdlog::logger> logger(new spdlog::async_logger("sf4e", sinks.begin(), sinks.end(),
                spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest));
            spdlog::set_default_logger(logger);
            spdlog::flush_on(spdlog::level::info);
            spdlog::info("Welcome to sf4e");
            LogVSyncForced();
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

    sf4e::NetplayFacade::NotifyGameReady();

    return rval;
}

void fMain::Destroy() {
    fUserApp::ShutdownNetplay(true);
	sf4e::NetplayFacade::StopHelper();

    (this->*rMain::publicMethods.Destroy)();
	sf4e::OverlayPrefs::StopPersistence();
    spdlog::shutdown();
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
