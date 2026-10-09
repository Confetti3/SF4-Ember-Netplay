#pragma once
#include <functional>
#include <imgui.h>
#include "../common/NetworkLink.hxx"
#include "MenuNavigation.hxx"
#include "FontMetrics.hxx"
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace sf4e { namespace ui {
// ImGui's default Latin range stops before typographic punctuation. Keep the
// HUD's unavailable marker and Latin and Cyrillic player names in the baked
// atlas, with the low quotes German, Polish and Czech open with. Characters
// beyond these (Chinese, Japanese and Korean names and chat) are added when
// text uses them: NoteUserText. Header scope so the catalog test can check
// glyph coverage without linking the UI.
constexpr ImWchar UiGlyphRanges[] = {0x0020, 0x017F, 0x0400, 0x04FF, 0x2013, 0x2014, 0x2018, 0x201E, 0x2026, 0x2026, 0};
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
// What player text is, from most to least worth atlas space when more distinct
// characters are on screen than the atlas can hold.
enum class UserTextRole { Name, Chat, Draft };
// Text a player wrote (a name, a room name, a message) that is about to be
// drawn, every frame it is drawn. The CJK fonts hold far more characters than
// the atlas bakes, so any character outside Inter's ranges is wanted by the
// atlas while text keeps using it; the next ApplyTheme rebuilds it (at most
// every quarter second) and the text draws with real glyphs from then on.
// Characters nothing has drawn for a while stop being wanted, and the next
// rebuild drops them, so the atlas follows what is on screen rather than
// everything ever seen. Cheap for text with nothing to add.
void NoteUserText(const std::string& utf8, UserTextRole role = UserTextRole::Name);
// The same for an entry's detail, which is player text only when the entry
// says so. Called where the detail is drawn.
inline void NoteDetailText(const std::string& detail, DetailText kind) {
    if (kind == DetailText::Name) NoteUserText(detail);
    else if (kind == DetailText::Chat) NoteUserText(detail, UserTextRole::Chat);
}
// The shortest time between two rebuilds for player text; tests widen it to
// see a rebuild wait and narrow it to see it run.
void SetUserGlyphRebuildInterval(std::chrono::milliseconds interval);
// Hears one line per font atlas rebuild (size, glyphs players' text added, and
// whether the build completed), for the game's log.
void SetAtlasBuildLog(std::function<void(const char*)> log);
// How long a character stays wanted after text last drew it; tests shorten it
// to retire text.
void SetUserGlyphRetention(std::chrono::milliseconds retention);
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
// Opaque foreground cover; leaves the native loading pipeline running.
void DrawMatchLoading();

// Ember's link marks in a size-by-size square at `min`: an Ethernet port in
// the ready green, Wi-Fi arcs in the accent colour since it is the link to keep
// an eye on, and a muted question ring when the link is unknown.
void DrawNetworkLinkGlyph(ImDrawList* draw, ImVec2 min, float size, NetworkLink link);
// The same, in words ("Wired connection").
const char* NetworkLinkName(NetworkLink link);

// Presentation-only input. Neither this view nor its renderer accesses the game.
struct MatchStripView {
    std::string names[2];
    NetworkLink links[2] = {};
    // Running win count (SetScoreText) shown in place of "vs"; empty when there is none.
    std::string score;
    unsigned rollbackFrames = 0;
    int pingMs = -1, appliedDelay = -1, size = 1;
    bool spectator = false, raised = false;
    // Where the strip sits: 0 bottom center, 1 bottom left, 2 bottom right, 3 top left, 4 top right.
    // Raised moves it further in from whichever edge it is anchored to.
    int anchor = 0;
    // 0 the Ember strip (one panel); 1 split: each name on a plate above its life bar,
    // and a small telemetry panel placed by `anchor`/`raised`.
    int layout = 0;
    // Split layout: the name plates' shift from the default PLAYER label row, in 720p
    // game units (positive is down), for a game whose own HUD position was changed.
    int nameOffset = 0;
    // Members watching this match; the split layout shows "Watching N" when above zero.
    int spectators = 0;
    // Link state and the latest netplay notice, drawn on a line above the
    // telemetry. Severity: 0 info, 1 warning, 2 error (matches NoticeSeverity).
    std::string notice;
    int noticeSeverity = 0;
    bool connectionWarning = false, predictionStalled = false;
    int disconnectCountdownMs = -1;
};
// The match HUD functions below, MatchStripGeometry and the label helpers are defined in MatchHud.cxx.
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
// Screen rectangles the split layout (view.layout 1) draws into, for the render
// harness: the two name plates and the telemetry panel with its state line.
// `valid` is false for the Ember strip, which has no separate name plates.
struct MatchStripBox { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool valid = false; };
// panelScale is the telemetry panel's scale; panelBelow is set when a top-anchored panel
// went to the bottom corner because names moved up left too little room above them.
struct MatchStripBounds { MatchStripBox names[2], panel; float panelScale = 0; bool panelBelow = false; };
MatchStripBounds MatchStripGeometry(const MatchStripView& view);
// The same on a screen of any size, for checking placement on screens the harness does not render.
MatchStripBounds MatchStripGeometry(const MatchStripView& view,ImVec2 screenPos,ImVec2 screenSize);
// The settings label for a MatchStripView::layout value ("Ember strip", "Split").
const char* MatchStripLayoutName(int layout);
// The settings label for a MatchStripView::anchor value ("Bottom center", "Top right").
const char* MatchStripAnchorName(int anchor);
// "Default", "Up 20" or "Down 6": the split layout's name offset (MatchStripView::nameOffset).
std::string MatchHudNameOffsetText(int offset);
void DrawControllerWarning(const std::string& message);
struct DiagnosticStripView {
    bool hasRemote = false, networkAvailable = false;
    int network[6] = {}; // RTT, kbps sent, receive/send queues, local/remote frames behind.
    int pendingSnapshots = -1, snapshotCount = 0;
};
void DrawDiagnosticStrip(const DiagnosticStripView& view);
} }
