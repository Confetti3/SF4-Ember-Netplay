#include "PublicRoomsCards.hxx"
#include "ApplicationShell.hxx"
#include "GameMenu.hxx"
#include "MenuProbes.hxx"
#include "MenuRows.hxx"
#include "PublicRoomsPanel.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace sf4e { namespace ui {
namespace {
using netplay::publicrooms::Room;
constexpr ImU32 PillFill = IM_COL32(38, 34, 30, 255), TrackFill = IM_COL32(60, 54, 48, 255), ChipDark = IM_COL32(16, 15, 14, 230),
    NearFill = IM_COL32(30, 44, 30, 235), Ink = IM_COL32(20, 19, 18, 255), Dimmed = IM_COL32(8, 7, 6, 120),
    Border = IM_COL32(255, 255, 255, 14), EmberFaint = IM_COL32(255, 135, 56, 40),
    StatusFill = IM_COL32(56, 51, 46, 255), MetaColor = IM_COL32(158, 147, 134, 255),
    FaceBacking = IM_COL32(88, 80, 72, 255), FaceBorder = IM_COL32(255, 255, 255, 46);
// How many faces a room card shows before "+N".
constexpr std::size_t CardFaces = 6;

float Measure(float size, const char* text) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text).x; }
// The largest size up to `natural` at which `text` fits `width` on one line.
float FitSize(float natural, const char* text, float width) {
    const float measured = Measure(natural, text);
    return measured > width ? natural * (std::max)(1.f, width) / measured : natural;
}
// `text` cut to `width` with "..." (a player's own words may elide); empty when not even the dots fit.
std::string Elide(float size, const std::string& text, float width) {
    if (Measure(size, text.c_str()) <= width) return text;
    const float dots = Measure(size, "...");
    if (width <= dots) return {};
    const char* end = nullptr;
    ImGui::GetFont()->CalcTextSizeA(size, (std::max)(1.f, width - dots), 0, text.c_str(), nullptr, &end);
    return std::string(text.c_str(), end) + "...";
}
void Text(ImVec2 at, float size, ImU32 color, const std::string& text) {
    if (!text.empty()) ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size, at, color, text.c_str());
}
// An interface string on one line, shrunk to `width` and reported; returns the width it took.
float Line(ImVec2 at, float natural, ImU32 color, const std::string& text, float width, const char* probe, float interior) {
    const float size = FitSize(natural, text.c_str(), width), used = Measure(size, text.c_str());
    ReportMenuText(probe, size, interior, used, width);
    Text(at, size, color, text);
    return used;
}
// An interface paragraph wrapped to `width` within `height`, shrunk when it needs more; returns the height it took.
float Wrapped(ImVec2 at, float natural, ImU32 color, const std::string& text, float width, float height, const char* probe) {
    auto* font = ImGui::GetFont();
    float size = natural;
    ImVec2 used = font->CalcTextSizeA(size, FLT_MAX, width, text.c_str());
    for (int i = 0; i < 8 && used.y > height; ++i) {
        size *= .92f;
        used = font->CalcTextSizeA(size, FLT_MAX, width, text.c_str());
    }
    ReportMenuText(probe, used.y, height, used.x, width);
    ImGui::GetWindowDrawList()->AddText(font, size, at, color, text.c_str(), nullptr, width);
    return used.y;
}
ImU32 ErrorColor() { return ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Error)); }
ImU32 WithAlpha(ImU32 color, float alpha) {
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>((std::max)(0.f, (std::min)(1.f, alpha)) * 255.f) << IM_COL32_A_SHIFT);
}
// The skeleton shapes' pulse, 0.2 to 0.5.
float Pulse(double now) { return .35f + .15f * std::sin(static_cast<float>(now) * 4); }

float ChipWidth(const char* text, float maxWidth) {
    const float s = Scale(), pad = 7 * s;
    return Measure(FitSize(13 * s, text, maxWidth - 2 * pad), text) + 2 * pad;
}

