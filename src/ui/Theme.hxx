#pragma once
#include <imgui.h>
#include <string>
#include <utility>

namespace sf4e { namespace ui {
enum class Tone { Neutral, Success, Pending, Error };
namespace palette {
constexpr ImU32 Ember = IM_COL32(255, 135, 56, 255);
constexpr ImU32 Ivory = IM_COL32(243, 235, 221, 255);
constexpr ImU32 Muted = IM_COL32(181, 169, 155, 255);
// Ready/confirmed. Matches ToneColor(Tone::Success) so a ready seat and a
// success status are the same green rather than two near-identical ones.
constexpr ImU32 Ready = IM_COL32(164, 206, 160, 255);
}

// Call between frames. A true result requires backend font texture recreation.
bool ApplyTheme(float dpiScale);
float Scale();
ImFont* HeadingFont();
ImFont* DiagnosticFont();
const char* FontLicense();
ImVec4 ToneColor(Tone tone);
void Header(const char* title, const char* subtitle = nullptr);
void MenuHeader(const char* title);
void Section(const char* title);
void Status(const char* text, Tone tone = Tone::Neutral);
void Text(const char* format, ...);
void FieldLabel(const char* label);
// Preserve native widget behavior while placing long labels above their fields.
// Hidden table-cell labels remain hidden and retain their existing IDs.
template<typename... Args> bool InputInt(const char* label, Args&&... args) {
    FieldLabel(label);
    return ImGui::InputInt((label[0] == '#' ? std::string(label) : "##" + std::string(label)).c_str(), std::forward<Args>(args)...);
}
template<typename... Args> bool InputText(const char* label, Args&&... args) {
    FieldLabel(label);
    return ImGui::InputText((label[0] == '#' ? std::string(label) : "##" + std::string(label)).c_str(), std::forward<Args>(args)...);
}
template<typename... Args> bool Combo(const char* label, Args&&... args) {
    FieldLabel(label);
    return ImGui::Combo((label[0] == '#' ? std::string(label) : "##" + std::string(label)).c_str(), std::forward<Args>(args)...);
}
void ConstrainNextWindow(ImVec2 preferredSize);
void KeepWindowVisible();
bool BeginToolWindow(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);
void EndToolWindow();

// Presentation-only input. Neither this view nor its renderer accesses the game.
struct MatchStripView {
    std::string names[2];
    unsigned rollbackFrames = 0;
    int pingMs = -1, appliedDelay = -1, size = 1;
    bool spectator = false, raised = false;
};
void DrawMatchStrip(const MatchStripView& view);
// Exposed for the render harness: Small/Standard/Large must not collapse.
float MatchStripScale(const MatchStripView& view);
void DrawMatchStripPreview(const MatchStripView& view);
void DrawControllerWarning(const std::string& message);
struct DiagnosticStripView {
    bool hasRemote = false, networkAvailable = false;
    int network[6] = {}; // RTT, kbps sent, receive/send queues, local/remote frames behind.
    int pendingSnapshots = -1, snapshotCount = 0;
};
void DrawDiagnosticStrip(const DiagnosticStripView& view);
} }
