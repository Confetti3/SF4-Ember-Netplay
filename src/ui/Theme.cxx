#include "Theme.hxx"
#include "EmbeddedFonts.hxx"
#include "EmbeddedBrand.hxx"
#include "MenuProbes.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <limits>
#include <chrono>
#include <map>
#include <optional>
#include <vector>

namespace sf4e { namespace ui {
namespace palette {
ImU32 Ember = IM_COL32(255, 135, 56, 255);
ImU32 Ivory = IM_COL32(243, 235, 221, 255), Muted = IM_COL32(181, 169, 155, 255);
}
namespace {
unsigned accentRgb = accent::Default;
ImGuiContext* accentStyleContext = nullptr;
std::function<void(const char*)> atlasBuildLog;
void ReportAtlasBuild(bool complete, int width, int height, std::size_t userGlyphs, bool forUserText) {
    if (!atlasBuildLog) return;
    char line[160];
    std::snprintf(line, sizeof(line), "Theme: font atlas rebuilt %dx%d user_glyphs=%u reason=%s%s", width, height,
        static_cast<unsigned>(userGlyphs), forUserText ? "player_text" : "interface", complete ? "" : " incomplete");
    atlasBuildLog(line);
}
ImVec4 Color(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f,
                  (rgb & 255) / 255.f, alpha);
}
const unsigned Ink = 0x141312, Panel = 0x211E1B, Border = 0x443A31;
const unsigned Ivory = 0xF3EBDD, Muted = 0xB5A99B;
const float BodySize = 18.f;

float Luminance(ImVec4 color) {
    const auto linear = [](float value) {
        return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
    };
    return .2126f * linear(color.x) + .7152f * linear(color.y) + .0722f * linear(color.z);
}
ImVec4 ReadableAccent(ImVec4 chosen) {
    // The same primary accent draws labels, focus strokes and filled badges
    // with dark text. Keep it legible on the normal panel (4.5:1) and the
    // brighter focused card (3:1), without changing the saved RGB or panels.
    // The small margin survives conversion to 8-bit draw-list colors.
    const float target = (std::max)(4.5f * (Luminance(AccentShade(Panel)) + .05f) - .05f,
        3.f * (Luminance(AccentShade(ImVec4(.40f,.23f,.12f,.9f))) + .05f) - .05f) + .002f;
    if (Luminance(chosen) >= target) return chosen;
    const auto mix = [&](float amount) {
        return ImVec4(chosen.x + (1.f - chosen.x) * amount, chosen.y + (1.f - chosen.y) * amount,
            chosen.z + (1.f - chosen.z) * amount, chosen.w);
    };
    float low = 0, high = 1;
    for (int i = 0; i < 16; ++i) {
        const float middle = (low + high) * .5f;
        if (Luminance(mix(middle)) < target) low = middle;
        else high = middle;
    }
    return mix(high);
}

constexpr std::size_t ScriptFontCount = std::size(fonts::ScriptFonts);

// An atlas holds Inter's ranges and each script font's native language names,
// which never change, plus the active locale's catalog but its country names
// (loc::InterfaceText) when a script font draws it. That locale, or none, is
// all that tells two atlases apart.
std::optional<loc::Locale> CatalogInAtlas(loc::Locale active) {
    for (const auto& font : fonts::ScriptFonts)
        if (font.script == loc::ScriptOf(active)) return active;
    return std::nullopt;
}

// What the current atlas holds, and the script-font ranges it was built from,
// which ImGui reads by pointer and so must outlive the build.
std::optional<loc::Locale> atlasCatalog;
float interfaceSize = 1.f;
ImVector<ImWchar> scriptRanges[ScriptFontCount];

// Characters of player-written text that Inter cannot draw, which every script
// font is asked for. Each records when a role last drew it. A character stays
// wanted for glyphRetention after its last draw, so the wanted set follows the
// text on screen: a room's chat and names stop costing atlas space once they
// leave it. At most MaxUserGlyphs are wanted (each costs a glyph at three
// sizes), taken by role, then by which was noted first, so a set that fills
// the cap does not reshuffle from frame to frame. The atlas is rebuilt when
// text needs a character it lacks, not more than once every rebuildInterval so
// a burst of typing or chat costs one rebuild; characters no longer wanted are
// dropped by whichever rebuild comes next.
constexpr std::size_t UserTextRoles = 3;
struct UserGlyph {
    std::array<std::optional<std::chrono::steady_clock::time_point>, UserTextRoles> seen;
    std::size_t order = 0;
};
std::map<ImWchar, UserGlyph> userGlyphs;
std::size_t userGlyphOrder = 0;
std::vector<ImWchar> wantedUserGlyphs, atlasUserGlyphs; // sorted
constexpr std::size_t MaxUserGlyphs = 512;
std::chrono::milliseconds rebuildInterval(250), glyphRetention(10000);
std::chrono::steady_clock::time_point lastUserGlyphBuild;
bool userGlyphBuildSeen = false;

// Retires characters that no role has drawn within glyphRetention and fills
// wantedUserGlyphs from the rest, best role first when over the cap.
void UpdateWantedUserGlyphs(std::chrono::steady_clock::time_point now) {
    struct Ranked { std::size_t role, order; ImWchar glyph; };
    std::vector<Ranked> ranked;
    ranked.reserve(userGlyphs.size());
    for (auto it = userGlyphs.begin(); it != userGlyphs.end();) {
        std::size_t role = UserTextRoles;
        for (std::size_t r = 0; r < UserTextRoles && role == UserTextRoles; ++r)
            if (it->second.seen[r] && now - *it->second.seen[r] <= glyphRetention) role = r;
        if (role == UserTextRoles) { it = userGlyphs.erase(it); continue; }
        ranked.push_back({role, it->second.order, it->first});
        ++it;
    }
    if (ranked.size() > MaxUserGlyphs) {
        std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
            return a.role != b.role ? a.role < b.role : a.order < b.order; });
        ranked.resize(MaxUserGlyphs);
    }
    wantedUserGlyphs.clear();
    for (const auto& glyph : ranked) wantedUserGlyphs.push_back(glyph.glyph);
    std::sort(wantedUserGlyphs.begin(), wantedUserGlyphs.end());
}