bool Full(const Room& room) { return room.capacity > 0 && room.members >= room.capacity; }
bool Joinable(const Room& room) { return !room.locked && !Full(room); }
const Room* FindRoom(const ShellView& v, const std::string& id) {
    for (const auto& room : v.publicRooms.rooms) if (room.id == id) return &room;
    return nullptr;
}

// A fighter's face on a lighter tile, so dark portraits read, with a hairline border.
void DrawFace(int fighter, ImVec2 at, float size, bool dim) {
    const float s = Scale();
    const ImVec2 end(at.x + size, at.y + size);
    DrawCharacterPortrait(fighter >= 0 ? fighter : -1, at, end, FaceBacking);
    auto* d = ImGui::GetWindowDrawList();
    if (dim) d->AddRectFilled(at, end, Dimmed);
    d->AddRect(at, end, FaceBorder, 2 * s, 0, 1.f);
}

// A pill-shaped action: the main call filled, the other outlined. Returns its width.
float DrawAction(ImVec2 at, const std::string& text, float height, bool filled, bool enabled, float maxWidth, const char* probe) {
    const float s = Scale(), pad = height * .5f + 2 * s, natural = (height >= 32 * s ? 16 : 15) * s;
    const float size = FitSize(natural, text.c_str(), maxWidth - 2 * pad), used = Measure(size, text.c_str());
    ReportMenuText(probe, size, height, used + 2 * pad, maxWidth);
    auto* d = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + used + 2 * pad, at.y + height);
    const ImU32 accent = enabled ? palette::Ember : palette::Muted;
    if (filled && enabled) d->AddRectFilled(at, end, palette::Ember, height * .5f);
    else d->AddRect(at, end, accent, height * .5f, 0, 1.5f * s);
    Text(ImVec2(at.x + pad, at.y + (height - size) * .5f), size, filled && enabled ? Ink : accent, text);
    return used + 2 * pad;
}

// A strip along the card's foot with an accent segment sliding across it: a wait with no end in sight.
void DrawBusyBar(ImVec2 min, ImVec2 max, double now) {
    const float s = Scale(), x0 = min.x + 8 * s, x1 = max.x - 8 * s, y1 = max.y - 4 * s, y0 = y1 - 3 * s, span = x1 - x0, segment = span * .3f;
    if (span <= 0) return;
    auto* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), EmberFaint, 1.5f * s);
    const float t = static_cast<float>(std::fmod(now, 1.2) / 1.2), a = x0 + (span + segment) * t - segment;
    const float from = (std::max)(a, x0), to = (std::min)(a + segment, x1);
    if (to > from) d->AddRectFilled(ImVec2(from, y0), ImVec2(to, y1), palette::Ember, 1.5f * s);
}

void DrawFrame(ImVec2 min, ImVec2 max, bool focused) {
    const float s = Scale(), rounding = PublicCardRounding * s;
    auto* d = ImGui::GetWindowDrawList();
    d->AddRect(min, max, Border, rounding);
    if (focused) d->AddRect(ImVec2(min.x + s, min.y + s), ImVec2(max.x - s, max.y - s), palette::Ember, rounding, 0, 2 * s);
}

void DrawCell(const MenuEntry& e, ImVec2 min, ImVec2 max) {
    const float s = Scale(), pad = 8 * s, height = max.y - min.y;
    const std::string& label = e.label;
    const float size = FitSize(ImGui::GetFontSize(), label.c_str(), max.x - min.x - 2 * pad), used = Measure(size, label.c_str());
    ReportMenuText(e.id.c_str(), size, height, used, max.x - min.x - 2 * pad);
    Text(ImVec2(min.x + (max.x - min.x - used) * .5f, min.y + (height - size) * .5f - s), size, e.enabled ? palette::Ivory : palette::Muted, label);
    // Quick join is the main call of the toolbar.
    if (e.id == "pr-quick" && e.enabled)
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x + 8 * s, max.y - 7 * s), ImVec2(max.x - 8 * s, max.y - 4 * s), palette::Ember, 1.5f * s);
}

