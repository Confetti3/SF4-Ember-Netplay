#pragma once
#include <imgui.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace sf4e { namespace ui {
// One ImGui input event the Win32 backend read from a window message.
struct Win32Input {
    enum Kind { MouseSource, MousePos, MouseButton, MouseWheel, Key, Focus, CharUTF16, Char } kind;
    int a = 0, b = 0;
    int nativeKey = -1; // Key only; modifier keys (ImGuiMod_*) have none
    float x = 0.0f, y = 0.0f;
    static Win32Input Of(Kind kind, int a = 0, int b = 0, float x = 0.0f, float y = 0.0f) {
        Win32Input input; input.kind = kind; input.a = a; input.b = b; input.x = x; input.y = y;
        return input;
    }
    static Win32Input KeyEvent(ImGuiKey key, bool down, int nativeKey) {
        Win32Input input = Of(Key, key, down); input.nativeKey = nativeKey;
        return input;
    }
};
void ApplyWin32Input(ImGuiIO& io, const Win32Input& input);

// SF4 can deliver window messages on another thread than the one that draws.
// Handed this bridge (ImGui_ImplWin32_SetInputBridge), the Win32 backend keeps
// its Win32 work on the message thread and puts the ImGui input it reads here;
// the drawing thread applies it, in order, at the start of its NewFrame. The
// lock covers only the queue, never a Win32 call, so a window procedure that
// Windows enters again from inside the backend (WM_CAPTURECHANGED during
// ReleaseCapture) never waits on it.
class Win32InputBridge {
public:
    void Push(const Win32Input& input);
    // Releases every key and button. Input queued before is dropped; input
    // queued after still applies.
    void RequestClear();
    // Drawing thread.
    void ApplyTo(ImGuiIO& io);
private:
    std::mutex lock_;
    std::vector<Win32Input> queued_;
    bool clear_ = false;
};
// The backend's route for one event: into the bridge when it has one,
// otherwise straight to io.
void DeliverWin32Input(Win32InputBridge* bridge, ImGuiIO& io, const Win32Input& input);

// Whether the backend may set the OS cursor. Set from either thread, so it is
// kept here rather than in io.ConfigFlags.
bool Win32CursorOwned();
void SetWin32CursorOwned(bool owned);
} }
