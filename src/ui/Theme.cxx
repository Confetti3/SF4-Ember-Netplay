#include "Theme.hxx"
#include "EmbeddedFonts.hxx"
#include "EmbeddedBrand.hxx"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace sf4e { namespace ui {
namespace {
ImVec4 Color(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f,
                  (rgb & 255) / 255.f, alpha);
}
const unsigned Ink = 0x141312, Panel = 0x211E1B, Border = 0x443A31;
const unsigned Ivory = 0xF3EBDD, Muted = 0xB5A99B, Ember = 0xFF8738;
const float BodySize = 18.f;
}

float Scale() {
    return ImGui::GetIO().FontGlobalScale;
}
ImFont* HeadingFont() {
    const auto& fonts = ImGui::GetIO().Fonts->Fonts;
    return fonts.Size == 3 ? fonts[1] : ImGui::GetFont();
}
ImFont* DiagnosticFont() {
    const auto& fonts = ImGui::GetIO().Fonts->Fonts;
    return fonts.Size == 3 ? fonts[2] : ImGui::GetFont();
}
const char* FontLicense() { return fonts::License; }
bool ApplyTheme(float dpiScale) {
    dpiScale = (std::max)(1.f, (std::min)(dpiScale, 3.f));
    auto& io = ImGui::GetIO();
    // Font configuration survives device resets; texture ownership stays with DX9.
    if (io.Fonts->Fonts.Size == 3 &&
        std::fabs(io.Fonts->Fonts[0]->FontSize - BodySize) < .01f &&
        std::fabs(io.FontGlobalScale - dpiScale) < .001f)
        return false;
    io.FontDefault = nullptr;
    io.Fonts->Clear();
    const float sizes[] = {BodySize, 28.f, 16.f};
    for (int i = 0; i < 3; ++i) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 3; config.OversampleV = 2;
        // Keep logical sizes integral: ImGui 1.91 truncates SizePixels. Baking at
        // display density then scaling metrics preserves fractional Windows DPI.
        config.RasterizerDensity = dpiScale;
        const auto* data = i == 1 ? fonts::Heading : fonts::Body;
        const int bytes = static_cast<int>(i == 1 ? sizeof(fonts::Heading) : sizeof(fonts::Body));
        std::snprintf(config.Name, sizeof(config.Name), "%s %.0fpx",
            i == 1 ? "Inter SemiBold" : "Inter Regular", sizes[i]*dpiScale);
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), bytes, sizes[i], &config);
    }
    io.FontGlobalScale = dpiScale;
    io.FontDefault = io.Fonts->Fonts[0];
    const int markIndex = io.Fonts->AddCustomRectRegular(brand::Size, brand::Size);
    IM_ASSERT(markIndex == 0); // Clear above makes this the first custom rectangle.
    io.Fonts->Build();
    unsigned char* pixels = nullptr;
    int atlasWidth = 0, atlasHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
    const auto* mark = io.Fonts->GetCustomRectByIndex(markIndex);
    for (int row = 0; row < brand::Size; ++row)
        std::memcpy(pixels + ((mark->Y + row) * atlasWidth + mark->X) * 4,
                    brand::Pixels + row * brand::Size * 4, brand::Size * 4);
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
    auto* c = style.Colors;
    c[ImGuiCol_Text] = Color(Ivory);
    c[ImGuiCol_TextDisabled] = Color(Muted);
    c[ImGuiCol_WindowBg] = Color(Ink, .97f);
    c[ImGuiCol_ChildBg] = Color(Panel);
    c[ImGuiCol_PopupBg] = Color(Panel, .99f);
    c[ImGuiCol_Border] = Color(Border);
    c[ImGuiCol_BorderShadow] = Color(Ink, 0);
    c[ImGuiCol_FrameBg] = Color(0x191715);
    c[ImGuiCol_FrameBgHovered] = Color(0x393027);
    c[ImGuiCol_FrameBgActive] = Color(0x4A3525);
    c[ImGuiCol_TitleBg] = Color(Panel);
    c[ImGuiCol_TitleBgActive] = Color(0x3A2A1F);
    c[ImGuiCol_TitleBgCollapsed] = Color(Ink, .95f);
    c[ImGuiCol_MenuBarBg] = Color(Panel, .98f);
    c[ImGuiCol_ScrollbarBg] = Color(Ink);
    c[ImGuiCol_ScrollbarGrab] = Color(Border);
    c[ImGuiCol_ScrollbarGrabHovered] = Color(0x76583D);
    c[ImGuiCol_ScrollbarGrabActive] = Color(Ember);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = Color(Ember);
    c[ImGuiCol_SliderGrabActive] = Color(0xFFB16F);
    c[ImGuiCol_Button] = Color(0x382D24);
    c[ImGuiCol_ButtonHovered] = Color(0x59402B);
    c[ImGuiCol_ButtonActive] = Color(0x76502F);
    c[ImGuiCol_Header] = Color(0x3A2A1F);
    c[ImGuiCol_HeaderHovered] = Color(0x59402B);
    c[ImGuiCol_HeaderActive] = Color(0x76502F);
    c[ImGuiCol_Separator] = Color(Border);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = Color(Ember);
    c[ImGuiCol_ResizeGrip] = Color(Ember, .25f);
    c[ImGuiCol_ResizeGripHovered] = Color(Ember, .65f);
    c[ImGuiCol_ResizeGripActive] = Color(Ember);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = Color(Panel);
    c[ImGuiCol_TabHovered] = Color(0x59402B);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = Color(0x4A3525);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = Color(Ember);
    c[ImGuiCol_TableHeaderBg] = Color(0x382D24);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = Color(Border);
    c[ImGuiCol_TableRowBg] = Color(Ink, 0);
    c[ImGuiCol_TableRowBgAlt] = Color(0x76614F, .09f);
    c[ImGuiCol_TextSelectedBg] = Color(Ember, .3f);
    c[ImGuiCol_NavCursor] = Color(Ember);
    c[ImGuiCol_PlotLines] = Color(0xF3C58B);
    c[ImGuiCol_PlotHistogram] = Color(Ember);
    c[ImGuiCol_PlotLinesHovered] = c[ImGuiCol_PlotHistogramHovered] = Color(0xFFB16F);
    c[ImGuiCol_DragDropTarget] = Color(Ember);
    style.ScaleAllSizes(dpiScale);
    ImGui::GetStyle() = style;
    return true;
}

