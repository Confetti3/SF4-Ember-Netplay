#include <windows.h>
#include <detours/detours.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../common/FocusGate.hxx"
#include "sf4e__BackgroundPlay.hxx"

namespace rPad = Dimps::Pad;
using rApp = Dimps::App;
using rMain = Dimps::Platform::Main;
namespace gate = sf4e::focus_gate;

namespace {
// Both detours are queued: Activate may make its edits once they commit.
bool hooked = false;
// Why the feature is off, or null once ready. Install and Activate run in
// DllMain, before the log exists, so the first pad update reports this.
const char* unavailable = "its hooks did not commit";
// The detours committed and every edit is in place. Until then every hook
// passes straight to the game, which keeps its own checks.
std::atomic<bool> ready{false};
// The caller's policy (BackgroundPlay::Policy), published by each pad update.
std::atomic<bool> backgroundInput{false}, keepSound{false}, keepRunning{false}, keepSoundMinimized{false};
std::atomic<HWND> gameWindow{nullptr};

bool KeepSound() { return ready.load() && keepSound.load(); }
bool KeepRunning() { return ready.load() && keepRunning.load(); }
bool KeepSoundMinimized() { return ready.load() && keepSoundMinimized.load(); }

const rMain::Win32_WindowData* WindowData() {
    rMain* main = rMain::staticMethods.GetSingleton();
    return main ? *rMain::GetWindowData(main) : nullptr;
}

// The pad gates' own test: the foreground window is the game's and the
// window procedure saw it take focus.
bool WindowInFront() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    if (!rMain::staticMethods.GetSingleton()) return true;
    const rMain::Win32_WindowData* data = WindowData();
    return data && foreground == data->hWnd && data->hasFocus != 0;
}

// What the app's frame sees as the foreground window. While the sound is
// kept it is the game's own, so the frame keeps the sound up, and restores a
// mute from before (at startup behind another window, or with the setting off).
HWND WINAPI SoundForeground() {
    if (KeepSound())
        if (const HWND window = gameWindow.load()) return window;
    return GetForegroundWindow();
}
// The frame's `call dword ptr [...]` reads its function from here.
HWND (WINAPI* soundForeground)() = SoundForeground;

// While the game is kept running, it is told its window is in front and not
// minimized: the frame's own "active" flag stays set, so the battle goes on
// behind another window. Whether it is heard there is the sound checks' own
// answer. Otherwise these answer as Windows does.
HWND WINAPI ActiveForeground() {
    if (KeepRunning())
        if (const HWND window = gameWindow.load()) return window;
    return GetForegroundWindow();
}
HWND (WINAPI* activeForeground)() = ActiveForeground;
BOOL WINAPI ActiveIconic(HWND window) { return KeepRunning() ? FALSE : IsIconic(window); }
BOOL WINAPI SoundIconic(HWND window) { return KeepSoundMinimized() ? FALSE : IsIconic(window); }
BOOL (WINAPI* windowIconic[2])(HWND) = { ActiveIconic, SoundIconic };

// focus_gate::ApplyAll's access to the game's code. A protection that cannot
// be put back leaves the bytes writable; they are already final.
struct CodePages {
    bool Unlock(std::uint8_t* at, std::uint8_t length, unsigned long& saved) {
        DWORD old = 0;
        if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old)) return false;
        saved = old;
        return true;
    }
    void Lock(std::uint8_t* at, std::uint8_t length, unsigned long saved) {
        DWORD old = 0;
        VirtualProtect(at, length, saved, &old);
        FlushInstructionCache(GetCurrentProcess(), at, length);
    }
};

gate::Edit edits[6];

struct AppMessages : rApp {
    unsigned int HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
};

// Losing focus mutes the game here as well as in the frame. Skipping it keeps
// the sound from dropping for the one frame before the frame restores it.
unsigned int AppMessages::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KILLFOCUS && KeepSound()) {
        spdlog::info("Background play: kept the game's sound as its window lost focus");
        return 0;
    }
    return (this->*rApp::publicMethods.HandleMessage)(window, message, wParam, lParam);
}

struct PadPoll : rPad::System_XInput {
    int Update();
};

int PadPoll::Update() {
    static bool readBehind = false;
    if (ready.load() && !WindowInFront()) {
        // What the native gate did: no poll while the window is behind.
        if (!backgroundInput.load()) { readBehind = false; return 0; }
        if (!readBehind) spdlog::info("Background play: reading the pads while another window is in front");
        readBehind = true;
    } else {
        readBehind = false;
    }
    return (this->*rPad::System_XInput::publicMethods.Update)();
}
}

void sf4e::BackgroundPlay::Install() {
    edits[0] = gate::Gate(rPad::System::focusGate, gate::kUpdateSkip);
    edits[1] = gate::Gate(rPad::System_XInput::focusGate, gate::kPollSkip);
    edits[2] = gate::CallThrough(rApp::soundFocusCheck, rApp::foregroundWindowImport,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&soundForeground)));
    edits[3] = gate::CallThrough(rApp::activeFocusCheck, rApp::foregroundWindowImport,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&activeForeground)));
    for (int i = 0; i < 2; i++)
        edits[4 + i] = gate::CallThrough(rApp::iconicChecks[i], rApp::iconicImport,
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&windowIconic[i])));
    for (const gate::Edit& edit : edits) {
        if (std::memcmp(edit.at, edit.native, edit.length) != 0) {
            unavailable = "the game's focus checks hold other bytes than this game build's";
            return;
        }
    }
    unsigned int (AppMessages::* handleMessage)(HWND, UINT, WPARAM, LPARAM) = &AppMessages::HandleMessage;
    int (PadPoll::* update)() = &PadPoll::Update;
    hooked = DetourAttach((PVOID*)&rApp::publicMethods.HandleMessage, *(PVOID*)&handleMessage) == NO_ERROR &&
        DetourAttach((PVOID*)&rPad::System_XInput::publicMethods.Update, *(PVOID*)&update) == NO_ERROR;
    if (!hooked) unavailable = "its hooks could not be attached";
}

void sf4e::BackgroundPlay::Activate() {
    if (!hooked) return;
    CodePages pages;
    if (!gate::ApplyAll(edits, pages)) {
        unavailable = "the game's focus checks could not be patched";
        return;
    }
    unavailable = nullptr;
    ready.store(true);
}

void sf4e::BackgroundPlay::BeforePadUpdate(rPad::System* system, const Policy& policy) {
    const rMain::Win32_WindowData* const data = WindowData();
    gameWindow.store(data ? data->hWnd : nullptr);
    keepSound.store(policy.keepSound);
    keepRunning.store(policy.keepRunning);
    keepSoundMinimized.store(policy.keepSoundMinimized);
    backgroundInput.store(policy.backgroundInput);
    static bool reported = false;
    if (!reported) {
        reported = true;
        if (unavailable) spdlog::warn("Background play: unavailable, {}", unavailable);
        else spdlog::info("Background play: available");
    }
    if (!ready.load() || !*rPad::System::GetUpdating(system) || WindowInFront()) return;
    if (!policy.backgroundInput) {
        (system->*rPad::System::publicMethods.ClearInputs)(0, 0, 0, 0, 1);
        return;
    }
    // Key releases do not reach a window behind another, so a key held at
    // alt-tab would stay down. The pads stay live.
    if (auto* keyboards = rPad::System_RawInput::staticMethods.GetSingleton())
        (keyboards->*rPad::System_RawInput::publicMethods.ClearKeys)();
}