bool InterDraws(unsigned codepoint) {
    for (const ImWchar* range = UiGlyphRanges; range[0] && range[1]; range += 2)
        if (codepoint >= range[0] && codepoint <= range[1]) return true;
    return false;
}

// Every CJK glyph costs atlas space, so a script font gets its languages'
// native names, for the language picker, and the catalog only if it draws it.
void BuildScriptRanges(std::optional<loc::Locale> catalog, const std::vector<ImWchar>& glyphs) {
    for (std::size_t i = 0; i < ScriptFontCount; ++i) {
        ImFontGlyphRangesBuilder builder;
        for (int locale = 0; locale < static_cast<int>(loc::Locale::Count); ++locale)
            if (loc::ScriptOf(static_cast<loc::Locale>(locale)) == fonts::ScriptFonts[i].script)
                builder.AddText(loc::NativeName(static_cast<loc::Locale>(locale)));
        if (catalog && loc::ScriptOf(*catalog) == fonts::ScriptFonts[i].script)
            builder.AddText(loc::InterfaceText(*catalog).c_str());
        for (const auto glyph : glyphs) builder.AddChar(glyph);
        scriptRanges[i].clear();
        builder.BuildRanges(&scriptRanges[i]);
    }
}
}

float Scale() {
    return ImGui::GetIO().FontGlobalScale;
}
float InterfaceScale() { return interfaceSize; }
ImFont* HeadingFont() {
    const auto& fonts = ImGui::GetIO().Fonts->Fonts;
    return fonts.Size == 3 ? fonts[1] : ImGui::GetFont();
}
ImFont* DiagnosticFont() {
    const auto& fonts = ImGui::GetIO().Fonts->Fonts;
    return fonts.Size == 3 ? fonts[2] : ImGui::GetFont();
}
const char* FontLicense() { return fonts::License; }
void SetUserGlyphRebuildInterval(std::chrono::milliseconds interval) { rebuildInterval = interval; }
void SetUserGlyphRetention(std::chrono::milliseconds retention) { glyphRetention = retention; }
void NoteUserText(const std::string& utf8, UserTextRole role) {
    std::optional<std::chrono::steady_clock::time_point> now;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        // ASCII and continuation bytes add nothing; everything else is decoded
        // and checked against the ranges Inter draws.
        if (lead < 0xC0) { ++i; continue; }
        const int length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (i + length > utf8.size()) return;
        unsigned codepoint = lead & (0xFFu >> (length + 1));
        for (int n = 1; n < length; ++n) codepoint = (codepoint << 6) | (static_cast<unsigned char>(utf8[i + n]) & 0x3Fu);
        i += length;
        if (codepoint > (std::numeric_limits<ImWchar>::max)() || InterDraws(codepoint)) continue;
        if (!now) now = std::chrono::steady_clock::now();
        auto& glyph = userGlyphs[static_cast<ImWchar>(codepoint)];
        // A character retired since it was last drawn starts over, behind the
        // ones still on screen.
        if (std::none_of(glyph.seen.begin(), glyph.seen.end(), [](const auto& t) { return t.has_value(); })) glyph.order = ++userGlyphOrder;
        glyph.seen[static_cast<std::size_t>(role)] = *now;
    }
}
void SetAtlasBuildLog(std::function<void(const char*)> log) { atlasBuildLog = std::move(log); }
ImVec4 AccentShade(unsigned orangeRgb, float alpha) {
    return AccentShade(Color(orangeRgb, alpha));
}
ImVec4 AccentShade(ImVec4 original) {
    if (accentRgb == accent::Default) return original;
    const auto reference = Color(accent::Default);
    if (original.x == reference.x && original.y == reference.y && original.z == reference.z) {
        auto primary = ImGui::ColorConvertU32ToFloat4(palette::Ember);
        primary.w = original.w;
        return primary;
    }
    float hue, saturation, value, referenceHue, referenceSaturation, referenceValue;
    const auto chosen = Color(accentRgb);
    ImGui::ColorConvertRGBtoHSV(chosen.x, chosen.y, chosen.z, hue, saturation, value);
    ImGui::ColorConvertRGBtoHSV(reference.x, reference.y, reference.z, referenceHue, referenceSaturation, referenceValue);
    float oldHue, oldSaturation, oldValue;
    ImGui::ColorConvertRGBtoHSV(original.x, original.y, original.z, oldHue, oldSaturation, oldValue);
    ImVec4 result(0, 0, 0, original.w);
    // Dark panels stay dark and pale text stays pale, even for a very bright
    // or dark chosen accent. Only decorative callers use this transformation.
    ImGui::ColorConvertHSVtoRGB(std::fmod(hue + oldHue - referenceHue + 1.f, 1.f),
        (std::min)(1.f, oldSaturation * saturation / referenceSaturation), oldValue, result.x, result.y, result.z);
    return result;
}
ImU32 AccentColor(ImU32 original) {
    if (accentRgb == accent::Default) return original;
    return ImGui::ColorConvertFloat4ToU32(AccentShade(ImGui::ColorConvertU32ToFloat4(original)));
}
void SetAccentColor(unsigned rgb) {
    rgb = accent::Normalize(rgb);
    if (rgb == accentRgb && accentStyleContext == ImGui::GetCurrentContext()) return;
    accentRgb = rgb;
    accentStyleContext = ImGui::GetCurrentContext();
    const auto selected = Color(accentRgb);
    palette::Ember = ImGui::ColorConvertFloat4ToU32(rgb == accent::Default ? selected : ReadableAccent(selected));
    palette::Ivory = AccentColor(IM_COL32(243, 235, 221, 255));
    palette::Muted = AccentColor(IM_COL32(181, 169, 155, 255));
    auto* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = AccentShade(Ivory);
    c[ImGuiCol_TextDisabled] = AccentShade(Muted);
    c[ImGuiCol_WindowBg] = AccentShade(Ink, .97f);
    c[ImGuiCol_ChildBg] = AccentShade(Panel);
    c[ImGuiCol_PopupBg] = AccentShade(Panel, .99f);
    c[ImGuiCol_Border] = AccentShade(Border);
    c[ImGuiCol_BorderShadow] = AccentShade(Ink, 0);
    c[ImGuiCol_FrameBg] = AccentShade(0x191715);
    c[ImGuiCol_TitleBg] = AccentShade(Panel);
    c[ImGuiCol_TitleBgCollapsed] = AccentShade(Ink, .95f);
    c[ImGuiCol_MenuBarBg] = AccentShade(Panel, .98f);
    c[ImGuiCol_ScrollbarBg] = AccentShade(Ink);
    c[ImGuiCol_ScrollbarGrab] = AccentShade(Border);
    c[ImGuiCol_Separator] = AccentShade(Border);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = AccentShade(Panel);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = AccentShade(Border);
    c[ImGuiCol_TableRowBg] = AccentShade(Ink, 0);
    c[ImGuiCol_FrameBgHovered] = AccentShade(0x393027);
    c[ImGuiCol_FrameBgActive] = AccentShade(0x4A3525);
    c[ImGuiCol_TitleBgActive] = AccentShade(0x3A2A1F);
    c[ImGuiCol_ScrollbarGrabHovered] = AccentShade(0x76583D);
    c[ImGuiCol_ScrollbarGrabActive] = c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = AccentShade(accent::Default);
    c[ImGuiCol_SliderGrabActive] = AccentShade(0xFFB16F);
    c[ImGuiCol_Button] = c[ImGuiCol_TableHeaderBg] = AccentShade(0x382D24);
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_TabHovered] = AccentShade(0x59402B);
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = AccentShade(0x76502F);
    c[ImGuiCol_Header] = AccentShade(0x3A2A1F);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = AccentShade(accent::Default);
    c[ImGuiCol_ResizeGrip] = AccentShade(accent::Default, .25f);
    c[ImGuiCol_ResizeGripHovered] = AccentShade(accent::Default, .65f);
    c[ImGuiCol_ResizeGripActive] = AccentShade(accent::Default);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = AccentShade(0x4A3525);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = AccentShade(accent::Default);
    c[ImGuiCol_TableRowBgAlt] = AccentShade(0x76614F, .09f);
    c[ImGuiCol_TextSelectedBg] = AccentShade(accent::Default, .3f);
    c[ImGuiCol_NavCursor] = c[ImGuiCol_PlotHistogram] = c[ImGuiCol_DragDropTarget] = AccentShade(accent::Default);
    c[ImGuiCol_PlotLines] = AccentShade(0xF3C58B);
    c[ImGuiCol_PlotLinesHovered] = c[ImGuiCol_PlotHistogramHovered] = AccentShade(0xFFB16F);
}
bool ApplyTheme(float dpiScale, float interfaceScale, unsigned rgb) {
    SetAccentColor(rgb);
    interfaceSize = interfaceScale;
    dpiScale = (std::max)(1.f, (std::min)(dpiScale * interfaceScale, 3.f));
    auto& io = ImGui::GetIO();
    const auto catalog = CatalogInAtlas(loc::Active());
    // Font configuration survives device resets; texture ownership stays with DX9.
    // A language change that needs other glyphs rebuilds the atlas, and so does
    // player text that needs them, once the last such rebuild is not too recent.
    const auto now = std::chrono::steady_clock::now();
    UpdateWantedUserGlyphs(now);
    const bool userGlyphsAdded = !std::includes(atlasUserGlyphs.begin(), atlasUserGlyphs.end(), wantedUserGlyphs.begin(), wantedUserGlyphs.end());
    const bool userGlyphsDue = userGlyphsAdded && (!userGlyphBuildSeen || now - lastUserGlyphBuild >= rebuildInterval);
    if (io.Fonts->Fonts.Size == 3 &&
        std::fabs(io.Fonts->Fonts[0]->FontSize - BodySize) < .01f &&
        std::fabs(io.FontGlobalScale - dpiScale) < .001f && atlasCatalog == catalog && !userGlyphsDue)
        return false;
    io.FontDefault = nullptr;
    io.Fonts->Clear();
    BuildScriptRanges(catalog, wantedUserGlyphs);
    atlasCatalog = catalog;
    if (userGlyphsAdded) { lastUserGlyphBuild = now; userGlyphBuildSeen = true; }
    atlasUserGlyphs = wantedUserGlyphs;
    // The catalog's font merges first so it draws the Han characters Japanese
    // and Chinese share.
    const auto drawsCatalog = [&](std::size_t font) { return catalog && fonts::ScriptFonts[font].script == loc::ScriptOf(*catalog); };
    std::vector<std::size_t> mergeOrder;
    for (std::size_t font = 0; font < ScriptFontCount; ++font)
        if (drawsCatalog(font)) mergeOrder.push_back(font);
    for (std::size_t font = 0; font < ScriptFontCount; ++font)
        // BuildRanges always writes the terminator; one entry means no glyphs.
        if (!drawsCatalog(font) && scriptRanges[font].Size > 1) mergeOrder.push_back(font);
    const float sizes[] = {BodySize, 28.f, 16.f};
    for (int i = 0; i < 3; ++i) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.GlyphRanges = UiGlyphRanges;
        // Keep logical sizes integral: ImGui 1.91 truncates SizePixels. Baking at
        // display density then scaling metrics preserves fractional Windows DPI.
        config.RasterizerDensity = i == 2 ? (std::max)(3.5f,dpiScale) : dpiScale;
        // ImGui stores an oversampled glyph at the oversampled size, so 3x2 costs
        // six times the atlas area. A font baked at two texels or more per logical
        // pixel (the diagnostic font always, body text from 200%) has the
        // subpixel precision already, and its atlas, which the game's 32-bit
        // address space has to allocate whole, is several times larger with it.
        const bool dense = config.RasterizerDensity >= 2.f;
        config.OversampleH = dense ? 1 : 3; config.OversampleV = dense ? 1 : 2;
        const auto* data = i == 1 ? fonts::Heading : fonts::Body;
        const int bytes = static_cast<int>(i == 1 ? sizeof(fonts::Heading) : sizeof(fonts::Body));
        std::snprintf(config.Name, sizeof(config.Name), "%s %.0fpx",
            i == 1 ? "Inter SemiBold" : "Inter Regular", sizes[i]*dpiScale);
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), bytes, sizes[i], &config);
        for (const auto font : mergeOrder) {
            ImFontConfig merged;
            merged.MergeMode = true;
            merged.FontDataOwnedByAtlas = false;
            merged.GlyphRanges = scriptRanges[font].Data;
            // Dense CJK outlines need no oversampling. The diagnostic font's
            // extra density is for the scaled match strip; cap its CJK glyphs
            // at 2x even at 300% DPI. Baking the whole catalog at that extra
            // density can double the atlas allocation after height rounding.
            // Body and heading glyphs still bake at the display's density.
            merged.OversampleH = merged.OversampleV = 1;
            // Noto Sans CJK sets more line space per em than Inter, so at Inter's
            // size its glyphs look small beside Latin text. Matching the line
            // span (capped, so one odd font cannot balloon the atlas) sizes both
            // scripts alike, with no vertical shift between them.
            const auto& source = fonts::ScriptFonts[font];
            const float interSpan = FontLineSpanEm(data, static_cast<std::size_t>(bytes));
            const float cjkSpan = FontLineSpanEm(source.data, source.bytes);
            const float fit = interSpan > 0 && cjkSpan > 0 ? (std::max)(1.f, (std::min)(cjkSpan / interSpan, 1.3f)) : 1.f;
            const float mergedSize = sizes[i] * fit;
            merged.GlyphOffset.y = 0;
            merged.RasterizerDensity = i == 2 ? (std::min)(config.RasterizerDensity, 2.f) : config.RasterizerDensity;
            std::snprintf(merged.Name, sizeof(merged.Name), "Noto Sans CJK %.0fpx", mergedSize*dpiScale);
            io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(source.data), static_cast<int>(source.bytes), mergedSize, &merged);
        }
    }
    io.FontGlobalScale = dpiScale;
    io.FontDefault = io.Fonts->Fonts[0];
    const int markIndex = io.Fonts->AddCustomRectRegular(brand::Size, brand::Size);
    IM_ASSERT(markIndex == 0); // Clear above makes this the first custom rectangle.
    const bool built = io.Fonts->Build();
    unsigned char* pixels = nullptr;
    int atlasWidth = 0, atlasHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
    const auto* mark = io.Fonts->GetCustomRectByIndex(markIndex);
    // The mark is copied into the atlas only where the build really placed
    // it: an unplaced rectangle reads 0xFFFF, which would write far past the
    // pixels, so a failed or partial build skips the mark instead.
    const bool markFits = built && pixels && mark && mark->IsPacked() &&
        mark->X + brand::Size <= atlasWidth && mark->Y + brand::Size <= atlasHeight;
    if (markFits)
        for (int row = 0; row < brand::Size; ++row)
            std::memcpy(pixels + ((mark->Y + row) * atlasWidth + mark->X) * 4,
                        brand::Pixels + row * brand::Size * 4, brand::Size * 4);
    ReportAtlasBuild(built && markFits, atlasWidth, atlasHeight, atlasUserGlyphs.size(), userGlyphsAdded);
    io.Fonts->TexPixelsUseColors = true;
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.Alpha = 1.f;
    style.DisabledAlpha = .6f;
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(12, 8);
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(8, 4);
    style.CellPadding = ImVec2(8, 6);
    style.WindowRounding = 6;
    style.ChildRounding = 4;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.TabRounding = 3;
    style.GrabRounding = 2;
    style.WindowBorderSize = style.ChildBorderSize = style.FrameBorderSize = 1;
    style.ScrollbarSize = 12;
    style.GrabMinSize = 12;
    style.ScaleAllSizes(dpiScale);
    ImGui::GetStyle() = style;
    accentStyleContext = nullptr;
    SetAccentColor(rgb);
    return true;
}