ImVec4 ToneColor(Tone tone) {
    switch (tone) {
    case Tone::Success: return Color(0xA4CEA0);
    case Tone::Pending: return Color(0xF1C477);
    case Tone::Error: return Color(0xFFAAA0);
    default: return Color(Muted);
    }
}
void Header(const char* title, const char* subtitle) {
    if (std::string(title) == "SF4 EMBER NETPLAY") {
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
    if (subtitle) { ImGui::PushStyleColor(ImGuiCol_Text, Color(Muted));
        ImGui::TextWrapped("%s", subtitle); ImGui::PopStyleColor(); }
    const auto p = ImGui::GetCursorScreenPos();
    const float width = (std::max)(1.f, ImGui::GetContentRegionAvail().x), s = Scale();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddLine(p, ImVec2(p.x + width, p.y), ImGui::GetColorU32(ImGuiCol_Border));
    draw->AddLine(p, ImVec2(p.x + (std::min)(72.f * s, width), p.y),
                  ImGui::ColorConvertFloat4ToU32(Color(Ember)), 3.f * s);
    if (width > 100 * s) draw->AddLine(ImVec2(p.x + width - 10*s, p.y - 4*s),
        ImVec2(p.x + width - 16*s, p.y + 4*s), ImGui::ColorConvertFloat4ToU32(Color(Ember)), 2*s);
    ImGui::Dummy(ImVec2(0, 8*s));
}
void Section(const char* title) { ImGui::Spacing(); ImGui::SeparatorText(title); }
void MenuHeader(const char* title) {
    const float s=Scale();const auto p=ImGui::GetCursorScreenPos();
    auto* draw=ImGui::GetWindowDrawList();auto* atlas=ImGui::GetIO().Fonts;
    ImVec2 uv0,uv1;atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0),&uv0,&uv1);
    const bool home=std::string(title)=="SF4 EMBER";
    const bool titleScreen=home&&ImGui::GetWindowWidth()>=1000*s;
    const float mark=(titleScreen?88:48)*s;
    draw->AddImage(atlas->TexID,p,ImVec2(p.x+mark,p.y+mark),uv0,uv1);
    const char* label=home?"EMBER":title;
    const float width=ImGui::GetContentRegionAvail().x-mark-16*s;
    const float font=(std::max)(18*s,(std::min)(titleScreen?68*s:home?38*s:28*s,width*HeadingFont()->FontSize/(std::max)(1.f,HeadingFont()->CalcTextSizeA(HeadingFont()->FontSize,FLT_MAX,0,label).x)));
    draw->AddText(HeadingFont(),font,ImVec2(p.x+mark+12*s,p.y+4*s),palette::Ivory,label,nullptr,width);
    const char* subtitle="SF4  /  PRIVATE ROOMS & ROLLBACK NETPLAY";
    if(home)draw->AddText(ImGui::GetFont(),12*s,ImVec2(p.x+mark+14*s,p.y+(titleScreen?76:42)*s),palette::Ember,subtitle,nullptr,width);
    const float textHeight=home?(titleScreen?76:42)*s+ImGui::GetFont()->CalcTextSizeA(12*s,FLT_MAX,width,subtitle).y:
        HeadingFont()->CalcTextSizeA(font,FLT_MAX,width,label).y+8*s;
    const float height=(std::max)(titleScreen?102*s:home?64*s:56*s,textHeight);
    if(!home)draw->AddLine(ImVec2(p.x,p.y+height),ImVec2(p.x+ImGui::GetContentRegionAvail().x,p.y+height),IM_COL32(255,135,56,70));
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
bool ActionButton(const char* label, ButtonKind kind, ImVec2 size) {
    int colors = 0;
    if (kind == ButtonKind::Primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, Color(Ember));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(0xFFA05F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(0xE87525));
        ImGui::PushStyleColor(ImGuiCol_Text, Color(Ink)); colors = 4;
    } else if (kind == ButtonKind::Destructive) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(Tone::Error)); colors = 1;
    }
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(colors);
    return clicked;
}
bool NavigationTab(const char* label, bool selected) {
    bool clicked = ActionButton(label, selected ? ButtonKind::Primary : ButtonKind::Secondary);
    return clicked;
}
void BeginPanel(const char* id, const char* title) {
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    Section(title);
    ImGui::PushItemWidth((std::max)(80.f * Scale(), ImGui::GetContentRegionAvail().x * .56f));
}
void EndPanel() { ImGui::PopItemWidth(); ImGui::EndChild(); }
void Metric(const char* label, const char* value) {
    ImGui::TextDisabled("%s", label);
    ImGui::TextWrapped("%s", value);
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
void DrawMatchStrip(const MatchStripView& view) {
    const auto* vp = ImGui::GetMainViewport();
    const float size = (std::max)(9.f, (std::min)(14.f, 11.f * vp->Size.y / 1080.f));
    auto* font = DiagnosticFont();
    const auto width = [&](const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; };
    const std::string middle = "   \xc2\xb7   RB " + std::to_string(view.rollbackFrames) + "f   \xc2\xb7   ";
    const float nameWidth = (std::max)(0.f, (vp->Size.x * .6f - width(middle)) * .5f);
    const auto fit = [&](std::string name) {
        if (width(name) <= nameWidth) return name;
        const std::string ellipsis = "\xe2\x80\xa6";
        while (!name.empty() && width(name + ellipsis) > nameWidth) {
            auto end = name.size() - 1;
            while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
            name.resize(end);
        }
        return width(ellipsis) <= nameWidth ? name + ellipsis : std::string{};
    };
    const std::string line = fit(view.names[0]) + middle + fit(view.names[1]);
    const ImVec2 pos(vp->Pos.x + (vp->Size.x - width(line)) * .5f, vp->Pos.y + vp->Size.y - size - 2.f);
    // Draw-list text has no window, hit target, focus or dependency on menu scale.
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0,0,0,180), line.c_str());
    draw->AddText(font, size, pos, IM_COL32(210,200,185,225), line.c_str());
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
