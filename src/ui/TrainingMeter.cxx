#include "TrainingPanel.hxx"
#include "FrameMeterWindow.hxx"
#include "ReplayPlaybackHud.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "TextPill.hxx"
#include "InputGlyphs.hxx"
#include "../common/ButtonLayoutTypes.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

// The passive HUD around the frame meter (FrameMeterWindow.cxx draws the
// meter itself); the F6 controls are in TrainingPanel.cxx.
namespace sf4e { namespace ui {
using training::InputDisplay;
// The match is ready: across the middle of the screen while the battle is
// about to be taken away, while the call stands. Under it, smaller: who sat
// down, and go now when it can be pressed.
void ChallengerBanner(const training::View& view, const ChallengerCall& call) {
    if (view.leavingIn <= 0 || !call.called) return;
    const auto* vp = ImGui::GetMainViewport();
    auto* draw = ImGui::GetForegroundDrawList();
    auto* font = ImGui::GetFont();
    const char* text = loc::T("training.challenger");
    // A long translation in a narrow window shrinks to nine tenths of its width.
    const float tall = vp->Size.y * .075f, wide = font->CalcTextSizeA(tall, FLT_MAX, 0, text).x;
    const float size = wide > vp->Size.x * .9f ? tall * vp->Size.x * .9f / wide : tall;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    const float small = size * .4f, glyph = small * 1.4f, gap = small * 1.5f;
    const auto goLabel = call.goNowGlyph ? PromptLabel(call.goNowGlyph, loc::T("training.go_now"), MenuPromptBrand()) : std::string();
    const char* goNow = call.goNowGlyph ? goLabel.c_str() : nullptr;
    const float goWidth = goNow ? glyph + small * .35f + font->CalcTextSizeA(small, FLT_MAX, 0, goNow).x : 0;
    if (goNow) ReportMenuText("challenger-go-now", small, glyph, goWidth, vp->Size.x * .9f);
    std::string who = call.opponent;
    if (const auto* fighter = who.empty() ? nullptr : selection::FindFighter(call.fighter)) who += "  \xC2\xB7  " + std::string(fighter->name);
    // Only the name gives way, so go now is never pushed off the screen.
    const float whoRoom = vp->Size.x * .9f - goWidth - (goNow ? gap : 0);
    if (!who.empty() && font->CalcTextSizeA(small, FLT_MAX, 0, who.c_str()).x > whoRoom) {
        const char* end = nullptr;
        font->CalcTextSizeA(small, (std::max)(1.f, whoRoom - font->CalcTextSizeA(small, FLT_MAX, 0, "...").x), 0, who.c_str(), nullptr, &end);
        who = std::string(who.c_str(), end) + "...";
    }
    const float whoWidth = who.empty() ? 0 : font->CalcTextSizeA(small, FLT_MAX, 0, who.c_str()).x;
    const bool second = !who.empty() || goNow;
    const float y = vp->Pos.y + vp->Size.y * .42f, band = size * 1.8f, below = second ? glyph + size * .3f : 0;
    draw->AddRectFilled(ImVec2(vp->Pos.x, y - (band - extent.y) / 2), ImVec2(vp->Pos.x + vp->Size.x, y + (band + extent.y) / 2 + below), IM_COL32(0, 0, 0, 170));
    const ImVec2 at(vp->Pos.x + (vp->Size.x - extent.x) / 2, y);
    // Lit and dim by turns, as the game's banner flashes.
    const ImU32 colour = (view.leavingIn / 8) % 2 ? IM_COL32(255, 214, 72, 255) : palette::Ember;
    for (const ImVec2 offset : {ImVec2(-2, 0), ImVec2(2, 0), ImVec2(0, -2), ImVec2(0, 2)})
        draw->AddText(font, size, ImVec2(at.x + offset.x, at.y + offset.y), IM_COL32(10, 10, 10, 255), text);
    draw->AddText(font, size, at, colour, text);
    if (!second) return;
    const float lineY = y + extent.y + size * .25f;
    float x = vp->Pos.x + (vp->Size.x - whoWidth - goWidth - (whoWidth && goWidth ? gap : 0)) / 2;
    if (!who.empty()) {
        NoteUserText(call.opponent);
        draw->AddText(font, small, ImVec2(x, lineY + (glyph - small) / 2), palette::Ivory, who.c_str());
        x += whoWidth + gap;
    }
    if (goNow) {
        DrawPromptGlyph(draw, call.goNowGlyph, ImVec2(x, lineY), glyph, MenuArt(), glyph / 32);
        draw->AddText(font, small, ImVec2(x + glyph + small * .35f, lineY + (glyph - small) / 2), palette::Ivory, goNow);
    }
}
float TrainingHudScale() { return FrameMeterHudScale(TrainingMeterOptions().size); }
void DrawTrainingRoomStatus(const TrainingRoomStatus& status) {
    if (status.Empty()) return;
    const auto* vp = ImGui::GetMainViewport();
    const float hudScale = TrainingHudScale();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y * TrainingRoomStatusTop), ImGuiCond_Always, ImVec2(.5f, 0));
    ImGui::SetNextWindowBgAlpha(TrainingHudBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * hudScale, 3 * hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training room status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f * hudScale / Scale());
        // The players' names give way so the line stays within two fifths of
        // the screen, clear of the gauges at the sides. Ember's own words keep
        // their room; the two names share what is left: half each, and either
        // takes what the other does not need. Each keeps at least a letter and
        // "...", so where Ember's words alone fill that room (a narrow screen,
        // a long translation) the line is those words and the two shortest
        // names; the render test holds every translation within nine tenths of
        // the screen.
        const auto width = [](const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x; };
        const bool hasRoom = !status.room.empty(), hasOpponent = !status.opponent.empty();
        const std::string mark = "W";
        const float fixed = width(TrainingRoomLine(status, hasRoom ? mark : std::string(), mark)) - (hasRoom ? width(mark) : 0) - (hasOpponent ? width(mark) : 0);
        const float spare = (std::max)(0.f, vp->Size.x * .4f - fixed), least = width("W...");
        const float roomWidth = hasRoom ? width(status.room) : 0, opponentWidth = hasOpponent ? width(status.opponent) : 0;
        const float half = hasRoom && hasOpponent ? spare / 2 : spare;
        const float roomFits = (std::min)(roomWidth, (std::max)(half, spare - (std::min)(opponentWidth, half)));
        const float opponentFits = (std::min)(opponentWidth, spare - roomFits);
        const std::string line = TrainingRoomLine(status, hasRoom ? FitLabel(status.room, (std::max)(least, roomFits)) : std::string(),
            hasOpponent ? FitLabel(status.opponent, (std::max)(least, opponentFits)) : std::string());
        ReportMenuText("training-room-line", ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight(), width(line), vp->Size.x * .9f);
        NoteUserText(line);
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
// What the lab is doing on standby, or while it records or plays a slot: a
// hollow or red dot, or a green arrow, the slot, how far along it is and, for
// the keyboard, the key that moves on from it, over a line that fills as the
// slot does. Standby has recorded nothing, so its line stays empty, and it
// names both ways to start recording: the pad's Back and Start, and F7.
static void ModeStrip(const training::View& view, float hudScale) {
    const bool standby = view.mode == training::Mode::Standby, recording = view.mode == training::Mode::Recording;
    const bool padPlays = training::PadPlaysDummy(view.mode);
    const float h = ImGui::GetTextLineHeight(), full = ImGui::GetContentRegionAvail().x, left = ImGui::GetCursorPosX();
    const ImU32 colour = padPlays ? IM_COL32(255, 121, 129, 255) : palette::Ready;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (!padPlays) draw->AddTriangleFilled(ImVec2(at.x + h * .15f, at.y + h * .2f), ImVec2(at.x + h * .15f, at.y + h * .8f), ImVec2(at.x + h * .7f, at.y + h * .5f), colour);
    // On standby the dot waits, hollow; recording, it blinks once a second, as a recorder's does.
    else if (standby) draw->AddCircle(ImVec2(at.x + h * .4f, at.y + h * .5f), h * .3f, colour, 0, (std::max)(1.f, 1.5f * hudScale));
    else if (static_cast<int>(ImGui::GetTime() * 2) % 2 == 0) draw->AddCircleFilled(ImVec2(at.x + h * .4f, at.y + h * .5f), h * .3f, colour);
    ImGui::Dummy(ImVec2(h * .8f, h)); ImGui::SameLine(0, 4 * hudScale);
    // What plays is the active slot; standby and a recording are for the selected one.
    const int slot = padPlays ? view.selected : view.active;
    const int total = padPlays ? training::MaxFrames : view.activeLength;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour), "%s", loc::T(standby ? "training.standby" : recording ? "training.record" : "training.play"));
    ImGui::SameLine(0, 10 * hudScale);
    if (standby) ImGui::Text("%s", loc::Tf("training.slot", slot + 1).c_str());
    else ImGui::Text("%s  %d / %d f", loc::Tf("training.slot", slot + 1).c_str(), view.cursor, total);
    if (standby) {
        // [Back] + [Start] / F7 Record, the pad's buttons on pills as the shortcut strip draws them.
        const auto names = TrainingPadNames();
        const float unit = ImGui::GetFontSize() / 15.f;
        const std::string plus = " + ", tail = " / " + TrainingKeyName(TrainingRecordKey) + " " + loc::T("training.record");
        const auto pill = [&](const char* text) { return ImGui::CalcTextSize(text).x + 12 * unit; };
        const float width = pill(names.view) + ImGui::CalcTextSize(plus.c_str()).x + pill(names.start) + ImGui::CalcTextSize(tail.c_str()).x;
        ImGui::SameLine(left + full - width, 0);
        for (const char* button : {names.view, names.start}) {
            DrawButtonPill(draw, button, ImGui::GetCursorScreenPos(), pill(button), h, unit, .8f);
            ImGui::Dummy(ImVec2(pill(button), h)); ImGui::SameLine(0, 0);
            if (button == names.view) { ImGui::TextDisabled("%s", plus.c_str()); ImGui::SameLine(0, 0); }
        }
        ImGui::TextDisabled("%s", tail.c_str());
    } else if (MenuPromptDevice() == sf4e::input::PadKeyboard) {
        const std::string next = TrainingKeyName(padPlays ? TrainingRecordKey : TrainingPlayKey) + " " + loc::T("training.stop");
        ImGui::SameLine(left + full - ImGui::CalcTextSize(next.c_str()).x); ImGui::TextDisabled("%s", next.c_str());
    }
    const ImVec2 bar = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(bar, ImVec2(bar.x + full, bar.y + 2 * hudScale), IM_COL32(28, 29, 29, 100));
    draw->AddRectFilled(bar, ImVec2(bar.x + full * (total > 0 ? (std::min)(1.f, static_cast<float>(view.cursor) / total) : 0), bar.y + 2 * hudScale), colour);
    ImGui::Dummy(ImVec2(full, 4 * hudScale));
}
// The meter's own window, with the recording strip over the meter while a
// slot records or plays.
static ImVec2 MeterWindow(const training::View& view, float hudScale, float width, float hudBottom, const training::MeterOptions& options) {
    // The dummy's typed reply plays from a slot of its own and is no playback of the player's.
    const bool strip = options.status && (training::PadPlaysDummy(view.mode) ||
        (view.mode == training::Mode::Playback && view.active >= 0 && view.active < training::SlotCount));
    return DrawFrameMeterWindow(view.meter, hudScale, width, hudBottom, options,
        strip ? std::function<void()>([&] { ModeStrip(view, hudScale); }) : std::function<void()>());
}
// The frame meter alone, over a rollback match.
void DrawMatchMeter(const training::View& view) {
    if (!view.watching) return;
    const auto* vp = ImGui::GetMainViewport();
    const float hudScale = TrainingHudScale();
    // A match's inputs stay hidden, the opponent's above all, and the match
    // records none: the classic meter's input lanes show over a replay only,
    // whose file has every input.
    auto options = TrainingMeterOptions();
    options.classicInputs = options.classicInputs && view.replayInputs;
    MeterWindow(view, hudScale, TrainingMeterWidth(hudScale), vp->Pos.y + vp->Size.y * TrainingMeterBottom(), options);
}
void DrawMatchKeysNotice(std::initializer_list<const char*> keys) {
    const auto* vp=ImGui::GetMainViewport();
    const float hudScale=TrainingHudScale();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x/2,vp->Pos.y+vp->Size.y*TrainingMeterBottom()+4*hudScale),ImGuiCond_Always,ImVec2(.5f,0));
    ImGui::SetNextWindowBgAlpha(TrainingHudBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(4*hudScale,3*hudScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);
    if(ImGui::Begin(MatchKeysWindow,nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(.8f*hudScale/Scale());
        // Ember's own words, so each line must fit the screen in every language.
        for(const char* key:keys){
            const char* line=loc::T(key);
            ReportMenuText(key,ImGui::GetTextLineHeight(),ImGui::GetTextLineHeight(),ImGui::CalcTextSize(line).x,vp->Size.x*.9f);
            ImGui::TextDisabled("%s",line);
        }
        ImGui::SetWindowFontScale(1.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
// Each fighter's pad beside the meter, P1 left and P2 right of it: the stick
// in an octagonal gate with a fading line through the directions it took
// over the last InputTrail frames, eight arrows with the held one lit, or
// both, and the six buttons, lit while held. It reads the session's input history
// (View::history), which a match does not keep. Returns the area both cover.
constexpr int InputTrail = 16;
// A pad's direction on the screen, as the lanes read its bits: x to the right, y down.
static ImVec2 Way(unsigned held) {
    return ImVec2(static_cast<float>(((held & 8) ? 1 : 0) - ((held & 4) ? 1 : 0)), static_cast<float>(((held & 2) ? 1 : 0) - ((held & 1) ? 1 : 0)));
}
static TrainingHudArea DrawInputDisplay(const training::View& view, InputDisplay shown, float hudScale, float hudLeft, float width, float hudBottom) {
    // Both takes a larger square, so the gate in the arrows' middle stays readable.
    const float r = (shown == InputDisplay::Both ? 34 : 24) * hudScale, disc = 18 * hudScale, gap = 3 * hudScale, pad = 6 * hudScale;
    const float inner = 2 * r + 2 * gap + 3 * (disc + gap), outer = inner + 2 * pad;
    // On a narrow screen there is no room beside the meter; the screen's edge wins.
    const auto* vp = ImGui::GetMainViewport();
    TrainingHudArea area;
    for (int side = 0; side < 2; ++side) {
        const auto& history = view.history[side];
        const float x = side ? (std::min)(hudLeft + width + gap, vp->Pos.x + vp->Size.x - outer) : (std::max)(hudLeft - gap - outer, vp->Pos.x);
        ImGui::SetNextWindowPos(ImVec2(x, hudBottom), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(TrainingHudBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        if (ImGui::Begin(InputDisplayWindows[side], nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos(), centre(at.x + r, at.y + r);
            const unsigned now = history.empty() ? 0 : history.front().buttons;
            // Thick arrows round the edge, the held one lit; Both keeps their middle for a smaller gate.
            const bool arrows = shown != InputDisplay::Stick, gated = shown != InputDisplay::Arrows;
            if (arrows) {
                const ImVec2 held = Way(now);
                const float h = r * (gated ? .42f : .62f), step = r - h / 2;
                for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) if (dx || dy)
                    glyphs::BoldArrow(draw, ImVec2(centre.x + dx * step, centre.y + dy * step), h, static_cast<char>('5' + dx - 3 * dy),
                        held.x == dx && held.y == dy ? palette::Ember : IM_COL32(96, 98, 104, 255));
            }
            if (gated) {
                const float g = arrows ? r * .45f : r, unit = g / r;
                // The gate's corners point the eight ways the stick goes.
                ImVec2 gate[8];
                for (int i = 0; i < 8; ++i) gate[i] = ImVec2(centre.x + g * std::cos(i * .785398f), centre.y + g * std::sin(i * .785398f));
                draw->AddConvexPolyFilled(gate, 8, IM_COL32(86, 88, 94, 220));
                draw->AddPolyline(gate, 8, IM_COL32(150, 152, 158, 255), ImDrawFlags_Closed, 1.5f * hudScale);
                draw->AddCircleFilled(centre, 2 * hudScale * unit, IM_COL32(150, 152, 158, 255));
                // A run ends on a button too; the line takes a point only where the direction changes.
                ImVec2 trail[InputTrail];
                int points = 0;
                unsigned frames = 0;
                for (const auto& run : history) {
                    const ImVec2 way = Way(run.buttons);
                    const float reach = (way.x && way.y ? .7071f : 1.f) * g * .8f;
                    const ImVec2 at(centre.x + way.x * reach, centre.y + way.y * reach);
                    if (!points || at.x != trail[points - 1].x || at.y != trail[points - 1].y) trail[points++] = at;
                    if ((frames += run.frames) >= InputTrail) break;
                }
                for (int i = 1; i < points; ++i)
                    draw->AddLine(trail[i - 1], trail[i], (palette::Ember & ~IM_COL32_A_MASK) | (static_cast<ImU32>(230 - 200 * i / points) << IM_COL32_A_SHIFT), 3 * hudScale * unit);
                const ImVec2 ball = points ? trail[0] : centre;
                draw->AddCircleFilled(ball, 6 * hudScale * unit, palette::Ember);
                draw->AddCircle(ball, 6 * hudScale * unit, IM_COL32(20, 20, 20, 255), 0, 1);
            }
            for (int button = 0; button < static_cast<int>(gamepads::AttackMasks.size()); ++button)
                glyphs::Disc(draw, ImVec2(at.x + 2 * r + 2 * gap + button % 3 * (disc + gap), centre.y + (button < 3 ? -disc - gap / 2 : gap / 2)), disc,
                    button < 3 ? 'P' : 'K', glyphs::Strength[button % 3], (now & gamepads::AttackMasks[button]) ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 70));
            ImGui::Dummy(ImVec2(inner, 2 * r));
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        area.Cover({x, hudBottom - 2 * r - 2 * pad, x + outer, hudBottom});
    }
    return area;
}
static_assert(training::MeterOptions{}.position == static_cast<int>(TrainingHudBottom * 100 + .5f), "The meter's default place left the super meters' edge");
float TrainingMeterBottom() { return TrainingMeterOptions().position / 100.f; }
// The replay strip keeps below this meter wherever the player put it.
[[maybe_unused]] static const bool s_replayStripKnowsTheMeter = (SetReplayStripMeterBottom(&TrainingMeterBottom), true);
TrainingHudInput DrawTrainingHud(const training::View& view, bool called) {
    if (!view.available) return {};
    const auto* vp = ImGui::GetMainViewport();
    const float hudScale = TrainingHudScale();
    const float width = TrainingMeterWidth(hudScale);
    // The game's super meters and their SUPER! banners start about 17% above
    // the bottom edge and scale with the height, so the meter sits just above
    // them unless the player moved it (MeterOptions::position).
    const auto options = TrainingMeterOptions();
    const float hudBottom = vp->Pos.y + vp->Size.y * TrainingMeterBottom();
    const ImVec2 hudTop = MeterWindow(view, hudScale, width, hudBottom, options);
    TrainingHudArea area{hudTop.x, hudTop.y, hudTop.x + width, hudBottom};
    if (options.inputs != InputDisplay::Off) area.Cover(DrawInputDisplay(view, options.inputs, hudScale, hudTop.x, width, hudBottom));
    auto input = DrawTrainingShortcuts(view, called, hudTop.x, hudTop.y, width, hudScale);
    area.Cover(input.area);
    input.area = area;
    return input;
}
} }