// A room: name and seats, where and whose, who is in it and by what rules. A phase other
// than Idle (Joining, Opening) swaps the seat count for its word and runs the bar.
void DrawRoomCard(const ShellView& v, const MenuEntry& e, PublicRoomsPanel::CardPhase phase, const Room& room, ImVec2 min, ImVec2 max, bool focused, double now) {
    using Phase = PublicRoomsPanel::CardPhase;
    const float s = Scale(), w = max.x - min.x, h = max.y - min.y, pad = 12 * s, rounding = PublicCardRounding * s;
    const bool narrow = h < 90 * s, full = Full(room), busy = phase != Phase::Idle, dimmed = (room.locked || full) && !busy;
    // A room with nothing to show under its name and place (a bridge that sends no details) centres them.
    const bool bare = PublicRoomBare(room) && !busy;
    auto* d = ImGui::GetWindowDrawList();
    auto* font = ImGui::GetFont();
    if (dimmed) d->AddRectFilled(min, max, Dimmed, rounding);
    const ImU32 ink = dimmed ? palette::Muted : palette::Ivory;
    const bool visible = ImGui::IsRectVisible(min, max);

    // Vertical rhythm: the name, the host and place, then the faces.
    const float nameFont = 20 * s, metaFont = 14 * s, pillH = 22 * s, barWidth = 64 * s;
    float nameY = min.y + (narrow ? 5 : 9) * s, metaY = min.y + (narrow ? 29 : 35) * s, pillY = min.y + (narrow ? 5 : 9) * s;
    if (bare) {
        nameY = min.y + (h - (nameFont + 6 * s + metaFont)) * .5f;
        metaY = nameY + nameFont + 6 * s;
        pillY = min.y + (h - (pillH + 8 * s)) * .5f;
    }

    // Right column: the seat count (or the state) in a pill, and how full the room is under it.
    const std::string count = std::to_string(room.members) + "/" + std::to_string(room.capacity);
    const std::string pillText = phase == Phase::Joining ? loc::T("public.joining_value") : phase == Phase::Opening ? loc::T("public.connecting") : count;
    const float maxPill = (std::max)(80 * s, w * .4f), pillFont = 14 * s;
    const float pillSize = FitSize(pillFont, pillText.c_str(), maxPill - 20 * s), textWidth = Measure(pillSize, pillText.c_str());
    const float pillW = textWidth + 20 * s;
    const ImVec2 pillMin(max.x - pad - pillW, pillY);
    if (busy) ReportMenuText("pr-room-state", pillSize, pillH, pillW, maxPill);
    d->AddRectFilled(pillMin, ImVec2(pillMin.x + pillW, pillMin.y + pillH), PillFill, pillH * .5f);
    d->AddText(font, pillSize, ImVec2(pillMin.x + 10 * s, pillMin.y + (pillH - pillSize) * .5f), busy ? palette::Ember : full ? palette::Muted : palette::Ivory, pillText.c_str());
    if (room.capacity > 0) {
        const ImVec2 bar(max.x - pad - barWidth, pillMin.y + pillH + 5 * s);
        const float filled = barWidth * (std::min)(1.f, static_cast<float>(room.members) / room.capacity);
        d->AddRectFilled(bar, ImVec2(bar.x + barWidth, bar.y + 3 * s), TrackFill, 1.5f * s);
        if (filled > 0) d->AddRectFilled(bar, ImVec2(bar.x + filled, bar.y + 3 * s), dimmed ? palette::Muted : palette::Ember, 1.5f * s);
    }
    const float left = min.x + pad, rightLimit = max.x - pad - (std::max)(pillW, barWidth) - 12 * s, lineWidth = rightLimit - left;

    // Line 1: the room's name, set as the title.
    if (visible) NoteUserText(e.label);
    ImGui::PushFont(HeadingFont());
    Text(ImVec2(left, nameY), nameFont, ink, Elide(nameFont, e.label, lineWidth));
    ImGui::PopFont();

    // Line 2: the host and the region, and a mark when the room is near.
    const std::string region = room.region.empty() ? std::string() : PublicRoomsPanel::RegionLabel(room.region);
    const std::string known = PublicRoomsPanel::KnownRegion(v);
    const bool near = !known.empty() && room.region == known;
    const std::string nearText = loc::T("public.near_you");
    const float nearWidth = near ? ChipWidth(nearText.c_str(), lineWidth * .45f) : 0;
    const float metaWidth = lineWidth - (near ? nearWidth + 8 * s : 0);
    std::string meta;
    float interfaceWidth = 0;
    if (visible && !room.hostName.empty()) NoteUserText(room.hostName);
    if (!room.hostName.empty() && !region.empty()) {
        interfaceWidth = Measure(metaFont, loc::Tf("public.card_meta", "", region).c_str());
        const std::string host = Elide(metaFont, room.hostName, metaWidth - interfaceWidth);
        if (host.empty()) { meta = region; interfaceWidth = Measure(metaFont, region.c_str()); }
        else meta = loc::Tf("public.card_meta", host, region);
    } else if (!room.hostName.empty()) meta = Elide(metaFont, room.hostName, metaWidth);
    else { meta = region; interfaceWidth = Measure(metaFont, region.c_str()); }
    const float chipY = metaY + (metaFont - 20 * s) * .5f;
    if (!meta.empty()) {
        if (interfaceWidth > 0) ReportMenuText("pr-room-meta", metaFont, 20 * s, interfaceWidth, metaWidth);
        Text(ImVec2(left, metaY), metaFont, MetaColor, meta);
        if (near) DrawChip(ImVec2(left + Measure(metaFont, meta.c_str()) + 8 * s, chipY), nearText.c_str(), palette::Ready, NearFill, "pr-chip-near", nearWidth);
    } else if (near) DrawChip(ImVec2(left, chipY), nearText.c_str(), palette::Ready, NearFill, "pr-chip-near", nearWidth);
    if (bare) {
        if (busy) DrawBusyBar(min, max, now);
        DrawFrame(min, max, focused);
        return;
    }

    // Line 3: the faces and the rules on the left, the room's standing on the right.
    const float face = (narrow ? 22 : 32) * s, gap = 4 * s, y3 = min.y + (narrow ? 46 : 60) * s, chipTop = y3 + (face - 20 * s) * .5f;
    float rightX = max.x - pad;
    const auto standing = [&](const char* text, ImU32 foreground, ImU32 background, const char* probe) {
        const float width = ChipWidth(text, (w - 2 * pad) * .5f);
        rightX -= width;
        DrawChip(ImVec2(rightX, chipTop), text, foreground, background, probe, width);
        rightX -= 6 * s;
    };
    if (room.locked) standing(loc::T("public.locked"), palette::Muted, StatusFill, "pr-chip-locked");
    else if (full) standing(loc::T("public.full"), palette::Muted, StatusFill, "pr-chip-full");
    if (room.playing > 0) standing(loc::T("public.in_match"), Ink, palette::Ready, "pr-chip-match");
    const float limit = rightX + 6 * s - 8 * s;
    float x = left;
    std::size_t shown = (std::min)(room.fighters.size(), CardFaces);
    while (shown > 0 && x + shown * (face + gap) > limit) --shown;
    for (std::size_t i = 0; i < shown; ++i) {
        DrawFace(room.fighters[i], ImVec2(x, y3), face, dimmed);
        x += face + gap;
    }
    if (shown > 0) {
        x += 2 * s;
        if (room.members > shown) {
            const std::string more = "+" + std::to_string(room.members - shown);
            const float size = 14 * s, width = Measure(size, more.c_str());
            if (x + width <= limit) { Text(ImVec2(x, y3 + (face - size) * .5f), size, palette::Muted, more); x += width + 10 * s; }
        } else x += 4 * s;
    }
    // A room without sets says nothing about them here; the preview does.
    if (!narrow && room.setFormat > 0) {
        const std::string set = SetLengthText(static_cast<room::SetFormat>(room.setFormat));
        float chip = ChipWidth(set.c_str(), limit - x);
        if (x + chip <= limit) { DrawChip(ImVec2(x, chipTop), set.c_str(), palette::Ember, ChipDark, "pr-chip-set", chip); x += chip + 6 * s; }
        if (room.rotation >= 0) {
            const char* rotation = RotationText(static_cast<room::RotationMode>(room.rotation));
            chip = ChipWidth(rotation, limit - x);
            if (x + chip <= limit) DrawChip(ImVec2(x, chipTop), rotation, palette::Muted, ChipDark, "pr-chip-rotation", chip);
        }
    }
    if (busy) DrawBusyBar(min, max, now);
    DrawFrame(min, max, focused);
}

