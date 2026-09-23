#pragma once
#include <imgui.h>
#include <cstdint>
#include <string>
#include <utility>

namespace sf4e { namespace ui {
// ImGui's default Latin range stops before typographic punctuation. Keep the
// HUD's unavailable marker and UTF-8 player names in the baked atlas. Header
// scope so the catalog test can check glyph coverage without linking the UI.
constexpr ImWchar UiGlyphRanges[] = {0x0020, 0x017F, 0x2013, 0x2014, 0x2018, 0x201D, 0x2026, 0x2026, 0};
enum class Tone { Neutral, Success, Pending, Error };
namespace palette {
constexpr ImU32 Ember = IM_COL32(255, 135, 56, 255);
constexpr ImU32 Ivory = IM_COL32(243, 235, 221, 255);
constexpr ImU32 Muted = IM_COL32(181, 169, 155, 255);
// Ready/confirmed. Matches ToneColor(Tone::Success) so a ready seat and a
// success status are the same green rather than two near-identical ones.
constexpr ImU32 Ready = IM_COL32(164, 206, 160, 255);
}
// The focused button of a modal dialog, and the other one.
inline ImVec4 DialogButtonColor(bool selected) { return selected ? ImVec4(.5f,.25f,.1f,1) : ImVec4(.15f,.14f,.13f,1); }
// Row and button text for an entry that can, or cannot, be activated right now.
inline ImVec4 EntryTextColor(bool enabled) { return enabled ? ImVec4(.95f,.92f,.87f,1) : ImVec4(.55f,.52f,.48f,1); }

// Call between frames. A true result requires backend font texture recreation.
bool ApplyTheme(float dpiScale);
float Scale();
ImFont* HeadingFont();
ImFont* DiagnosticFont();
const char* FontLicense();
ImVec4 ToneColor(Tone tone);
void Header(const char* title, const char* subtitle = nullptr, bool brand = false);
void MenuHeader(const char* title, bool home = false);
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
    // Running win count (SetScoreText) shown in place of "vs"; empty when there is none.
    std::string score;
    unsigned rollbackFrames = 0;
    int pingMs = -1, appliedDelay = -1, size = 1;
    bool spectator = false, raised = false;
    // Link state and the latest netplay notice, drawn on a line above the
    // telemetry. Severity: 0 info, 1 warning, 2 error (matches NoticeSeverity).
    std::string notice;
    int noticeSeverity = 0;
    bool connectionWarning = false, predictionStalled = false;
    int disconnectCountdownMs = -1;
};
void DrawMatchStrip(const MatchStripView& view);
// "2 - 1": a room pair's running win count, as the table card and HUD show it.
std::string SetScoreText(const std::uint32_t (&score)[2]);
// The single line the strip shows for the link state, or empty. Exposed so
// the render harness and tests can check the wording without a draw list.
std::string MatchStripStateLine(const MatchStripView& view);
// A notice without the telemetry strip (match HUD hidden by the player).
void DrawMatchNotice(const std::string& message, int severity);
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
