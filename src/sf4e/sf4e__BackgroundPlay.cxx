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
#include "sf4e__CodePages.hxx"

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
// The player's setting, published by each pad update.
std::atomic<bool> enabled{false};

bool Active() { return ready.load() && enabled.load(); }

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

// What the app's frame sees as the foreground window. With background play
// it is the game's own, so the frame keeps the sound up, and restores a mute
// from before (at startup behind another window, or with the setting off).
HWND WINAPI SoundForeground() {
    if (Active())
        if (const rMain::Win32_WindowData* data = WindowData()) return data->hWnd;
    return GetForegroundWindow();
}
// The frame's `call dword ptr [...]` reads its function from here.
HWND (WINAPI* soundForeground)() = SoundForeground;

gate::Edit edits[3];

struct AppMessages : rApp {
    unsigned int HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
};

// Losing focus mutes the game here as well as in the frame. Skipping it keeps
// the sound from dropping for the one frame before the frame restores it.
unsigned int AppMessages::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KILLFOCUS && Active()) {
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
        if (!enabled.load()) { readBehind = false; return 0; }
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
    sf4e::CodePages pages;
    if (!gate::ApplyAll(edits, pages)) {
        unavailable = "the game's focus checks could not be patched";
        return;
    }
    unavailable = nullptr;
    ready.store(true);
}

void sf4e::BackgroundPlay::BeforePadUpdate(rPad::System* system, bool on) {
    enabled.store(on);
    static bool reported = false;
    if (!reported) {
        reported = true;
        if (unavailable) spdlog::warn("Background play: unavailable, {}", unavailable);
        else spdlog::info("Background play: available");
    }
    if (!ready.load() || !*rPad::System::GetUpdating(system) || WindowInFront()) return;
    if (!on) {
        (system->*rPad::System::publicMethods.ClearInputs)(0, 0, 0, 0, 1);
        return;
    }
    // Key releases do not reach a window behind another, so a key held at
    // alt-tab would stay down. The pads stay live.
    if (auto* keyboards = rPad::System_RawInput::staticMethods.GetSingleton())
        (keyboards->*rPad::System_RawInput::publicMethods.ClearKeys)();
}