// A message in a card: the sentence, and under it the one thing that can be done.
void DrawMessage(const MenuEntry& e, ImVec2 min, ImVec2 max, ImU32 color, bool focused) {
    const float s = Scale(), pad = 12 * s, edge = 16 * s, width = max.x - min.x - 2 * pad, pillH = 30 * s;
    const bool action = !e.hint.empty();
    const float labelHeight = max.y - min.y - 2 * edge - (action ? pillH + 8 * s : 0);
    Wrapped(ImVec2(min.x + pad, min.y + edge), 18 * s, color, e.label, width, labelHeight, e.id.c_str());
    if (action) DrawAction(ImVec2(min.x + pad, max.y - edge - pillH), e.hint, pillH, false, e.enabled, width, (e.id + "-action").c_str());
    DrawFrame(min, max, focused);
}

// The first look: the question, and bars where its answer will be.
void DrawChecking(const MenuEntry& e, ImVec2 min, ImVec2 max, bool focused, double now) {
    const float s = Scale(), pad = 12 * s, width = max.x - min.x - 2 * pad, rounding = 5 * s;
    auto* d = ImGui::GetWindowDrawList();
    Wrapped(ImVec2(min.x + pad, min.y + 14 * s), 18 * s, palette::Ivory, e.label, width, 40 * s, e.id.c_str());
    const float alpha = Pulse(now);
    const ImU32 bar = IM_COL32(120, 108, 96, 255);
    d->AddRectFilled(ImVec2(min.x + pad, min.y + 66 * s), ImVec2(min.x + pad + width * .7f, min.y + 76 * s), WithAlpha(bar, alpha), rounding);
    d->AddRectFilled(ImVec2(min.x + pad, min.y + 84 * s), ImVec2(min.x + pad + width * .45f, min.y + 94 * s), WithAlpha(bar, alpha), rounding);
    DrawBusyBar(min, max, now);
    DrawFrame(min, max, focused);
}