ImVec4 ToneColor(Tone tone) {
    switch (tone) {
    case Tone::Success: return Color(0xA4CEA0); // Keep in step with palette::Ready.
    case Tone::Pending: return Color(0xF1C477);
    case Tone::Error: return Color(0xFFAAA0);
    default: return AccentShade(Muted);
    }
}
void Header(const char* title, const char* subtitle, bool brand) {
    if (brand) {
        const float unit = Scale();
        auto* atlas = ImGui::GetIO().Fonts;
        ImVec2 topLeft, bottomRight;
        atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0), &topLeft, &bottomRight);
        ImGui::Image(atlas->TexID, ImVec2(30*unit,30*unit), topLeft, bottomRight);
        ImGui::SameLine();
    }
    ImGui::PushFont(HeadingFont());
    ImGui::TextWrapped("%s", title);
    ImGui::PopFont();
    if (subtitle) { ImGui::PushStyleColor(ImGuiCol_Text, AccentShade(Muted));
        ImGui::TextWrapped("%s", subtitle); ImGui::PopStyleColor(); }
    const auto p = ImGui::GetCursorScreenPos();
    const float width = (std::max)(1.f, ImGui::GetContentRegionAvail().x), s = Scale();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddLine(p, ImVec2(p.x + width, p.y), ImGui::GetColorU32(ImGuiCol_Border));
    draw->AddLine(p, ImVec2(p.x + (std::min)(72.f * s, width), p.y),
                  palette::Ember, 3.f * s);
    if (width > 100 * s) draw->AddLine(ImVec2(p.x + width - 10*s, p.y - 4*s),
        ImVec2(p.x + width - 16*s, p.y + 4*s), palette::Ember, 2*s);
    ImGui::Dummy(ImVec2(0, 8*s));
}
void Section(const char* title) { ImGui::Spacing(); ImGui::SeparatorText(title); }
void MenuHeader(const char* title, bool home) {
    const float s=Scale();const auto p=ImGui::GetCursorScreenPos();
    auto* draw=ImGui::GetWindowDrawList();auto* atlas=ImGui::GetIO().Fonts;
    ImVec2 uv0,uv1;atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0),&uv0,&uv1);
    const bool titleScreen=home&&ImGui::GetWindowWidth()>=1000*s;
    const float mark=(titleScreen?88:48)*s;
    draw->AddImage(atlas->TexID,p,ImVec2(p.x+mark,p.y+mark),uv0,uv1);
    const char* label=home?"EMBER":title;
    const float width=ImGui::GetContentRegionAvail().x-mark-16*s;
    const float font=(std::max)(18*s,(std::min)(titleScreen?68*s:home?38*s:28*s,width*HeadingFont()->FontSize/(std::max)(1.f,HeadingFont()->CalcTextSizeA(HeadingFont()->FontSize,FLT_MAX,0,label).x)));
    draw->AddText(HeadingFont(),font,ImVec2(p.x+mark+12*s,p.y+4*s),palette::Ivory,label,nullptr,width);
    const char* subtitle=loc::T("brand.subtitle");
    if(home)draw->AddText(ImGui::GetFont(),12*s,ImVec2(p.x+mark+14*s,p.y+(titleScreen?76:42)*s),palette::Ember,subtitle,nullptr,width);
    const float textHeight=home?(titleScreen?76:42)*s+ImGui::GetFont()->CalcTextSizeA(12*s,FLT_MAX,width,subtitle).y:
        HeadingFont()->CalcTextSizeA(font,FLT_MAX,width,label).y+8*s;
    const float height=(std::max)(titleScreen?102*s:home?64*s:56*s,textHeight);
    if(!home)draw->AddLine(ImVec2(p.x,p.y+height),ImVec2(p.x+ImGui::GetContentRegionAvail().x,p.y+height),AccentAlpha(70));
    ImGui::Dummy(ImVec2(0,height+8*s));
}
void Status(const char* text, Tone tone) {
    const float s = Scale();
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = (std::max)(1.f, ImGui::GetContentRegionAvail().x);
    const auto textSize = ImGui::CalcTextSize(text, nullptr, false, (std::max)(1.f, width - 16*s));
    const float height = textSize.y + 8*s;
    auto color = ToneColor(tone); color.w = .08f;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), ImGui::ColorConvertFloat4ToU32(color), 3*s);
    draw->AddRectFilled(origin, ImVec2(origin.x + 2*s, origin.y + height), ImGui::ColorConvertFloat4ToU32(ToneColor(tone)));
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 8*s, origin.y + 4*s));
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (std::max)(1.f, width - 16*s));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
}
void Text(const char* format, ...) {
    va_list args; va_start(args, format);
    ImGui::PushTextWrapPos(0);
    ImGui::TextV(format, args);
    ImGui::PopTextWrapPos();
    va_end(args);
}
void FieldLabel(const char* label) {
    if (label[0] != '#') { ImGui::AlignTextToFramePadding(); ImGui::TextWrapped("%s", label); }
    ImGui::SetNextItemWidth(-1);
}
void ConstrainNextWindow(ImVec2 preferredSize) {
    const auto* vp = ImGui::GetMainViewport();
    const float s = Scale();
    ImVec2 maxSize((std::max)(1.f, vp->WorkSize.x - 16*s), (std::max)(1.f, vp->WorkSize.y - 16*s));
    ImGui::SetNextWindowSize(ImVec2((std::min)(preferredSize.x*s, maxSize.x),
                                  (std::min)(preferredSize.y*s, maxSize.y)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2((std::min)(320*s, maxSize.x), (std::min)(220*s, maxSize.y)), maxSize);
}
void KeepWindowVisible() {
    const auto* vp = ImGui::GetMainViewport();
    auto p = ImGui::GetWindowPos(); const auto size = ImGui::GetWindowSize();
    p.x = (std::max)(vp->WorkPos.x, (std::min)(p.x, vp->WorkPos.x + vp->WorkSize.x - size.x));
    p.y = (std::max)(vp->WorkPos.y, (std::min)(p.y, vp->WorkPos.y + vp->WorkSize.y - size.y));
    ImGui::SetWindowPos(p);
}
bool BeginToolWindow(const char* name, bool* open, ImGuiWindowFlags flags) {
    const bool decorated = !(flags & ImGuiWindowFlags_NoTitleBar);
    if (decorated) ConstrainNextWindow(ImVec2(680, 500));
    bool visible = ImGui::Begin(name, open, flags);
    if (decorated) { KeepWindowVisible(); if (visible) Header(name); }
    ImGui::PushFont(DiagnosticFont());
    ImGui::PushItemWidth((std::max)(100.f * Scale(), ImGui::GetContentRegionAvail().x * .55f));
    return visible;
}
void EndToolWindow() { ImGui::PopItemWidth(); ImGui::PopFont(); ImGui::End(); }

void DrawControllerWarning(const std::string& message) {
    if(message.empty())return;
    const auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x*.5f,vp->Pos.y+12*Scale()),ImGuiCond_Always,ImVec2(.5f,0));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x*.7f,0));
    ImGui::Begin("Controller warning",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextWrapped("%s",message.c_str());ImGui::End();
}
std::string SetScoreText(const std::uint32_t (&score)[2]) {
    return std::to_string(score[0])+" - "+std::to_string(score[1]);
}
void DrawNetworkLinkGlyph(ImDrawList* draw, ImVec2 min, float size, NetworkLink link) {
    const float t=(std::max)(1.f,size*.11f);
    const auto at=[&](float x,float y){return ImVec2(min.x+size*x,min.y+size*y);};
    if(link==NetworkLink::Wireless){
        // An eighth turn (45 degrees) either side of straight up.
        const float up=-1.5707963f,spread=.7853982f;
        const ImVec2 origin=at(.5f,.86f);
        for(const float radius:{.26f,.48f,.7f}){
            draw->PathArcTo(origin,size*radius,up-spread,up+spread,12);
            draw->PathStroke(palette::Ember,0,t);
        }
        draw->AddCircleFilled(origin,t*1.2f,palette::Ember);
    }else if(link==NetworkLink::Wired){
        const ImVec2 port[]={at(.14f,.18f),at(.86f,.18f),at(.86f,.66f),at(.66f,.66f),
            at(.66f,.84f),at(.34f,.84f),at(.34f,.66f),at(.14f,.66f)};
        draw->AddPolyline(port,8,palette::Ready,ImDrawFlags_Closed,t);
        for(const float x:{.36f,.5f,.64f})draw->AddLine(at(x,.18f),at(x,.36f),palette::Ready,t);
    }else{
        draw->AddCircle(at(.5f,.5f),size*.42f,palette::Muted,0,t);
        const float textSize=size*.66f;
        const auto extent=ImGui::GetFont()->CalcTextSizeA(textSize,FLT_MAX,0,"?");
        draw->AddText(ImGui::GetFont(),textSize,ImVec2(min.x+(size-extent.x)*.5f,min.y+(size-extent.y)*.5f),palette::Muted,"?");
    }
}
const char* NetworkLinkName(NetworkLink link) {
    return loc::T(link==NetworkLink::Wired?"room.link_wired":link==NetworkLink::Wireless?"room.link_wireless":"room.link_unknown");
}
void DrawDiagnosticStrip(const DiagnosticStripView& view) {
    const auto* viewport = ImGui::GetMainViewport();
    const float scale = Scale();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f,
        viewport->WorkPos.y + viewport->WorkSize.y - 12 * scale), ImGuiCond_Always, ImVec2(.5f, 1));
    ImGui::SetNextWindowSize(ImVec2((std::min)(960 * scale, (std::max)(1.f, viewport->WorkSize.x - 24 * scale)), 0));
    ImGui::SetNextWindowBgAlpha(.9f);
    if (BeginToolWindow("GGPO", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!view.hasRemote) Status("No remote player", Tone::Neutral);
        else if (!view.networkAvailable) Status("Network statistics unavailable", Tone::Pending);
        if (ImGui::BeginTable("GGPO metrics", viewport->WorkSize.x >= 960 * scale ? 8 : 4,
                             ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg)) {
            const char* labels[] = {"RTT ms", "kbps", "recv", "send", "LFB", "RFB"};
            for (int i = 0; i < 6; ++i) {
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", labels[i]);
                if (view.networkAvailable) Text("%d", view.network[i]); else Text("N/A");
            }
            ImGui::TableNextColumn(); ImGui::TextDisabled("Pending snaps");
            if (view.pendingSnapshots >= 0) Text("%d", view.pendingSnapshots); else Text("N/A");
            ImGui::TableNextColumn(); ImGui::TextDisabled("Snapshot map"); Text("%d", view.snapshotCount);
            ImGui::EndTable();
        }
    }
    EndToolWindow();
}
} }
