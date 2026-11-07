#include "CameraPanel.hxx"
#include "V2.hxx"
#include "../training/CameraControl.hxx"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace sf4e { namespace ui {
namespace camera = sf4e::training::camera;
namespace {
bool s_open = false, s_fileSet = false;
// The mouse jog: while on, the mouse over the picture moves the camera. Holding Ctrl does the same without it.
bool s_mouse = false;
// The panel and the timeline can each shrink to their header row.
bool s_panelMin = false, s_barMin = false;
// How solid the panel and the timeline are; low, the picture shows through them.
float s_opacity = 0.9f;
float s_reserve = 0;
// Over a replay the panel is either the video editor (timeline, shot keys, the replay controls inside) or just the camera
// beside the plain replay controls' strip.
bool s_editor = false;
constexpr float PanelWidth = 312;
// Numpad and number-row keys: the arrows and letters are what keyboard players fight with.
const ImGuiKey ViewKeys[camera::ViewSlots] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4};
struct EffectKey { ImGuiKey key; const char* id; const char* label; };
const EffectKey Effects[] = {{ImGuiKey_5, "push", "Push in"}, {ImGuiKey_6, "pull", "Pull out"}, {ImGuiKey_7, "orbit", "Orbit 360"},
    {ImGuiKey_8, "crash", "Crash zoom"}, {ImGuiKey_9, "vertigo", "Vertigo"}, {ImGuiKey_0, "dutch", "Dutch tilt"}};