// The list on its way: a caption over three rooms' worth of pulsing shapes.
void DrawLoading(const MenuEntry& e, ImVec2 min, ImVec2 max, bool focused, double now) {
    const float s = Scale(), pad = 12 * s, width = max.x - min.x - 2 * pad, rounding = PublicCardRounding * s;
    auto* d = ImGui::GetWindowDrawList();
    Line(ImVec2(min.x + pad, min.y + 10 * s), 14 * s, palette::Muted, e.label, width, e.id.c_str(), 18 * s);
    const float alpha = Pulse(now), cardHeight = (max.y - min.y - 34 * s - 12 * s - 10 * s) / 3;
    const ImU32 block = IM_COL32(120, 108, 96, 255);
    for (int i = 0; i < 3; ++i) {
        const ImVec2 a(min.x + pad, min.y + 34 * s + i * (cardHeight + 6 * s)), b(max.x - pad, a.y + cardHeight);
        d->AddRectFilled(a, b, WithAlpha(PillFill, alpha * 1.4f), rounding);
        const auto bar = [&](float x, float y, float barWidth, float barHeight, float strength = 1) {
            d->AddRectFilled(ImVec2(a.x + x, a.y + y), ImVec2(a.x + x + barWidth, a.y + y + barHeight), WithAlpha(block, alpha * strength), barHeight * .5f);
        };
        const float inner = b.x - a.x;
        bar(12 * s, 14 * s, inner * .34f, 14 * s);
        bar(12 * s, 36 * s, inner * .22f, 10 * s, .8f);
        for (int face = 0; face < 3; ++face) bar(12 * s + face * 25 * s, 52 * s, 22 * s, 22 * s, .8f);
        bar(inner - 12 * s - 54 * s, 12 * s, 54 * s, 22 * s, .9f);
    }
    DrawFrame(min, max, focused);
}

