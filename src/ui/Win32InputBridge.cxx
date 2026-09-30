#include "Win32InputBridge.hxx"

namespace sf4e { namespace ui {
void ApplyWin32Input(ImGuiIO& io, const Win32Input& input) {
    switch (input.kind) {
    case Win32Input::MouseSource: io.AddMouseSourceEvent(static_cast<ImGuiMouseSource>(input.a)); break;
    case Win32Input::MousePos: io.AddMousePosEvent(input.x, input.y); break;
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

void DeliverWin32Input(Win32InputBridge* bridge, ImGuiIO& io, const Win32Input& input) {
    if (bridge) bridge->Push(input);
    else ApplyWin32Input(io, input);
}

namespace { std::atomic<bool> cursorOwned{true}; }
bool Win32CursorOwned() { return cursorOwned; }
void SetWin32CursorOwned(bool owned) { cursorOwned = owned; }

void Win32InputBridge::Push(const Win32Input& input) {
    std::lock_guard<std::mutex> hold(lock_);
    queued_.push_back(input);
}

void Win32InputBridge::RequestClear() {
    std::lock_guard<std::mutex> hold(lock_);
    queued_.clear();
    clear_ = true;
}

void Win32InputBridge::ApplyTo(ImGuiIO& io) {
    std::vector<Win32Input> input;
    bool clear = false;
    {
        std::lock_guard<std::mutex> hold(lock_);
        input.swap(queued_);
        clear = clear_;
        clear_ = false;
    }
    if (clear) { io.ClearInputKeys(); io.ClearInputMouse(); }
    for (const auto& event : input) ApplyWin32Input(io, event);
}
} }