void Order(const nlohmann::json& body) { std::string error; camera::Phone().Order(body, error); }
// Held keys move the camera by the frame's time, so the speed does not depend on the frame rate.
void HeldKeys(float dt) {
    const bool shift = ImGui::IsKeyDown(ImGuiMod_Shift), slow = ImGui::IsKeyDown(ImGuiMod_Alt);
    const float speed = slow ? 0.25f : 1.f;
    const auto held = [](ImGuiKey key) { return ImGui::IsKeyDown(key) ? 1.f : 0.f; };
    const float zoom = held(ImGuiKey_KeypadSubtract) - held(ImGuiKey_KeypadAdd) + held(ImGuiKey_PageDown) - held(ImGuiKey_PageUp);
    const float roll = held(ImGuiKey_Keypad9) - held(ImGuiKey_Keypad7);
    // The right stick, as ImGui's backend reads it from the pad (dead zone already taken off); squared, so small pushes are fine moves.
    const auto stick = [](ImGuiKey key) { return ImGui::GetKeyData(key)->AnalogValue; };
    float padSide = stick(ImGuiKey_GamepadRStickRight) - stick(ImGuiKey_GamepadRStickLeft), padUp = stick(ImGuiKey_GamepadRStickUp) - stick(ImGuiKey_GamepadRStickDown);
    padSide *= std::abs(padSide); padUp *= std::abs(padUp);
    const float side = std::clamp(held(ImGuiKey_Keypad6) - held(ImGuiKey_Keypad4) + padSide, -1.f, 1.f), up = std::clamp(held(ImGuiKey_Keypad8) - held(ImGuiKey_Keypad2) + padUp, -1.f, 1.f);
    if (!side && !up && !zoom && !roll) return;
    camera::Offsets by; by.zoom = std::exp(zoom * dt * speed);
    if (shift) { by.panX = side * 2.5f * dt * speed; by.panY = up * 2.5f * dt * speed; }
    else { by.yaw = side * 1.5f * dt * speed; by.pitch = up * 1.0f * dt * speed; }
    by.roll = roll * 0.8f * dt * speed;
    camera::Phone().Nudge(by);
}
// CAD-style mouse over the picture (not over any window), when the jog is on or Ctrl is held: left drag orbits, middle / right / Shift drag pans, wheel zooms.
// A drag that began on a window (a slider) is left to that window.
void Mouse() {
    const auto& io = ImGui::GetIO();
    static bool grab = false;
    const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    const bool on = s_mouse || io.KeyCtrl;
    if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2)) grab = on && !over;
    camera::Offsets by;
    if (io.MouseWheel != 0 && on && !over) by.zoom = std::exp(-io.MouseWheel * 0.1f);
    const bool pan = ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2) || (io.KeyShift && ImGui::IsMouseDown(0));
    if (grab && (pan || ImGui::IsMouseDown(0))) {
        if (pan) { by.panX = -io.MouseDelta.x * 0.01f; by.panY = io.MouseDelta.y * 0.01f; }
        else { by.yaw = -io.MouseDelta.x * 0.008f; by.pitch = io.MouseDelta.y * 0.008f; }
    }
    if (!by.Identity()) camera::Phone().Nudge(by);
}
// A scrub by frames from where the replay is, or from where a jump is already going so quick steps add up.
// Scrubbing pauses the replay first, as an editor does; the pause and the jump reach the playback together.
// One toggle at a time: the playback's view of "paused" lags a frame or two, and a second toggle would play it again.
void Pause(const ReplayBar& bar) {
    static int last = -100;
    const int now = ImGui::GetFrameCount();
    if (bar.paused || !bar.send || now - last < 6) return;
    last = now; bar.send(replaytransport::Command::TogglePause);
}
void Jump(const ReplayBar& bar, int frame) { Pause(bar); camera::Phone().AskSeek(frame); }
void StepBack(const ReplayBar& bar, int frames) {
    camera::Control& phone = camera::Phone();
    int round = 0, goal = 0;
    const int from = phone.Goal(round, goal) ? goal : (std::max)(phone.Frame(), 0);
    Jump(bar, (std::max)(phone.Floor(), (std::min)(from - frames, (std::max)(phone.Total() - 1, 0))));
}
void Keys(const camera::Settings& now, const ReplayBar& bar) {
    const auto& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    HeldKeys(io.DeltaTime);
    Mouse();
    const bool ctrl = io.KeyCtrl;
    for (int i = 0; i < camera::ViewSlots; ++i)
        if (ImGui::IsKeyPressed(ViewKeys[i], false)) Order(ctrl ? nlohmann::json{{"saveView", i}} : nlohmann::json{{"goView", i}});
    if (!ctrl)
        for (const auto& e : Effects) if (ImGui::IsKeyPressed(e.key, false)) Order({{"effect", e.id}});
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) Order({{"reset", true}});
    if (ImGui::IsKeyPressed(ImGuiKey_Insert, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR3, false)) Order({{"free", !now.free}});
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) s_mouse = !s_mouse;
    if (bar.on && s_editor) {
        // Held, the comma steps back again after 300 ms, ten times a second; the wheel over the timeline scrubs too.
        for (int steps = ImGui::GetKeyPressedAmount(ImGuiKey_Comma, .3f, .1f); steps > 0; --steps) StepBack(bar, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_K, false)) Order({{"addKey", {{"at", camera::OffsetsJson(now.live)}, {"ease", 1}}}});
        const int frame = camera::Phone().Frame();
        for (const auto& k : now.keys) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false) && k.frame > frame + 1) { Jump(bar, k.frame); break; }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false))
            for (auto k = now.keys.rbegin(); k != now.keys.rend(); ++k) if (k->frame < frame - 1) { Jump(bar, k->frame); break; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) Order({{"tour", true}});
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) Order({{"stopGlide", true}});
}
// One offset as a slider, in the unit the player reads (degrees, times, units); a drag is a hand on the camera.
void Row(const char* label, const char* id, float& value, float shown, float low, float high, const char* format, float unit) {
    float edit = shown;
    ImGui::TextUnformatted(label); ImGui::SameLine(ImGui::GetWindowWidth() * 0.27f);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat(id, &edit, low, high, format)) { value = edit * unit; }
}
// The selected key of the timeline and the shot: its ease, update, delete, and the render.
void KeyRows(const camera::Settings& s, const ReplayBar& bar, float scale) {
    const auto& c = v2::Colors();
    ImGui::Separator();
    const bool picked = s.selected >= 0 && s.selected < static_cast<int>(s.keys.size());
    ImGui::PushStyleColor(ImGuiCol_Text, c.muted);
    if (picked) ImGui::Text("SHOT KEY %d  (frame %d)", s.selected + 1, s.keys[s.selected].frame); else ImGui::TextUnformatted("SHOT KEYS  click a diamond to pick one");
    ImGui::PopStyleColor();
    if (picked) {
        const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
        static const char* names[] = {"Linear", "Smooth", "Hold"};
        for (int e = 0; e < 3; ++e) {
            if (e) ImGui::SameLine();
            const bool on = static_cast<int>(s.keys[s.selected].ease) == e;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, c.accent);
            if (ImGui::Button(names[e], ImVec2(third, 0))) Order({{"easeKey", {{"key", s.selected}, {"ease", e}}}});
            if (on) ImGui::PopStyleColor();
        }
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Update key from sliders", ImVec2(half, 0))) Order({{"updateKey", s.selected}});
        ImGui::SameLine();
        if (ImGui::Button("Delete key", ImVec2(-1, 0))) Order({{"delKey", s.selected}});
    }
    const auto render = camera::Phone().GetRender();
    const bool ready = render == camera::Render::Ready && s.keys.size() >= 2;
    const char* label = render == camera::Render::Ready ? (ready ? "Render shot to MP4" : "Render shot (needs two keys)") :
        render == camera::Render::Done ? "Shot saved. Render again" : render == camera::Render::Failed ? "Render failed. Try again" :
        render == camera::Render::Unavailable ? "Render needs a replay from Ember's Replays" : "Rendering the shot...";
    const bool again = render == camera::Render::Done || render == camera::Render::Failed;
    if (!ready && !again) ImGui::BeginDisabled();
    if (ImGui::Button(label, ImVec2(-1, 0))) Order({{"render", true}});
    if (!ready && !again) ImGui::EndDisabled();
    (void)bar; (void)scale;
}
// The timeline under a replay: the transport, a bar for the round with the shot keys on it, and the jump.
void DrawTimeline(const camera::Settings& s, const ReplayBar& bar, float scale) {
    const auto* vp = ImGui::GetMainViewport();
    const auto& c = v2::Colors();
    camera::Control& phone = camera::Phone();
    const int total = (std::max)(phone.Total(), 1), frame = (std::max)(phone.Frame(), 0);
    const float height = (s_barMin ? 52 : 108) * scale;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - height));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, v2::color::Alpha(c.panel, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, c.border);
    ImGui::PushStyleColor(ImGuiCol_Button, c.button); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c.hover);
    ImGui::PushStyleColor(ImGuiCol_Text, c.text);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0); ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 10 * scale)); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 6 * scale));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("##cameratimeline", nullptr, flags)) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetWindowPos();
        draw->AddRectFilled(at, ImVec2(at.x + ImGui::GetWindowWidth(), at.y + 3 * scale), c.accent);
        const camera::Seek seek = phone.GetSeek();
        int prevKey = -1, nextKey = -1;
        for (const auto& k : s.keys) { if (k.frame < frame - 1) prevKey = k.frame; if (k.frame > frame + 1 && nextKey < 0) nextKey = k.frame; }
        if (ImGui::Button("|<")) Jump(bar, 0);
        ImGui::SameLine(); if (ImGui::Button("< key") && prevKey >= 0) Jump(bar, prevKey);
        ImGui::SameLine(); if (ImGui::Button(bar.paused ? "Play  F1" : "Pause  F1") && bar.send) bar.send(replaytransport::Command::TogglePause);
        ImGui::SameLine(); if (ImGui::Button("key >") && nextKey >= 0) Jump(bar, nextKey);
        ImGui::SameLine(); if (ImGui::Button("Step  F2") && bar.send) bar.send(replaytransport::Command::Step);
        ImGui::SameLine(); if (ImGui::Button("Back 1  ,")) StepBack(bar, 1);
        ImGui::SameLine();
        if (seek == camera::Seek::None) ImGui::Text("  f %d / %d  %.1f s", frame, total, frame / 60.f);
        else {
            int round = 0, goal = 0; phone.Goal(round, goal);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, c.accent);
            ImGui::Text("   Playing to frame %d", goal);
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 420 * scale);
        if (ImGui::Button(s.loop ? "Loop: on" : "Loop: off", ImVec2(84 * scale, 0))) Order({{"loop", !s.loop}});
        ImGui::SameLine();
        if (ImGui::Button(s.dolly ? "Follow: on" : "Follow: off", ImVec2(96 * scale, 0))) Order({{"dolly", !s.dolly}});
        ImGui::SameLine();
        if (ImGui::Button("Add key  (K)", ImVec2(120 * scale, 0))) Order({{"addKey", {{"at", camera::OffsetsJson(s.live)}, {"ease", 1}}}});
        ImGui::SameLine();
        if (ImGui::Button(s_barMin ? "^##bmin" : "v##bmin", ImVec2(28 * scale, 0))) s_barMin = !s_barMin;
        if (s_barMin) { ImGui::End(); ImGui::PopStyleVar(5); ImGui::PopStyleColor(5); return; }

        // The bar: a click or drag on empty bar jumps there (once, on release); a diamond is picked and dragged to move it.
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 44 * scale;
        ImGui::InvisibleButton("##bar", ImVec2(w, h));
        const auto frameAt = [&](float x) { return std::clamp(static_cast<int>(std::lround((x - p0.x) / w * (total - 1))), 0, total - 1); };
        const auto xOf = [&](int f) { return p0.x + static_cast<float>(f) / static_cast<float>((std::max)(total - 1, 1)) * w; };
        static int dragKey = -1, dragFrame = 0, scrubFrame = 0; static bool scrub = false;
        const float mouseX = ImGui::GetIO().MousePos.x;
        if (ImGui::IsItemActivated()) {
            int hit = -1; float best = 10 * scale;
            for (std::size_t i = 0; i < s.keys.size(); ++i) { const float d = std::fabs(mouseX - xOf(s.keys[i].frame)); if (d <= best) { best = d; hit = static_cast<int>(i); } }
            if (hit >= 0) {
                dragKey = hit; dragFrame = s.keys[hit].frame;
                // Picking a key puts its camera on the sliders to be tuned, and shows the sliders' camera.
                Order({{"selectKey", hit}, {"live", camera::OffsetsJson(s.keys[hit].at)}, {"dolly", false}});
            } else { scrub = true; scrubFrame = frameAt(mouseX); Pause(bar); }
        }
        if (ImGui::IsItemActive()) { if (dragKey >= 0) dragFrame = frameAt(mouseX); else if (scrub) scrubFrame = frameAt(mouseX); }
        if (ImGui::IsItemDeactivated()) {
            if (dragKey >= 0 && dragKey < static_cast<int>(s.keys.size()) && dragFrame != s.keys[dragKey].frame) Order({{"moveKey", {{"key", dragKey}, {"frame", dragFrame}}}});
            else if (scrub) Jump(bar, scrubFrame);
            dragKey = -1; scrub = false;
        }
        // The wheel over the bar scrubs a frame at a time (Shift: ten), up going back.
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) StepBack(bar, static_cast<int>(std::lround(ImGui::GetIO().MouseWheel)) * (ImGui::GetIO().KeyShift ? 10 : 1));
        draw->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), c.button);
        // Before the floor the replay was not played under these controls: no jump goes back there.
        if (phone.Floor() > 0) draw->AddRectFilled(p0, ImVec2(xOf(phone.Floor()), p0.y + h), v2::color::Alpha(c.border, 0.6f));
        draw->AddRect(p0, ImVec2(p0.x + w, p0.y + h), c.border);
        for (int f = 0; f < total; f += 60) {
            const bool five = f % 300 == 0;
            draw->AddLine(ImVec2(xOf(f), p0.y + h), ImVec2(xOf(f), p0.y + h - (five ? 14 : 7) * scale), c.border);
            if (five) { char t[16]; std::snprintf(t, sizeof t, "%ds", f / 60); draw->AddText(ImVec2(xOf(f) + 3, p0.y + h - 16 * scale), c.muted, t); }
        }
        const float mid = p0.y + 20 * scale;
        draw->AddLine(ImVec2(p0.x, mid), ImVec2(p0.x + w, mid), c.border, 2);
        for (std::size_t i = 0; i < s.keys.size(); ++i) {
            const int f = static_cast<int>(i) == dragKey ? dragFrame : s.keys[i].frame;
            const float x = xOf(f), r = 8 * scale;
            const ImU32 fill = static_cast<int>(i) == s.selected ? c.accent : c.button;
            draw->AddQuadFilled(ImVec2(x, mid - r), ImVec2(x + r, mid), ImVec2(x, mid + r), ImVec2(x - r, mid), fill);
            draw->AddQuad(ImVec2(x, mid - r), ImVec2(x + r, mid), ImVec2(x, mid + r), ImVec2(x - r, mid), c.accent, 2);
            char n[8]; std::snprintf(n, sizeof n, "%d", static_cast<int>(i) + 1);
            draw->AddText(ImVec2(x - 3 * scale, mid + r + 1), c.muted, n);
        }
        if (scrub) draw->AddLine(ImVec2(xOf(scrubFrame), p0.y), ImVec2(xOf(scrubFrame), p0.y + h), c.muted, 2);
        int goalRound = 0, goalFrame = 0;
        if (phone.Goal(goalRound, goalFrame)) draw->AddLine(ImVec2(xOf(goalFrame), p0.y), ImVec2(xOf(goalFrame), p0.y + h), c.accent, 2);
        draw->AddLine(ImVec2(xOf(frame), p0.y - 4 * scale), ImVec2(xOf(frame), p0.y + h + 4 * scale), c.text, 2);
    }
    ImGui::End();
    ImGui::PopStyleVar(5); ImGui::PopStyleColor(5);
}
void DrawWindow(const camera::Settings& s, const ReplayBar& bar, float scale) {
    const auto* vp = ImGui::GetMainViewport();
    const auto& c = v2::Colors();
    const float width = PanelWidth * scale;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - width - 24 * scale, vp->Pos.y + 80 * scale));
    // Under the timeline, never over it: a taller panel scrolls.
    const float room = vp->Size.y - 80 * scale - (bar.on && s_editor ? (s_barMin ? 52 : 116) * scale : 16 * scale) - 8 * scale;
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, (std::max)(room, 120.f * scale)));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, v2::color::Alpha(c.panel, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, c.border);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, c.text); ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, c.accentLight);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, c.button); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, c.hover);
    ImGui::PushStyleColor(ImGuiCol_Button, c.button); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c.hover);
    ImGui::PushStyleColor(ImGuiCol_Text, c.text);
    ImGui::PushStyleColor(ImGuiCol_Header, c.button); ImGui::PushStyleColor(ImGuiCol_HeaderHovered, c.hover); ImGui::PushStyleColor(ImGuiCol_HeaderActive, c.hover);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * scale); ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale, 12 * scale)); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 6 * scale));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##camerapanel", nullptr, flags)) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetWindowPos();
        draw->AddRectFilled(at, ImVec2(at.x + ImGui::GetWindowWidth(), at.y + 3 * scale), c.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, c.accent); ImGui::TextUnformatted("CAMERA"); ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, c.muted);
        ImGui::TextUnformatted(s.gliding ? "GLIDE" : s.free ? "FREE" : "LIVE");
        ImGui::PopStyleColor();
        // See-through: the whole panel and the timeline fade, so the picture shows behind them.
        ImGui::SameLine(ImGui::GetWindowWidth() - 150 * scale);
        ImGui::SetNextItemWidth(84 * scale);
        ImGui::SliderFloat("##opacity", &s_opacity, 0.25f, 1.f, "%.0f%%", ImGuiSliderFlags_NoRoundToFormat);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("See-through: how solid the panel and the timeline are");
        ImGui::SameLine(ImGui::GetWindowWidth() - 36 * scale);
        if (ImGui::SmallButton(s_panelMin ? "+##pmin" : "-##pmin")) s_panelMin = !s_panelMin;
        if (!s_panelMin) {
        ImGui::Spacing();
        if (bar.on) {
            // Over a replay: the video editor (timeline, keys, the replay controls inside) or the plain replay controls.
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (s_editor) ImGui::PushStyleColor(ImGuiCol_Button, c.accent);
            if (ImGui::Button("Video editor", ImVec2(half, 0))) s_editor = true;
            if (s_editor) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (!s_editor) ImGui::PushStyleColor(ImGuiCol_Button, c.accent);
            if (ImGui::Button("Replay controls", ImVec2(-1, 0))) s_editor = false;
            if (!s_editor) ImGui::PopStyleColor();
            if (s_editor && ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
                // The replay controls, inside the editor: what the strip has, with the same keys.
                const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
                const char* speed = bar.divisor == 4 ? "Speed 1/4  F3 F4" : bar.divisor == 2 ? "Speed 1/2  F3 F4" : "Speed 1x  F3 F4";
                if (ImGui::Button(speed, ImVec2(third, 0)) && bar.send) bar.send(replaytransport::Command::CycleSpeed);
                ImGui::SameLine();
                ImGui::BeginDisabled(!bar.lanesKnown);
                if (ImGui::Button(bar.lanes ? "Inputs on  F9" : "Inputs off  F9", ImVec2(third, 0)) && bar.send) bar.send(replaytransport::Command::Inputs);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(bar.meter ? "Meter on  F5" : "Meter off  F5", ImVec2(-1, 0)) && bar.send) bar.send(replaytransport::Command::Meter);
            }
        }
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        camera::Offsets o = s.live; bool moved = false;
        const auto before = o;
        Row("Orbit", "##yaw", o.yaw, o.yaw * 57.2958f, -180.f, 180.f, "%.0f deg", 0.0174533f);
        Row("Tilt", "##pitch", o.pitch, o.pitch * 57.2958f, -80.f, 80.f, "%.0f deg", 0.0174533f);
        Row("Zoom", "##zoom", o.zoom, o.zoom, camera::MinZoom, camera::MaxZoom, "%.2fx", 1.f);
        Row("Pan side", "##panx", o.panX, o.panX, -camera::MaxPan, camera::MaxPan, "%.1f", 1.f);
        Row("Pan up", "##pany", o.panY, o.panY, -camera::MaxPan, camera::MaxPan, "%.1f", 1.f);
        Row("Roll", "##roll", o.roll, o.roll * 57.2958f, -180.f, 180.f, "%.0f deg", 0.0174533f);
        Row("Lens", "##fov", o.fov, o.fov * 57.2958f, -30.f, 45.f, "%.0f deg", 0.0174533f);
        moved = o.yaw != before.yaw || o.pitch != before.pitch || o.zoom != before.zoom || o.panX != before.panX || o.panY != before.panY || o.roll != before.roll || o.fov != before.fov;
        if (moved) Order({{"live", camera::OffsetsJson(o)}});
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::Spacing();
        if (ImGui::Button("Reset camera  (Home)", ImVec2(half, 0))) Order({{"reset", true}});
        ImGui::SameLine();
        if (ImGui::Button(s.free ? "Free camera: on  (Ins)" : "Free camera: off  (Ins)", ImVec2(-1, 0))) Order({{"free", !s.free}});
        if (ImGui::Button(s_mouse ? "Mouse jog: on  (M)" : "Mouse jog: off  (M)", ImVec2(-1, 0))) s_mouse = !s_mouse;
        }
        if (bar.on && s_editor && ImGui::CollapsingHeader("Shot keys", ImGuiTreeNodeFlags_DefaultOpen)) KeyRows(s, bar, scale);
        if (ImGui::CollapsingHeader("Saved views  (1-4 go, Ctrl+1-4 save)")) {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        for (int i = 0; i < camera::ViewSlots; ++i) {
            const auto& v = s.views[i];
            ImGui::PushID(i);
            ImGui::Text("%d", i + 1); ImGui::SameLine(22 * scale);
            ImGui::PushStyleColor(ImGuiCol_Text, v.used ? c.text : c.muted);
            ImGui::TextUnformatted(v.used ? v.name.c_str() : "empty"); ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetWindowWidth() - 120 * scale);
            ImGui::BeginDisabled(!v.used);
            if (ImGui::Button("Go", ImVec2(48 * scale, 0))) Order({{"goView", i}});
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Save", ImVec2(56 * scale, 0))) Order({{"saveView", i}});
            ImGui::PopID();
        }
        float glide = s.glideMs / 1000.f;
        Row("Glide", "##glide", glide, glide, 0.3f, 10.f, "%.1f s", 1.f);
        if (std::abs(glide - s.glideMs / 1000.f) > 1e-4f) Order({{"glideMs", static_cast<int>(std::lround(glide * 1000))}});
        int used = 0; for (const auto& v : s.views) used += v.used;
        ImGui::BeginDisabled(used < 2);
        if (ImGui::Button("Tour  (T)", ImVec2(half, 0))) Order({{"tour", true}});
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.gliding);
        if (ImGui::Button("Stop move  (Bksp)", ImVec2(-1, 0))) Order({{"stopGlide", true}});
        ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader("Effects  (5-0)")) {
        const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
        int n = 0;
        for (const auto& e : Effects) {
            if (n++ % 3) ImGui::SameLine();
            if (ImGui::Button(e.label, ImVec2(third, 0))) Order({{"effect", e.id}});
        }
        }
        if (ImGui::CollapsingHeader("Help")) {
        ImGui::PushStyleColor(ImGuiCol_Text, c.muted);
        ImGui::TextWrapped("F12 closes this panel. The slider at the top makes the panel and the timeline see-through.\n\nTimeline: scrubbing pauses the replay. Drag the bar to jump (back to where you have played from the fight's start). Click a diamond to pick it, drag to move it. Wheel over the bar scrubs (Shift: 10), comma steps back, K adds a key, [ and ] jump between keys.\n\nMouse (jog on, or hold Ctrl) on the picture: drag orbits, middle/right/Shift drag pans, wheel zooms. Right stick (or keypad 4/6/8/2) orbit and tilt, Shift pans, +/- or PgUp/PgDn zoom, 7/9 roll, Alt slower. Press the right stick: free camera.");
        ImGui::PopStyleColor();
        }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(5); ImGui::PopStyleColor(12);
}
}  // namespace