// The call to set up public rooms (and the Ember ID unlock that must come first), with its progress:
// `setup` says whether the setup runs (its step under the text) or failed (why, in place of the text).
void DrawSetup(const MenuEntry& e, const PublicSetupView& setup, ImVec2 min, ImVec2 max, bool focused, double now, bool needsId) {
    const float s = Scale(), pad = 12 * s, ctaHeight = 32 * s, rounding = PublicCardRounding * s;
    // A few faces of the roster on the right make the card an invitation; a narrow list has no room for them.
    const float collage = 2 * 56 * s + 6 * s;
    const bool faces = !needsId && max.x - min.x >= 520 * s;
    const float width = max.x - min.x - 2 * pad - (faces ? collage + 16 * s : 0);
    const bool running = !needsId && setup.Running(), failed = !needsId && setup.step == PublicSetupStep::Failed;
    const std::string line = running || failed ? PublicRoomsPanel::SetupLine(setup) : std::string();
    const std::string title = needsId ? e.label : std::string(loc::T("public.setup_title"));
    const std::string body = needsId ? e.detail : failed ? line : std::string(loc::T("public.setup_body"));
    const float ctaY = max.y - 14 * s - ctaHeight;
    auto* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(min, ImVec2(min.x + 3 * s, max.y), palette::Ember, rounding, ImDrawFlags_RoundCornersLeft);
    if (faces) {
        static const int roster[] = {1, 6, 17, 24};
        const float face = 56 * s, x = max.x - pad - collage, y = min.y + (max.y - min.y - collage) * .5f;
        for (int i = 0; i < 4; ++i) {
            const ImVec2 at(x + (i % 2) * (face + 6 * s), y + (i / 2) * (face + 6 * s));
            DrawCharacterPortrait(roster[i], at, ImVec2(at.x + face, at.y + face));
            d->AddRect(at, ImVec2(at.x + face, at.y + face), IM_COL32(0, 0, 0, 120), 3 * s);
        }
    }
    Line(ImVec2(min.x + pad, min.y + 14 * s), 22 * s, palette::Ivory, title, width, needsId ? "pr-setup-id" : "pr-setup-title", 28 * s);
    Wrapped(ImVec2(min.x + pad, min.y + 46 * s), 16 * s, failed ? ErrorColor() : palette::Muted, body, width, ctaY - 8 * s - (min.y + 46 * s), needsId ? "pr-setup-id-body" : "pr-setup-body");
    if (running) {
        Line(ImVec2(min.x + pad, ctaY + (ctaHeight - 15 * s) * .5f), 15 * s, palette::Ivory, line, width, "pr-setup-step", ctaHeight);
        DrawBusyBar(min, max, now);
    } else if (needsId) DrawAction(ImVec2(min.x + pad, ctaY), loc::T("identity.unlock"), ctaHeight, true, e.enabled, width, "pr-setup-id-action");
    else DrawAction(ImVec2(min.x + pad, ctaY), e.label, ctaHeight, true, e.enabled, width, "pr-setup-action");
    DrawFrame(min, max, focused);
}
}

bool PublicRoomBare(const Room& room) {
    return !room.hasDetails && room.fighters.empty() && room.setFormat < 0 && !room.locked && !Full(room) && room.playing == 0;
}

