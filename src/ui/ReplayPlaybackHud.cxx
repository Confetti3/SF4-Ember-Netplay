#include "ReplayPlaybackHud.hxx"
#include "GameMenu.hxx"
#include "MenuGlyphs.hxx"
#include "Theme.hxx"
#include "TrainingPanel.hxx"
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
ImU32 Faded(ImU32 color, float alpha) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}
float TextWidth(float size, const std::string& text) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }
// One legend entry: one or two glyphs (F3 and F4 share "Speed") and its label.
struct Hint { const char* glyphs[2]; std::string label; };
float HintWidth(const Hint& hint, float glyph, float font, float s) {
    return glyph * (hint.glyphs[1] ? 2 : 1) + 4 * s + TextWidth(font, hint.label) + 10 * s;
}
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
}

void DrawReplayStrip(const ReplayHudView& view) {
    const auto* vp = ImGui::GetMainViewport();
    const float s = HudScale(), height = ReplayStripMostHeight * s, most = ReplayStripMostWidth * s;
    const float stripTop = vp->Pos.y + vp->Size.y * TrainingHudBottom + 4 * s;
    if (view.unavailable) {
        // Under the strip's place (the strip may be faded out), between the
        // super meters: the frame meter sits above the strip, so the two never meet.
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

    // What it says, measured before anything is drawn.
    float statusFont = 14 * s, legendFont = 12 * s;
    const float glyph = 18 * s, gap = 6 * s, padX = 8 * s, icon = 11 * s;
    const std::string pausedWord = view.paused ? loc::T("replays.controls.paused") : "";
    const std::string speed = loc::T(view.divisor == 4 ? "replays.controls.speed_quarter" : view.divisor == 2 ? "replays.controls.speed_half" : "replays.controls.speed_full");
    char clock[16]; replaytransport::FormatClock(view.cursor, clock);
    // Paused, the frame takes the clock's place: a step moves it by one.
    const std::string place = loc::Tf("replays.controls.round", view.round + 1) + " \xC2\xB7 " +
        (view.paused ? loc::Tf("replays.controls.frame", view.cursor) : std::string(clock));
    std::vector<Hint> hints;
    hints.push_back({{view.pad ? "RB" : "F1", nullptr}, loc::T(view.paused ? "replays.controls.play" : "replays.controls.pause")});
    hints.push_back({{view.pad ? "RT" : "F2", nullptr}, loc::T("replays.controls.step")});
    if (view.pad) hints.push_back({{"LB", nullptr}, loc::T("replays.controls.speed")});
    else hints.push_back({{"F3", "F4"}, loc::T("replays.controls.speed")});
    // A fourth entry: the lanes when the replay's inputs are known; else, on
    // the keyboard, F5's meter (the same key as in Training). The pad has no
    // meter button.
    if (view.inputsKnown) hints.push_back({{view.pad ? "LT" : "F9", nullptr}, loc::T("replays.controls.inputs")});
    else if (!view.pad) hints.push_back({{"F5", nullptr}, loc::T("replays.controls.meter")});
    const auto statusWidth = [&] {
        return icon + gap + (pausedWord.empty() ? 0 : TextWidth(statusFont, pausedWord) + gap) + TextWidth(statusFont, speed) + gap + TextWidth(statusFont, place);
    };
    const auto legendWidth = [&] { float w = 0; for (const auto& hint : hints) w += HintWidth(hint, glyph, legendFont, s); return w; };
    const auto total = [&] { return 2 * padX + statusWidth() + 2 * gap + 1 + legendWidth(); };
    // Too wide (a long translation): the type shrinks a little, then the fourth entry goes.
    while (total() > most && legendFont > 11 * s) legendFont -= .5f * s;
    while (total() > most && statusFont > 12.5f * s) statusFont -= .5f * s;
    while (total() > most && hints.size() > 3) hints.pop_back();
    const float width = (std::min)(total(), most);

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, stripTop), ImGuiCond_Always, ImVec2(.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.9f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    // The theme's smallest window is taller than the strip.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    if (ImGui::Begin("###Replay strip", nullptr, Passive)) {
        auto* d = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        const float cy = p.y + height * .5f;
        ImGui::PushClipRect(p, ImVec2(p.x + width, p.y + height), true);
        float x = p.x + padX;
        const auto text = [&](const std::string& value, float size, ImU32 color) {
            d->AddText(ImGui::GetFont(), size, ImVec2(x, cy - size * .5f), Faded(color, alpha), value.c_str());
            x += TextWidth(size, value) + gap;
        };
        // Paused: two bars in the accent; playing: a play triangle.
        if (view.paused) {
            const float bar = icon * .3f;
            d->AddRectFilled(ImVec2(x + icon * .1f, cy - icon * .5f), ImVec2(x + icon * .1f + bar, cy + icon * .5f), Faded(palette::Ember, alpha));
            d->AddRectFilled(ImVec2(x + icon * .9f - bar, cy - icon * .5f), ImVec2(x + icon * .9f, cy + icon * .5f), Faded(palette::Ember, alpha));
        }
        else d->AddTriangleFilled(ImVec2(x + icon * .15f, cy - icon * .5f), ImVec2(x + icon * .15f, cy + icon * .5f), ImVec2(x + icon, cy), Faded(palette::Ivory, alpha));
        x += icon + gap;
        if (!pausedWord.empty()) text(pausedWord, statusFont, palette::Ember);
        // The chosen speed, dimmed where the game's own pace runs (the knockout, the round's end).
        text(speed, statusFont, !view.armed ? palette::Muted : view.divisor != 1 ? palette::Ember : palette::Ivory);
        text(place, statusFont, palette::Ivory);
        d->AddLine(ImVec2(x, p.y + 7 * s), ImVec2(x, p.y + height - 7 * s), ImGui::GetColorU32(ImGuiCol_Border));
        x += gap + 1;
        SelectionArt* const art = MenuArt();
        for (const auto& hint : hints) {
            for (const char* g : hint.glyphs) if (g) { DrawPromptGlyph(d, g, ImVec2(x, cy - glyph * .5f), glyph, art, glyph / 32, 10 * s); x += glyph; }
            x += 4 * s;
            d->AddText(ImGui::GetFont(), legendFont, ImVec2(x, cy - legendFont * .5f), Faded(palette::Muted, alpha), hint.label.c_str());
            x += TextWidth(legendFont, hint.label) + 10 * s;
        }
        ImGui::PopClipRect();
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
}

void DrawReplayLanes(const ReplayHudView& view) {
    const auto* vp = ImGui::GetMainViewport();
    // The game's 16:9 picture in its 720p units, as the match HUD measures it.
    const float gs = (std::min)(vp->Size.y / 720.f, vp->Size.x / 1280.f);
    const float gx0 = vp->Pos.x + (vp->Size.x - 1280 * gs) * .5f, gy0 = vp->Pos.y + (vp->Size.y - 720 * gs) * .5f;
    // Sized with the picture, but kept readable in a small window.
    const float ls = (std::max)(gs, .75f);
    const float row = 17 * ls, font = 12 * ls, arrow = 14 * ls, chip = 19 * ls, count = 30 * ls, pad = 6 * ls;
    const float width = 2 * pad + arrow + 6 * ls + 6 * chip + count;
    // Below the life bars (to about y 130) and the name logos beside them (y 144 to 166).
    const float top = gy0 + 180 * gs;
    for (int side = 0; side < 2; side++) {
        const int rows = (std::min)(view.rowCount[side], replaylane::kRows);
        if (rows <= 0) continue;
        const float x0 = side ? gx0 + 1280 * gs - 8 * gs - width : gx0 + 8 * gs;
        ImGui::SetNextWindowPos(ImVec2(x0, top), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, rows * row + 2 * pad), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.82f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * ls);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
        if (ImGui::Begin(side ? "###Replay inputs 2" : "###Replay inputs 1", nullptr, Passive)) {
            auto* d = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetWindowPos();
            for (int i = 0; i < rows; i++) {
                const auto& r = view.rows[side][i];
                const float y = p.y + pad + i * row, cy = y + row * .5f;
                float x = p.x + pad;
                DrawDirection(d, ImVec2(x + arrow * .5f, cy), arrow * .5f, r.held, palette::Ivory, palette::Muted);
                x += arrow + 6 * ls;
                // Each button keeps its column; Ember orange on the frame it went down, Ivory while held.
                for (int b = 0; b < 6; b++, x += chip) {
                    if (!(r.held & replayinputs::Buttons[b])) continue;
                    const ImU32 color = (r.pressed & replayinputs::Buttons[b]) ? palette::Ember : palette::Ivory;
                    d->AddText(ImGui::GetFont(), font, ImVec2(x, cy - font * .5f), color, replayinputs::ButtonNames[b]);
                }
                const std::string frames = r.frames > 999 ? std::string("999+") : std::to_string(r.frames);
                d->AddText(ImGui::GetFont(), font, ImVec2(p.x + width - pad - TextWidth(font, frames), cy - font * .5f), palette::Muted, frames.c_str());
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(4);
    }
}
} }
