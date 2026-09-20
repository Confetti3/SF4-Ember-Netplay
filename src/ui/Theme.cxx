#include "Theme.hxx"
#include "EmbeddedFonts.hxx"
#include "EmbeddedBrand.hxx"
#include "../common/Localization.hxx"
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
        config.GlyphRanges = UiGlyphRanges;
        config.OversampleH = 3; config.OversampleV = 2;
        // Keep logical sizes integral: ImGui 1.91 truncates SizePixels. Baking at
        // display density then scaling metrics preserves fractional Windows DPI.
        config.RasterizerDensity = i == 2 ? (std::max)(3.5f,dpiScale) : dpiScale;
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
    case Tone::Success: return Color(0xA4CEA0); // Keep in step with palette::Ready.
    case Tone::Pending: return Color(0xF1C477);
    case Tone::Error: return Color(0xFFAAA0);
    default: return Color(Muted);
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
std::string MatchStripStateLine(const MatchStripView& view) {
    // The most urgent condition wins: a hard notice, then a stall (the game
    // is visibly frozen and the player needs to know why), then a warning.
    if(view.noticeSeverity>=2&&!view.notice.empty())return view.notice;
    if(view.predictionStalled)return loc::T("match.waiting_opponent");
    if(view.connectionWarning){
        if(view.disconnectCountdownMs>=0)
            return loc::Tf("match.connection_countdown",(view.disconnectCountdownMs+999)/1000);
        return loc::T("match.connection_unstable");
    }
    return view.notice;
}
void DrawMatchNotice(const std::string& message,int severity) {
    if(message.empty())return;
    const auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x*.5f,vp->Pos.y+vp->Size.y-24*Scale()),ImGuiCond_Always,ImVec2(.5f,1));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x*.6f,0));
    ImGui::Begin("Match notice",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing);
    const ImVec4 colors[]={ImVec4(.71f,.66f,.61f,1),ImVec4(1,.77f,.38f,1),ImVec4(1,.46f,.38f,1)};
    ImGui::PushStyleColor(ImGuiCol_Text,colors[(std::max)(0,(std::min)(2,severity))]);
    ImGui::TextWrapped("%s",message.c_str());ImGui::PopStyleColor();ImGui::End();
}
namespace {
constexpr float MatchWidth=520.f;
constexpr float MatchHeight=62.f;
constexpr float MatchStateHeight=24.f;
float MatchScale(const MatchStripView& view) {
    const float sizes[]={.85f,1.f,1.25f};
    // The readability floor belongs to the viewport term alone. Flooring the
    // product made Small and Standard identical at 720p and collapsed all three
    // at 480p, so two of the three settings did nothing.
    const float viewport=(std::max)(.8f,ImGui::GetMainViewport()->Size.y/1080.f);
    return viewport*sizes[(std::max)(0,(std::min)(2,view.size))];
}
void PaintMatchStrip(const MatchStripView& view, ImDrawList* draw, ImVec2 p, float w, float s) {
    auto* font=DiagnosticFont();
    const auto measure=[&](const std::string& t,float size){return font->CalcTextSizeA(size,FLT_MAX,0,t.c_str()).x;};
    const auto fit=[&](std::string t,float maxWidth,float size){
        if(measure(t,size)<=maxWidth)return t;
        const std::string ellipsis="...";
        while(!t.empty()&&measure(t+ellipsis,size)>maxWidth){
            auto end=t.size()-1;while(end>0&&(static_cast<unsigned char>(t[end])&0xc0)==0x80)--end;t.resize(end);
        }
        return measure(ellipsis,size)<=maxWidth?t+ellipsis:std::string();
    };
    // A neutral panel keeps the telemetry readable on bright stages, but a
    // solid block pulled the eye mid-fight: let the stage show through a
    // little. Keep the border quiet; the small "vs" is the only accent.
    draw->AddRectFilled(p,ImVec2(p.x+w,p.y+MatchHeight*s),IM_COL32(20,19,18,200),6*s);
    draw->AddRect(p,ImVec2(p.x+w,p.y+MatchHeight*s),IM_COL32(88,76,65,100),6*s);
    const auto text=[&](float x,float y,const std::string& t,float size,ImU32 color){draw->AddText(font,size,ImVec2(x,y),color,t.c_str());};
    // Link state / notice line above the panel. Same width, own background so
    // it reads against any stage; colour follows severity.
    const auto state=MatchStripStateLine(view);
    if(!state.empty()){
        const int severity=view.noticeSeverity>0||view.connectionWarning||view.predictionStalled?
            (std::max)(view.noticeSeverity,view.connectionWarning||view.predictionStalled?1:0):0;
        const ImU32 colors[]={IM_COL32(181,169,155,255),IM_COL32(255,196,96,255),IM_COL32(255,118,96,255)};
        const float h=MatchStateHeight*s;
        draw->AddRectFilled(ImVec2(p.x,p.y-h-4*s),ImVec2(p.x+w,p.y-4*s),IM_COL32(20,19,18,235),4*s);
        const auto line=fit(state,w-16*s,16*s);
        text(p.x+8*s,p.y-h-4*s+(h-16*s)*.5f,line,16*s,colors[(std::max)(0,(std::min)(2,severity))]);
    }
    // One centred "A vs B" line. Anchoring each name to a fixed "vs" made the
    // pair lopsided whenever the names differed in length.
    const auto nameWidth=(w-70*s)*.5f,gap=12*s;
    const auto left=fit(view.names[0],nameWidth,20*s),right=fit(view.names[1],nameWidth,20*s);
    const auto versus=loc::T("match.versus_short");
    const auto leftWidth=measure(left,20*s),vsWidth=measure(versus,16*s);
    auto x=p.x+(w-(leftWidth+gap+vsWidth+gap+measure(right,20*s)))*.5f;
    text(x,p.y+4*s,left,20*s,IM_COL32(243,235,221,255));x+=leftWidth+gap;
    text(x,p.y+6*s,versus,16*s,IM_COL32(255,135,56,230));x+=vsWidth+gap;
    text(x,p.y+4*s,right,20*s,IM_COL32(243,235,221,255));
    const std::string labels[]={loc::T("match.ping"),loc::T("match.rollback"),view.spectator?"":loc::T("match.delay")};
    const std::string values[]={view.pingMs<0?"\xe2\x80\x94":std::to_string(view.pingMs)+" ms",
        std::to_string(view.rollbackFrames)+"f",view.spectator?loc::T("match.spectating"):view.appliedDelay<0?"\xe2\x80\x94":std::to_string(view.appliedDelay)+"f"};
    const float column=(w-24*s)/3;
    for(int i=0;i<3;++i){
        const float x=p.x+12*s+column*i;
        // Fixed label/value anchors keep the layout stable as values change.
        text(x,p.y+33*s,labels[i],16*s,IM_COL32(181,169,155,255));
        const float valueX=x+(labels[i].empty()?0:measure(labels[i],16*s)+8*s);
        const auto value=fit(values[i],x+column-valueX-4*s,22*s);
        text(valueX,p.y+29*s,value,22*s,IM_COL32(243,235,221,255));
    }
}
}
float MatchStripScale(const MatchStripView& view) { return MatchScale(view); }
void DrawMatchStrip(const MatchStripView& view) {
    const auto* vp=ImGui::GetMainViewport();const float s=MatchScale(view);
    const float w=(std::min)(MatchWidth*s,vp->Size.x*.8f);
    const float gap=(view.raised?48.f:12.f)*(std::max)(.8f,vp->Size.y/1080.f);
    PaintMatchStrip(view,ImGui::GetForegroundDrawList(),ImVec2(vp->Pos.x+(vp->Size.x-w)*.5f,vp->Pos.y+vp->Size.y-gap-MatchHeight*s),w,s);
}
void DrawMatchStripPreview(const MatchStripView& view) {
    const auto available=ImGui::GetContentRegionAvail();
    const float s=(std::min)(MatchScale(view),(std::max)(1.f,available.x)/MatchWidth);
    const auto p=ImGui::GetCursorScreenPos();
    PaintMatchStrip(view,ImGui::GetWindowDrawList(),p,MatchWidth*s,s);
    ImGui::Dummy(ImVec2(MatchWidth*s,MatchHeight*s));
    ImGui::TextDisabled("%s",loc::Tf("match.preview_spacing",view.raised?loc::T("spacing.raised"):loc::T("spacing.normal")).c_str());
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