float DrawChip(ImVec2 at, const char* text, ImU32 foreground, ImU32 background, const char* probe, float maxWidth) {
    const float s = Scale(), pad = 7 * s, height = 20 * s;
    if (maxWidth < 2 * pad + 8 * s) return 0;
    const float font = FitSize(13 * s, text, maxWidth - 2 * pad), used = Measure(font, text);
    ReportMenuText(probe, font, height, used + 2 * pad, maxWidth);
    auto* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(at, ImVec2(at.x + used + 2 * pad, at.y + height), background, height * .5f);
    d->AddText(ImGui::GetFont(), font, ImVec2(at.x + pad, at.y + (height - font) * .5f), foreground, text);
    return used + 2 * pad;
}

bool DrawPublicCard(const ShellView& v, const PublicRoomsPanel& panel, const PublicSetupView& setup, PublicRoomsPanel::CardPhase phase,
                    const MenuEntry& e, ImVec2 min, ImVec2 max, bool focused, double now) {
    const std::string& id = e.id;
    if (id == "pr-quick" || id == "pr-create" || id == "pr-filter" || id == "pr-paste") { DrawCell(e, min, max); return true; }
    if (id.compare(0, 8, "pr-room:") == 0) {
        const Room* room = FindRoom(v, id.substr(8));
        if (!room) return false;
        DrawRoomCard(v, e, phase, *room, min, max, focused, now);
        return true;
    }
    if (id == "pr-opening") {
        Room room;
        if (const Room* current = panel.CurrentRoom()) room = *current;
        DrawRoomCard(v, e, phase, room, min, max, focused, now);
        return true;
    }
    if (id == "pr-none" || id == "pr-show-all") { DrawMessage(e, min, max, palette::Ivory, focused); return true; }
    if (id == "pr-error") { DrawMessage(e, min, max, ErrorColor(), focused); return true; }
    if (id == "pr-checking") { DrawChecking(e, min, max, focused, now); return true; }
    if (id == "pr-loading") { DrawLoading(e, min, max, focused, now); return true; }
    if (id == "pr-setup" || id == "pr-setup-id") { DrawSetup(e, setup, min, max, focused, now, id == "pr-setup-id"); return true; }
    return false;
}

