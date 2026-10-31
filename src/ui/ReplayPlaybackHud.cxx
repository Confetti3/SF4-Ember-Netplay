#include "ReplayPlaybackHud.hxx"
#include "GameMenu.hxx"
#include "MenuGlyphs.hxx"
#include "Theme.hxx"
#include "TrainingPanel.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayTransport.hxx"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

// The replay controls' strip and input lanes (ReplayPlaybackHud.hxx).
namespace sf4e { namespace ui {
namespace {
constexpr ImGuiWindowFlags Passive = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
// Sized to the game viewport like the training HUD, not to the menu's scale.
float HudScale() { return (std::max)(1.f, (std::min)(1.5f, ImGui::GetMainViewport()->Size.y / 900.f)); }
// The strip's height, and how far its bottom edge sits above the screen's
// (the Playback board), in pixels at HudScale 1.
constexpr float StripHeight = 68, StripLift = 64;
ImU32 Faded(ImU32 color, float alpha) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}
float TextWidth(float size, const std::string& text) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }
// An arrow for the held direction, screen-relative as the replay records it;
// a dot when no direction is held.
void DrawDirection(ImDrawList* d, ImVec2 c, float r, unsigned held, ImU32 color, ImU32 idle) {
    float dx = static_cast<float>(((held & replayinputs::Right) ? 1 : 0) - ((held & replayinputs::Left) ? 1 : 0));
    float dy = static_cast<float>(((held & replayinputs::Down) ? 1 : 0) - ((held & replayinputs::Up) ? 1 : 0));
    if (!dx && !dy) { d->AddCircleFilled(c, r * .22f, idle); return; }
    const float length = std::sqrt(dx * dx + dy * dy); dx /= length; dy /= length;
    const ImVec2 tip(c.x + dx * r * .85f, c.y + dy * r * .85f), tail(c.x - dx * r * .7f, c.y - dy * r * .7f);
    const ImVec2 base(tip.x - dx * r * .6f, tip.y - dy * r * .6f);
    d->AddLine(tail, base, color, (std::max)(1.5f, r * .24f));
    d->AddTriangleFilled(tip, ImVec2(base.x - dy * r * .5f, base.y + dx * r * .5f), ImVec2(base.x + dy * r * .5f, base.y - dx * r * .5f), color);
}
std::string SpeedWord(int divisor) {
    return loc::T(divisor == 4 ? "replays.controls.speed_quarter" : divisor == 2 ? "replays.controls.speed_half" : "replays.controls.speed_full");
}
// The strip (the Playback board): a button per control, its pad and
// keyboard keys over what it does, lit while that is on, then the place in
// the round; centred, its top at `top`. The keys are the legend, so it has no hint bar.
void DrawStrip(const ReplayHudView& view, float alpha, const std::string& place, float top) {
    const auto* vp = ImGui::GetMainViewport();
    const auto& c = v2::Colors();
    const auto padKey = [](const char* id) {
        const auto glyph = BrandGlyph(id, MenuPromptBrand(), MenuPromptNames());
        return std::string(*glyph.name ? glyph.name : id) + " \xC2\xB7 ";
    };
    const auto onOff = [](bool on) { return std::string(" ") + loc::T(on ? "common.on" : "common.off"); };
    struct Control { std::string keys, label; bool lit; ImU32 ink; };
    std::vector<Control> controls{
        {padKey("RB") + "F1", loc::T(view.paused ? "replays.controls.play" : "replays.controls.pause"), !view.paused, c.text},
        {padKey("RT") + "F2", loc::T("replays.controls.step"), false, c.text},
        // The chosen speed, dimmed where the game's own pace runs (the knockout, the round's end).
        {padKey("LB") + "F3 F4", loc::T("replays.controls.speed") + std::string(" ") + SpeedWord(view.divisor), view.divisor != 1, view.armed ? c.text : c.muted}};
    if (view.inputsKnown) controls.push_back({padKey("LT") + "F9", loc::T("replays.controls.inputs") + onOff(view.lanes), view.lanes, c.text});
    // The pad has no meter button.
    controls.push_back({"F5", loc::T("replays.controls.meter") + onOff(view.meter), view.meter, c.text});
    // The board's sizes, shrunk to fit a narrow window.
    float s = HudScale();
    const auto measure = [&](float k) {
        float w = 16 * k + 20 * k + TextWidth(14 * k, place);
        for (const auto& control : controls) w += (std::max)(92 * k, (std::max)(TextWidth(11 * k, control.keys), TextWidth(16 * k, control.label)) + 20 * k) + 6 * k;
        return w;
    };
    const float room = vp->Size.x - 32 * s;
    if (measure(s) > room) s *= room / measure(s);
    const float width = measure(s), height = StripHeight * s;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, top), ImGuiCond_Always, ImVec2(.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
    if (ImGui::Begin("###Replay strip", nullptr, Passive)) {
        auto* d = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        d->AddRectFilled(p, ImVec2(p.x + width, p.y + height), Faded(v2::color::Alpha(c.ink, .92f), alpha), 4 * s);
        d->AddRect(p, ImVec2(p.x + width, p.y + height), Faded(v2::color::Alpha(c.accent, .45f), alpha), 4 * s, 0, s);
        float x = p.x + 8 * s;
        const float top = p.y + 8 * s, bottom = p.y + height - 8 * s;
        for (const auto& control : controls) {
            const float w = (std::max)(92 * s, (std::max)(TextWidth(11 * s, control.keys), TextWidth(16 * s, control.label)) + 20 * s);
            d->AddRectFilled(ImVec2(x, top), ImVec2(x + w, bottom), Faded(control.lit ? c.header : c.button, alpha), 4 * s);
            d->AddRect(ImVec2(x, top), ImVec2(x + w, bottom), Faded(control.lit ? c.accent : c.border, alpha), 4 * s, 0, (control.lit ? 2 : 1) * s);
            const float mid = x + w * .5f, cy = (top + bottom) * .5f;
            d->AddText(ImGui::GetFont(), 11 * s, ImVec2(mid - TextWidth(11 * s, control.keys) * .5f, cy - 13 * s), Faded(c.muted, alpha), control.keys.c_str());
            d->AddText(ImGui::GetFont(), 16 * s, ImVec2(mid - TextWidth(16 * s, control.label) * .5f, cy - 1 * s), Faded(control.ink, alpha), control.label.c_str());
            x += w + 6 * s;
        }
        d->AddText(ImGui::GetFont(), 14 * s, ImVec2(x + 10 * s, p.y + (height - 14 * s) * .5f), Faded(c.muted, alpha), place.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}
}

void DrawReplayStrip(const ReplayHudView& view) {
    const auto* vp = ImGui::GetMainViewport();
    const float s = HudScale(), height = StripHeight * s, bottom = vp->Pos.y + vp->Size.y;
    // At the bottom centre, as the board has it, unless the frame meter
    // reaches lower (a 720p window, or a meter the player moved down): then
    // just under the meter, but never past the screen's edge.
    const float meterBottom = vp->Pos.y + vp->Size.y * TrainingMeterBottom();
    const float stripTop = (std::min)((std::max)(bottom - (StripLift + StripHeight) * s, meterBottom + 4 * s), bottom - height);
    if (view.unavailable) {
        // Under the strip's place (the strip may be faded out), so it meets
        // neither the strip nor the frame meter above it.
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, stripTop + height + 4 * s), ImGuiCond_Always, ImVec2(.5f, 0));
        ImGui::SetNextWindowBgAlpha(.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 4 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * s);
        if (ImGui::Begin("###Replay notice", nullptr, Passive | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetWindowFontScale(.8f * s / Scale());
            ImGui::PushStyleColor(ImGuiCol_Text, palette::Ivory);
            ImGui::TextUnformatted(loc::T("replays.controls.unavailable"));
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.f);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    if (view.stripAlpha <= 0) return;
    const float alpha = (std::min)(1.f, view.stripAlpha);
    char clock[16]; replaytransport::FormatClock(view.cursor, clock);
    // Paused, the frame takes the clock's place: a step moves it by one.
    const std::string place = loc::Tf("replays.controls.round", view.round + 1) + " \xC2\xB7 " +
        (view.paused ? loc::Tf("replays.controls.frame", view.cursor) : std::string(clock));
    DrawStrip(view, alpha, place, stripTop);
}

void DrawReplayLanes(const ReplayHudView& view) {
    const auto* vp = ImGui::GetMainViewport();
    // The game's 16:9 picture in its 720p units, as the match HUD measures it.
    const float gs = (std::min)(vp->Size.y / 720.f, vp->Size.x / 1280.f);
    const float gx0 = vp->Pos.x + (vp->Size.x - 1280 * gs) * .5f, gy0 = vp->Pos.y + (vp->Size.y - 720 * gs) * .5f;
    // Sized with the picture, but kept readable in a small window.
    const float ls = (std::max)(gs, .75f);
    // The Playback board's lanes: a card per run, the direction, the buttons
    // in the accent (brighter on the frame they went down) and the run's frames.
    const auto& c = v2::Colors();
    const float row = 30 * ls, gap = 4 * ls, font = 14 * ls, small = 12 * ls, width = 136 * ls, top = gy0 + 180 * gs;
    for (int side = 0; side < 2; side++) {
        const int rows = (std::min)(view.rowCount[side], 8);
        if (rows <= 0) continue;
        const float x0 = side ? gx0 + 1280 * gs - 24 * gs - width : gx0 + 24 * gs;
        ImGui::SetNextWindowPos(ImVec2(x0, top), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, rows * (row + gap)), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
        if (ImGui::Begin(side ? "###Replay inputs 2" : "###Replay inputs 1", nullptr, Passive)) {
            auto* d = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetWindowPos();
            for (int i = 0; i < rows; i++) {
                const auto& r = view.rows[side][i];
                const float y = p.y + i * (row + gap), cy = y + row * .5f;
                d->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width, y + row), v2::color::Alpha(c.ink, .85f), 3 * ls);
                DrawDirection(d, ImVec2(p.x + 17 * ls, cy), 8 * ls, r.held, c.text, c.muted);
                float x = p.x + 32 * ls;
                for (int b = 0; b < 6; b++) {
                    if (!(r.held & replayinputs::Buttons[b])) continue;
                    d->AddText(ImGui::GetFont(), font, ImVec2(x, cy - font * .5f), (r.pressed & replayinputs::Buttons[b]) ? c.accent : c.accentLight, replayinputs::ButtonNames[b]);
                    x += TextWidth(font, replayinputs::ButtonNames[b]) + 4 * ls;
                }
                const std::string frames = r.frames > 999 ? std::string("999+") : std::to_string(r.frames);
                d->AddText(ImGui::GetFont(), small, ImVec2(p.x + width - 8 * ls - TextWidth(small, frames), cy - small * .5f), c.muted, frames.c_str());
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }
}
} }