bool CameraPanelOpen() { return s_open; }
bool CameraEditorShown() { return s_reserve > 0; }

void DrawCameraPanel(bool available, bool focused, const std::filesystem::path& views, const ReplayBar& replay) {
    if (!s_fileSet) { camera::Phone().SetFile(views); s_fileSet = true; }
    if (!available || !focused) { if (!available) s_open = false; s_reserve = 0; return; }
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F12, false)) s_open = !s_open;
    if (!s_open) { s_reserve = 0; return; }
    const auto settings = camera::Phone().Get();
    Keys(settings, replay);
    const float scale = (std::max)(1.f, (std::min)(1.5f, ImGui::GetMainViewport()->Size.y / 900.f));
    DrawWindow(settings, replay, scale);
    s_reserve = replay.on && s_editor ? (s_barMin ? 52 : 108) * scale : 0;
    if (replay.on && s_editor) DrawTimeline(settings, replay, scale);
}
void DrawCameraPanel(bool available, bool focused, const std::filesystem::path& directory, bool replayOn, const ReplaysView& replays,
                     std::function<void(replaytransport::Command)> send) {
    ReplayBar bar;
    bar.on = replayOn && replays.playback.armed;
    bar.paused = replays.playback.paused; bar.divisor = replays.playback.divisor; bar.lanes = replays.playback.lanes;
    bar.meter = replays.meterShown; bar.lanesKnown = replays.lanes != nullptr; bar.send = std::move(send);
    DrawCameraPanel(available, focused, directory / "camera-views.json", bar);
}
} }
