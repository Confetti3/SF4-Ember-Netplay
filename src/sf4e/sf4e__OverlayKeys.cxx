#include "sf4e__OverlayKeys.hxx"
#include <imgui.h>
#include "sf4e__ReplayPlayback.hxx"
#include "../common/ReplayTransport.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../ui/Win32Input.hxx"

namespace sf4e { namespace overlay {
bool KeepsKeyFromGame(UINT message, WPARAM w, LPARAM l, const KeyOwners& owners) {
    // The position hotkeys only as plain keys, so Alt+F4 still reaches the game.
    const bool plainKey = message == WM_KEYDOWN || message == WM_KEYUP, systemKey = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
    if (replaytransport::KeepsKey(owners.replayPlayback, message, static_cast<unsigned>(w))) return true;
    if (ui::KeepsAltEnter(message, w, l, owners.watchingStream)) return true;
    return owners.trainingAvailable && !owners.replayPlayback && ((w >= VK_F5 && w <= VK_F8 && (plainKey || systemKey)) ||
        (plainKey && w >= VK_F1 && w <= VK_F12 && ui::TrainingHotkeyBound(static_cast<int>(w - VK_F1))));
}

void SendReplayKeys(std::uint32_t session) {
    using replaytransport::Command;
    const auto send = [session](Command command) { replayplayback::Submit(command, replaytransport::Device::Keyboard, session); };
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) send(Command::TogglePause);
    // Held, F2 steps again after 300 ms, ten times a second.
    for (int steps = ImGui::GetKeyPressedAmount(ImGuiKey_F2, .3f, .1f); steps > 0; --steps) send(Command::Step);
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) send(Command::Slower);
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) send(Command::Faster);
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) send(Command::Meter);
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) send(Command::Inputs);
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) send(Command::TakeoverPrimary);
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) send(Command::TakeoverSecondary);
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) send(Command::TakeoverOther);
}
} }
