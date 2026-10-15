#include "Win32InputBridge.hxx"
#include <cfloat>

namespace sf4e { namespace ui {
void ApplyWin32Input(ImGuiIO& io, const Win32Input& input, ImVec2 mouseScale) {
    switch (input.kind) {
    case Win32Input::MouseSource: io.AddMouseSourceEvent(static_cast<ImGuiMouseSource>(input.a)); break;
    case Win32Input::MousePos:
        // -FLT_MAX means no mouse and stays that way.
        io.AddMousePosEvent(input.x > -FLT_MAX ? input.x * mouseScale.x : input.x, input.y > -FLT_MAX ? input.y * mouseScale.y : input.y);
        break;
    case Win32Input::MouseButton: io.AddMouseButtonEvent(input.a, input.b != 0); break;
    case Win32Input::MouseWheel: io.AddMouseWheelEvent(input.x, input.y); break;
    case Win32Input::Key:
        io.AddKeyEvent(static_cast<ImGuiKey>(input.a), input.b != 0);
        if (input.nativeKey >= 0) io.SetKeyEventNativeData(static_cast<ImGuiKey>(input.a), input.nativeKey, -1);
        break;
    case Win32Input::Focus: io.AddFocusEvent(input.a != 0); break;
    case Win32Input::CharUTF16: io.AddInputCharacterUTF16(static_cast<ImWchar16>(input.a)); break;
    case Win32Input::Char: io.AddInputCharacter(static_cast<unsigned int>(input.a)); break;
    }
}

void DeliverWin32Input(Win32InputBridge* bridge, ImGuiIO& io, const Win32Input& input, ImVec2 mouseScale) {
    if (bridge) bridge->Push(input);
    else ApplyWin32Input(io, input, mouseScale);
}

namespace { std::atomic<bool> cursorOwned{true}; }
bool Win32CursorOwned() { return cursorOwned; }
void SetWin32CursorOwned(bool owned) { cursorOwned = owned; }

void Win32InputBridge::Push(const Win32Input& input) {
    std::lock_guard<std::mutex> hold(lock_);
    if (queued_.size() >= MaxQueuedEvents) {
        Win32Input focus;
        bool focusQueued = false;
        for (const auto& event : queued_) if (event.kind == Win32Input::Focus) { focus = event; focusQueued = true; }
        queued_.clear();
        clear_ = true;
        if (focusQueued) queued_.push_back(focus);
        ++overflows_;
    }
    queued_.push_back(input);
}

void Win32InputBridge::RequestClear() {
    std::lock_guard<std::mutex> hold(lock_);
    queued_.clear();
    clear_ = true;
}

void Win32InputBridge::ApplyTo(ImGuiIO& io, ImVec2 mouseScale) {
    std::vector<Win32Input> input;
    bool clear = false;
    {
        std::lock_guard<std::mutex> hold(lock_);
        input.swap(queued_);
        clear = clear_;
        clear_ = false;
    }
    if (clear) { io.ClearInputKeys(); io.ClearInputMouse(); }
    for (const auto& event : input) ApplyWin32Input(io, event, mouseScale);
}
} }