void DrawPublicPreview(const ShellView& v, const PublicRoomsPanel& panel, const std::string& id) {
    if (id.compare(0, 8, "pr-room:") != 0) return;
    const Room* room = FindRoom(v, id.substr(8));
    if (!room) return;
    const float s = Scale(), gap = 6 * s, pad = 14 * s, rounding = PublicCardRounding * s;
    const ImVec2 space = ImGui::GetContentRegionAvail();
    // A pane on top of a narrow list is short: the faces only, and only when they fit in what is left.
    const bool compact = ImGui::GetWindowHeight() <= 145 * s + 1;
    const std::size_t count = room->fighters.size();
    if (compact && (count == 0 || space.y < 52 * s)) return;
    auto* d = ImGui::GetWindowDrawList();
    const auto capacity = [&](float width, float face) { return static_cast<std::size_t>((std::max)(1.f, std::floor((width + gap) / (face + gap)))); };
    const auto portrait = [&](int fighter, ImVec2 at, float face) { DrawFace(fighter, at, face, false); };
    if (compact) {
        const float face = 44 * s;
        const std::size_t shown = (std::min)(count, (std::min)(CardFaces, capacity(space.x - (count > capacity(space.x, face) ? 34 * s : 0), face)));
        const auto origin = ImGui::GetCursorScreenPos();
        for (std::size_t i = 0; i < shown; ++i) portrait(room->fighters[i], ImVec2(origin.x + i * (face + gap), origin.y), face);
        if (room->members > shown) {
            const std::string more = "+" + std::to_string(room->members - shown);
            const float size = 14 * s, x = origin.x + shown * (face + gap) + 2 * s;
            if (x + Measure(size, more.c_str()) <= origin.x + space.x) Text(ImVec2(x, origin.y + (face - size) * .5f), size, palette::Muted, more);
        }
        ImGui::Dummy(ImVec2(space.x, face));
        return;
    }
    ImGui::Dummy(ImVec2(0, 8 * s));
    const ImU32 panelFill = IM_COL32(16, 15, 14, 190);
    // The players: their faces, as large as the pane allows.
    if (count > 0) {
        const float inner = space.x - 2 * pad;
        const float face = (std::max)(44 * s, (std::min)(96 * s, (inner - 5 * gap) / 6));
        const std::size_t perRow = (std::min)(CardFaces, capacity(inner, face)), rows = (count + perRow - 1) / perRow;
        const float caption = 18 * s, height = pad + caption + 8 * s + rows * face + (rows - 1) * gap + pad;
        const auto at = ImGui::GetCursorScreenPos();
        d->AddRectFilled(at, ImVec2(at.x + space.x, at.y + height), panelFill, rounding);
        Line(ImVec2(at.x + pad, at.y + pad), 14 * s, palette::Muted, loc::T("public.players"), inner, "pr-preview-players", caption);
        const float top = at.y + pad + caption + 8 * s;
        for (std::size_t i = 0; i < count; ++i)
            portrait(room->fighters[i], ImVec2(at.x + pad + (i % perRow) * (face + gap), top + (i / perRow) * (face + gap)), face);
        if (room->members > count) {
            const std::string more = "+" + std::to_string(room->members - count);
            const float size = 14 * s, x = at.x + pad + ((count - 1) % perRow + 1) * (face + gap) + 2 * s;
            if (x + Measure(size, more.c_str()) <= at.x + space.x - pad) Text(ImVec2(x, top + ((count - 1) / perRow) * (face + gap) + (face - size) * .5f), size, palette::Muted, more);
        }
        ImGui::Dummy(ImVec2(space.x, height + 8 * s));
    }
    // The rules, how long the room has been open and who moderates it.
    const double open = panel.OpenSeconds(v, *room, ImGui::GetTime());
    const bool rules = room->setFormat >= 0;
    const float line = 18 * s, chipRow = 20 * s;
    float height = 2 * pad - 6 * s;
    int rowsCount = 0;
    const auto add = [&](float rowHeight) { height += rowHeight + 6 * s; ++rowsCount; };
    if (rules) add(chipRow);
    if (open >= 0) add(line);
    if (rowsCount == 0) return;
    const auto at = ImGui::GetCursorScreenPos();
    d->AddRectFilled(at, ImVec2(at.x + space.x, at.y + height), panelFill, rounding);
    float y = at.y + pad;
    const float inner = space.x - 2 * pad;
    if (rules) {
        const bool sets = room->setFormat != 0;
        // The chip's text, not its edge, lines up with the lines under it.
        float x = at.x + pad - 7 * s;
        const std::string set = SetLengthText(static_cast<room::SetFormat>(room->setFormat));
        x += DrawChip(ImVec2(x, y), set.c_str(), sets ? palette::Ember : palette::Muted, ChipDark, "pr-preview-set", inner) + 6 * s;
        if (sets && room->rotation >= 0)
            DrawChip(ImVec2(x, y), RotationText(static_cast<room::RotationMode>(room->rotation)), palette::Muted, ChipDark, "pr-preview-rotation", at.x + space.x - pad - x);
        y += chipRow + 6 * s;
    }
    if (open >= 0) {
        const long long minutes = (std::max)(1LL, static_cast<long long>(open / 60));
        const std::string age = minutes < 60 ? loc::Tf("public.age_minutes", minutes) : loc::Tf("public.age_hours", minutes / 60);
        Line(ImVec2(at.x + pad, y), 14 * s, palette::Muted, loc::Tf("public.open_for", age), inner, "pr-preview-open", line);
    }
    ImGui::Dummy(ImVec2(space.x, height));
}
} }
