#include "GameMenu.hxx"
#include "../common/Localization.hxx"
#include "Theme.hxx"
#include "SelectionArt.hxx"
#include "MenuGlyphs.hxx"
#include "MenuPresentation.hxx"
#include "MenuDialogs.hxx"
#include "../common/FighterCatalog.hxx"
#include <imgui.h>
#include <cstring>
#include <utility>

namespace sf4e { namespace ui {
namespace { MenuInput input; MenuAction forwarded; SelectionArt* menuArt=nullptr;
const char* selectGlyph="LP";const char* backGlyph="LK";PlayerCardView playerCard;
int menuDeviceType=0;
MenuTextProbe textProbe;
MenuCardProbe cardProbe;
MenuStatusProbe statusProbe;
MenuEntriesProbe entriesProbe;
PortraitProbe portraitProbe;
// A row's parts in screen space, laid out once and used both to paint the row
// and to read a click on it, so the two cannot disagree.
struct RowGeometry {
    ImVec2 label, valueText;     // text origins
    ImVec2 valueMin, valueMax;   // the value's box; its ends hold the arrows
    float arrow = 0;             // width of each arrow zone
    // -1 or +1 for a click on an arrow, 0 elsewhere. A plain value's whole box
    // is its arrows, split at the middle; a choice value's middle opens it.
    int Step(ImVec2 p,bool choices) const {
        if(p.x<valueMin.x||p.x>valueMax.x||p.y<valueMin.y||p.y>valueMax.y) return 0;
        if(choices&&p.x>=valueMin.x+arrow&&p.x<=valueMax.x-arrow) return 0;
        return p.x<(valueMin.x+valueMax.x)*.5f?-1:1;
    }
};
RowGeometry LayOutRow(ImVec2 start,float rowWidth,float height,bool stacked,float valueWidth,float unit) {
    const float pad=ImGui::GetStyle().FramePadding.x,line=ImGui::GetTextLineHeight();
    RowGeometry g;g.arrow=24*unit;
    g.label=ImVec2(start.x+pad,start.y+(stacked?6*unit:(height-line)*.5f));
    const float valueY=stacked?start.y+height-line-6*unit:g.label.y;
    const float valueX=stacked?start.x+pad:start.x+rowWidth-pad-valueWidth;
    // Stacked, the value owns only the lower line: the label above it is Select.
    g.valueMin=ImVec2(valueX,stacked?valueY-3*unit:start.y);g.valueMax=ImVec2(valueX+valueWidth,start.y+height);
    g.valueText=ImVec2(valueX,valueY);
    return g;
}
void DrawPlayerCard(ImVec2 p,float width,bool compact) {
    const float s=Scale(),height=(compact?94:142)*s;auto* d=ImGui::GetWindowDrawList();
    d->AddRectFilled(p,ImVec2(p.x+width,p.y+height),IM_COL32(17,16,15,215),3*s);
    d->AddLine(p,ImVec2(p.x+width,p.y),palette::Ember,2*s);
    const float portrait=(compact?48:66)*s;
    if(menuArt){const auto art=menuArt->PortraitFor(playerCard.fighter,portrait);if(art.texture){const float ratio=float(art.width)/art.height;
        const float w=(std::min)(portrait,portrait*ratio),h=w/ratio;
        d->AddImage(art.texture,ImVec2(p.x+12*s,p.y+12*s),ImVec2(p.x+12*s+w,p.y+12*s+h),art.uvMin,art.uvMax);}}
    // Interface strings report their full width; the player's name may elide.
    const auto text=[&](float x,float y,const std::string& label,float font,ImU32 color,const char* probe=nullptr){
        const float max=width-x-12*s;std::string shown=label;
        if(probe)ReportMenuText(probe,font,font,ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,label.c_str()).x,max);
        if(ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,label.c_str()).x>max){
            const char* end=nullptr;const float dots=ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,"...").x;
            ImGui::GetFont()->CalcTextSizeA(font,(std::max)(1.f,max-dots),0,label.c_str(),nullptr,&end);
            shown=std::string(label.c_str(),end)+"...";
        }
        d->AddText(ImGui::GetFont(),font,ImVec2(p.x+x,p.y+y),color,shown.c_str());};
    NoteUserText(playerCard.name);
    text(portrait+24*s,13*s,playerCard.name.empty()?loc::T("card.player"):playerCard.name,18*s,palette::Ivory);
    text(portrait+24*s,38*s,playerCard.fighterName.empty()?loc::T("card.choose_fighter"):playerCard.fighterName,12*s,palette::Muted,"card-fighter");
    if(!compact)text(12*s,82*s,playerCard.controllerReady?loc::T("card.controller_ready"):loc::T("card.assign_controller"),11*s,playerCard.controllerReady?palette::Ready:palette::Ember,"card-controller");
    const float y=(compact?70:116)*s;
    const auto wins=playerCard.recordAvailable?loc::Tf("card.wins",playerCard.wins):loc::T("card.no_wins");
    const auto losses=playerCard.recordAvailable?loc::Tf("card.losses",playerCard.losses):loc::T("card.no_losses");
    const auto games=playerCard.wins+playerCard.losses;
    const auto winRate=!playerCard.recordAvailable?std::string(loc::T("card.no_win_rate")):
        loc::Tf("card.win_rate",games?playerCard.wins*100/games:0);
    const float statGap=18*s;
    const auto statWidth=[&](const std::string& value){return ImGui::GetFont()->CalcTextSizeA(11*s,FLT_MAX,0,value.c_str()).x;};
    float statX=12*s;text(statX,y,wins,11*s,palette::Ivory,"card-wins");statX+=statWidth(wins)+statGap;
    text(statX,y,losses,11*s,palette::Muted,"card-losses");statX+=statWidth(losses)+statGap;
    text(statX,y,winRate,11*s,palette::Muted,"card-win-rate");
}
}
void SetMenuPlayerCard(PlayerCardView view){playerCard=std::move(view);}
void DrawInputGlyph(ImDrawList* d,const char* glyph,ImVec2 min,float size){
    const auto icon=menuArt&&!KeyCapGlyph(glyph)?menuArt->InputPrompt(PromptAsset(glyph)):SelectionImage{};
    if(icon.texture){d->AddImage(icon.texture,min,ImVec2(min.x+size,min.y+size),icon.uvMin,icon.uvMax);return;}
    // No art (a key cap, or the art not loaded yet): the glyph's name on a key cap.
    const ImVec2 capTop(min.x+size*.06f,min.y+size*.1f),capBottom(min.x+size*.94f,min.y+size*.9f);
    d->AddRectFilled(capTop,capBottom,IM_COL32(40,38,36,235),size*.15f);
    d->AddRect(capTop,capBottom,palette::Ivory,size*.15f,0,(std::max)(1.f,size*.045f));
    const float font=size*(std::strlen(glyph)>1?.34f:.5f);
    const ImVec2 text=ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,glyph);
    d->AddText(ImGui::GetFont(),font,ImVec2((capTop.x+capBottom.x-text.x)*.5f,(capTop.y+capBottom.y-text.y)*.5f),palette::Ivory,glyph);
}
float DrawCardBadge(ImVec2 p,float width,const char* text,const char* probe,bool rightAligned){
    // Shrunk to the card like its caption, so a long translation stays whole.
    const float s=Scale(),pad=4*s,natural=ImGui::GetFontSize();
    const float measured=ImGui::GetFont()->CalcTextSizeA(natural,FLT_MAX,0,text).x;
    const float font=(std::min)(natural,natural*(std::max)(1.f,width-2*pad)/(std::max)(1.f,measured));
    const float textWidth=ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,text).x;
    ReportMenuText(probe,font,natural,textWidth+2*pad,width);
    auto* d=ImGui::GetWindowDrawList();
    if(rightAligned)p.x-=textWidth+2*pad;
    d->AddRectFilled(p,ImVec2(p.x+textWidth+2*pad,p.y+natural+2*s),IM_COL32(16,15,14,230));
    d->AddText(ImGui::GetFont(),font,ImVec2(p.x+pad,p.y+(natural-font)*.5f),palette::Ember,text);
    return textWidth+2*pad;
}
RosterGrid LayOutRosterGrid(float windowWidth){
    const float s=Scale(),gap=8*s,columns=selection::RosterGridColumns,share=.8f;
    // GameMenu::Draw splits a window of 820 or more into list and detail; the list's
    // cards are what has to be wide enough, so the 15 columns ask for a bigger share.
    const float list=(windowWidth>=820*s?windowWidth*share:windowWidth)-ImGui::GetStyle().ScrollbarSize;
    const float card=(list-gap*(columns-1))/columns;
    RosterGrid grid;
    if(card>=56*s){
        // Portraits are about square; the caption strip and its padding come on top.
        grid.columns=selection::RosterGridColumns;grid.cardHeight=(std::min)(100.f,card/s*.9f+26);grid.listShare=share;
    }else{
        grid.columns=(std::max)(3,(std::min)(8,static_cast<int>(windowWidth/(170*s))));grid.cardHeight=100;grid.listShare=.53f;
    }
    return grid;
}
void SetMenuTextProbe(MenuTextProbe probe){textProbe=std::move(probe);}
void ReportMenuText(const char* id,float textHeight,float interiorHeight,float textWidth,float availableWidth){
    if(textProbe)textProbe(id,textHeight,interiorHeight,textWidth,availableWidth);
}
void SetMenuCardProbe(MenuCardProbe probe){cardProbe=std::move(probe);}
void ReportMenuCard(const char* id,ImVec2 min,ImVec2 max){if(cardProbe)cardProbe(id,min,max);}
void SetMenuStatusProbe(MenuStatusProbe probe){statusProbe=std::move(probe);}
void SetMenuEntriesProbe(MenuEntriesProbe probe){entriesProbe=std::move(probe);}
void SetPortraitProbe(PortraitProbe probe){portraitProbe=std::move(probe);}
void DrawCharacterPortrait(int fighter,ImVec2 min,ImVec2 max,ImU32 backing){
    if(portraitProbe)portraitProbe(fighter,min,max);
    auto* d=ImGui::GetWindowDrawList();
    d->AddRectFilled(min,max,backing);
    if(menuArt){const auto art=menuArt->PortraitFor(fighter,max.y-min.y);if(art.texture){
        const float factor=(std::min)((max.x-min.x)/art.width,(max.y-min.y)/art.height);
        const float w=art.width*factor,h=art.height*factor;
        d->AddImage(art.texture,ImVec2(min.x+(max.x-min.x-w)*.5f,min.y),ImVec2(min.x+(max.x-min.x+w)*.5f,min.y+h),art.uvMin,art.uvMax);return;
    }}
    const auto size=ImGui::CalcTextSize("?");
    d->AddText(ImVec2((min.x+max.x-size.x)*.5f,(min.y+max.y-size.y)*.5f),palette::Muted,"?");
}
void DrawMainPortrait(int fighter,bool saved,ImVec2 min,ImVec2 max){
    auto* d=ImGui::GetWindowDrawList();const float footer=22*Scale();
    if(menuArt){const auto art=menuArt->Portrait(fighter);if(art.texture){const float factor=(std::min)((max.x-min.x)/art.width,(max.y-min.y-footer)/art.height);
        const float w=art.width*factor,h=art.height*factor;d->AddImage(art.texture,ImVec2(min.x+(max.x-min.x-w)*.5f,min.y),ImVec2(min.x+(max.x-min.x+w)*.5f,min.y+h),art.uvMin,art.uvMax);}}
    // The name shrinks to the card, as the roster's captions do, so narrow cards keep it whole.
    const char* name=selection::FindFighter(fighter)->name;
    const float font=(std::min)(ImGui::GetFontSize(),(max.x-min.x-8*Scale())*ImGui::GetFontSize()/(std::max)(1.f,ImGui::CalcTextSize(name).x));
    d->AddRectFilled(ImVec2(min.x,max.y-footer),max,IM_COL32(16,15,14,235));
    d->AddText(ImGui::GetFont(),font,ImVec2(min.x+4*Scale(),max.y-footer+(ImGui::GetFontSize()-font)*.5f),saved?palette::Ember:palette::Ivory,name);
    if(saved)DrawCardBadge(ImVec2(min.x+4*Scale(),min.y+4*Scale()),max.x-min.x-8*Scale(),loc::T("profile.main_badge"),"main-badge");
}
// The legend follows whatever last moved the menu: a player with a pad
// assigned who types on the keyboard sees keys, and the other way round.
const char* padSelectGlyph="LP";const char* padBackGlyph="LK";
bool keyboardLast=false;unsigned previousKeys=0,previousPad=0;
void ApplyMenuGlyphs(){
    const bool pad=menuDeviceType==input::PadXInput||menuDeviceType==input::PadDirectInput;
    const bool keys=!pad||keyboardLast;
    selectGlyph=keys?"Enter":padSelectGlyph;
    backGlyph=keys?"Esc":padBackGlyph;
}
void SetMenuGlyphs(int type,unsigned select,unsigned back,const char* selectFallback,const char* backFallback){
    menuDeviceType=type;
    padSelectGlyph=PhysicalGlyph(type,select,selectFallback);
    padBackGlyph=PhysicalGlyph(type,back,backFallback);
    ApplyMenuGlyphs();
}
bool KeyboardPrompts(){ return !std::strcmp(selectGlyph,"Enter"); }
void NoteMenuDevice(unsigned padHeld){
    const unsigned keys=KeyboardMenuBits();
    bool functionKey=false;
    for(int key=ImGuiKey_F1;key<=ImGuiKey_F12;++key) functionKey|=ImGui::IsKeyPressed(static_cast<ImGuiKey>(key),false);
    if((keys&~previousKeys)||functionKey) keyboardLast=true;
    if(padHeld&~previousPad) keyboardLast=false;
    previousKeys=keys; previousPad=padHeld;
    ApplyMenuGlyphs();
}
int MenuPromptDevice(){ return KeyboardPrompts()?input::PadKeyboard:menuDeviceType; }
void PromptGlyph(const char* glyph,float size){
    const auto icon=menuArt?menuArt->InputPrompt(PromptAsset(glyph)):SelectionImage{};
    if(icon.texture) ImGui::Image(icon.texture,ImVec2(size,size),icon.uvMin,icon.uvMax);
    else ImGui::TextUnformatted(glyph);
}
void SetMenuArt(SelectionArt* art) { menuArt=art; }
SelectionArt* MenuArt() { return menuArt; }
void ForwardMenuAction(MenuAction action) { forwarded=std::move(action); }
namespace { EmbeddedReturn embeddedReturn; }
void SetEmbeddedReturn(EmbeddedReturn context) { embeddedReturn=std::move(context); }
const EmbeddedReturn& EmbeddedReturnContext() { return embeddedReturn; }
bool TakeEmbeddedFresh() { return std::exchange(embeddedReturn.fresh,false); }
MenuAction TakeForwardedMenuAction() { return std::exchange(forwarded,MenuAction{}); }
void SetMenuInput(MenuInput value) { input=value; }
unsigned KeyboardMenuBits() {
    struct Mapping { ImGuiKey key; unsigned bit; };
    static const Mapping always[]={
        {ImGuiKey_UpArrow,MenuInput::Up},{ImGuiKey_DownArrow,MenuInput::Down},{ImGuiKey_LeftArrow,MenuInput::Left},
        {ImGuiKey_RightArrow,MenuInput::Right},{ImGuiKey_Enter,MenuInput::Select},{ImGuiKey_KeypadEnter,MenuInput::Select},{ImGuiKey_Escape,MenuInput::Back}};
    // Letters, Space, Backspace and Delete belong to a text field while one
    // is being typed in.
    static const Mapping menuOnly[]={
        {ImGuiKey_Space,MenuInput::Select},{ImGuiKey_Backspace,MenuInput::Back},
        {ImGuiKey_F,MenuInput::Fighter},{ImGuiKey_T,MenuInput::Options},{ImGuiKey_C,MenuInput::Chat},{ImGuiKey_Delete,MenuInput::Leave}};
    unsigned bits=0;
    for(const auto& mapping:always) if(ImGui::IsKeyDown(mapping.key)) bits|=mapping.bit;
    if(!ImGui::GetIO().WantTextInput)
        for(const auto& mapping:menuOnly) if(ImGui::IsKeyDown(mapping.key)) bits|=mapping.bit;
    return bits;
}
MenuInput ReadMenuInput() {
    auto value=input; value.time=ImGui::GetTime();
    const unsigned keys=KeyboardMenuBits();
    // Typing counts as using the keyboard, though its keys press no menu bit.
    const auto& io=ImGui::GetIO();
    const bool typed=!io.InputQueueCharacters.empty()||ImGui::IsKeyPressed(ImGuiKey_Backspace,false)||ImGui::IsKeyPressed(ImGuiKey_Delete,false);
    if((keys&~previousKeys)||typed) keyboardLast=true;
    if(input.held&~previousPad) keyboardLast=false;
    previousKeys=keys; previousPad=input.held;
    ApplyMenuGlyphs();
    value.held|=keys; value.keyboard=keys;
    value.acceptText=ImGui::IsKeyPressed(ImGuiKey_Enter,false);
    return value;
}
void GameMenu::DrawHomeStatusLine(const std::vector<MenuEntry>& entries,const char* status,Tone statusTone,float homeMargin) {
    ImGui::SetCursorPosX(homeMargin);
    const auto current=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.Focus();});
    if(current!=entries.end()){
        // The focused row's help and the status each keep a line of their
        // own, so a save or an error never hides what the row does. Each is
        // clipped to its line, so neither can move the list.
        const float s=Scale(),width=ImGui::GetContentRegionAvail().x,line=HomeStatusHeight*s*.5f;
        const auto p=ImGui::GetCursorScreenPos();auto* d=ImGui::GetWindowDrawList();
        const auto clipped=[&](float y,ImU32 color,const char* text){
            d->PushClipRect(ImVec2(p.x,p.y+y),ImVec2(p.x+width,p.y+y+line),true);
            d->AddText(ImGui::GetFont(),12*s,ImVec2(p.x,p.y+y),color,text,nullptr,width);
            d->PopClipRect();};
        clipped(0,palette::Muted,current->detail.c_str());
        if(*status)clipped(line,statusTone!=Tone::Neutral?ImGui::ColorConvertFloat4ToU32(ToneColor(statusTone)):palette::Ivory,status);
    }
    ImGui::Dummy(ImVec2(0,HomeStatusHeight*Scale()));ImGui::SetCursorPosX(homeMargin);
}
MenuAction GameMenu::Draw(const char* title,const std::vector<MenuEntry>& entries,const char* status,const Detail& detail,int columns,const Card& card,const Body& body,float flyoutScale,float cardHeight,bool stableStatus,Tone statusTone,bool home) {
    const bool flyout=flyoutScale>0;
    const float unit=flyout?flyoutScale:Scale();
    columns=(std::max)(1,columns);
    const double now=clock_.Update(ImGui::GetTime());
    // A newly visible component cannot reuse its parent's opening press.
    if(lastFrame_!=ImGui::GetFrameCount()-1) { navigation.NeutralGate(); lastEdit_.clear(); }
    lastFrame_=ImGui::GetFrameCount();
    const bool modalAtStart=navigation.Editing()||navigation.Asking();
    // A choice belongs to the body that draws it, so the body stays live for it.
    const bool choosingAtStart=navigation.Choosing();
    auto menuInput=ReadMenuInput(); menuInput.time=now;
    // A notice owns the input until dismissed, so the dismissing press can
    // neither activate the focused row nor leak into the list behind it.
    const bool noticeOpen=!notice_.empty();
    if(noticeOpen) {
        const unsigned pressed=menuInput.held&~noticePrevious_; noticePrevious_=menuInput.held;
        if(!noticeAlternative_.empty()) {
            if(pressed&(MenuInput::Up|MenuInput::Left)) noticeAlternativeSelected_=false;
            if(pressed&(MenuInput::Down|MenuInput::Right)) noticeAlternativeSelected_=true;
        }
        // Back always declines: only Select on the alternative chooses it.
        if(pressed&(MenuInput::Select|MenuInput::Back))
            DismissNotice(noticeAlternativeSelected_&&!(pressed&MenuInput::Back));
        menuInput.held=0; menuInput.acceptText=false;
    } else noticePrevious_=~0u;
    held_=menuInput.held;
    // InputText consumes this frame's characters before the requested acceptance.
    // The navigation model still owns the neutral gate and Back/Enter ordering.
    auto action=navigation.Update(menuInput,entries,columns,true,true);
    const bool acceptEditText=action.kind==MenuAction::SubmitText;
    if(acceptEditText)action={};
    feedback_.Update(navigation.Screen(),entries,now);
    // Settled after the body: a board may take Back for itself.
    bool backRequested=false;
    if(statusProbe)statusProbe(status,statusTone);
    if(entriesProbe)entriesProbe(entries);
    const bool changed=lastScreen_!=navigation.Screen();
    // Scaled like every other breakpoint, so a 1.5x interface on a 1024-wide
    // window takes the same layout decision as the header and the galleries.
    const auto windowSize=ImGui::GetWindowSize();const bool roomy=windowSize.x>=1000*Scale()&&windowSize.x/windowSize.y>=1.5f;
    const float homeMargin=roomy?windowSize.x*.10f:20*Scale();
    float homeTop=roomy?windowSize.y*.10f:16*Scale();
    float homeLift=0;
    if(home&&roomy&&!entries.empty()) {
        const float rows=entries.size()*(std::max)(44*Scale(),28*Scale()+2*ImGui::GetStyle().FramePadding.y)+(entries.size()-1)*ImGui::GetStyle().ItemSpacing.y;
        const float footer=MenuLegend(windowSize.x-homeMargin-ImGui::GetStyle().WindowPadding.x,selectGlyph,backGlyph,false,false,menuArt,unit,loc::T("menu.select"),{},backHint.c_str())+
            8*unit+3*ImGui::GetStyle().ItemSpacing.y+HomeStatusHeight*unit+ImGui::GetStyle().WindowPadding.y;
        homeLift=(std::min)((std::max)(0.f,homeTop-16*Scale()),(std::max)(0.f,rows+footer-windowSize.y*.61f));
        homeTop-=homeLift;
    }
    if(menuArt&&!flyout) {
        const auto art=menuArt->MenuBackdrop();const auto p=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
        if(art.texture){auto* d=ImGui::GetWindowDrawList();
            ImVec2 uv0(0,0),uv1(1,1);
            const float sourceAspect=float(art.width)/art.height,targetAspect=size.x/size.y;
            if(targetAspect<sourceAspect){const float span=targetAspect/sourceAspect;uv0.x=1-span;}
            else {const float span=sourceAspect/targetAspect;uv0.y=(1-span)*.5f;uv1.y=1-uv0.y;}
            d->AddImage(art.texture,p,ImVec2(p.x+size.x,p.y+size.y),uv0,uv1);
            d->AddRectFilledMultiColor(p,ImVec2(p.x+size.x,p.y+size.y),IM_COL32(16,15,14,220),IM_COL32(16,15,14,75),IM_COL32(16,15,14,110),IM_COL32(16,15,14,235));}
    }
    if(home)ImGui::SetCursorPos(ImVec2(homeMargin,homeTop));
    if(flyout) {
        ImGui::TextColored(ToneColor(Tone::Pending),"%s",loc::Tf("menu.flyout_title",title).c_str());
        ImGui::Separator();
    }
    const auto headerTop=ImGui::GetCursorScreenPos();
    if(!flyout) MenuHeader(title,home);
    // An open dialog owns the header and the legend: Back cancels it, and
    // Select names the button it highlights.
    std::string dialogSelect,dialogBack;
    const bool dialog=DialogLegend(entries,dialogSelect,dialogBack);
    // Home's header button leaves Ember, so it is offered only with a place to name.
    if(!home||!exitName.empty()) {
        // A roomy Home has space under its title; a compact one, where the
        // player card follows the title, puts the button at the title's right.
        const auto afterHeader=ImGui::GetCursorScreenPos();
        if(home&&roomy)ImGui::SetCursorPosX(homeMargin);
        const bool root=navigation.Screen()==navigation.Root();
        const std::string parent=navigation.Parent()==navigation.Root()&&!rootName.empty()?rootName:MenuScreenLabel(navigation.Parent());
        const std::string backLabel=dialog?dialogBack:!root?loc::Tf("menu.back_to",parent):!exitName.empty()?loc::Tf("menu.back_to",exitName):
            !backHint.empty()?backHint:loc::Tf("menu.back_to",parent);
        const float room=home?windowSize.x-2*homeMargin:ImGui::GetContentRegionAvail().x;
        if(home&&!roomy)ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x+windowSize.x-homeMargin-
            ImGui::CalcTextSize(backLabel.c_str()).x-2*ImGui::GetStyle().FramePadding.x,headerTop.y));
        if(ImGui::Button((backLabel+"###MenuBack").c_str())) {
            // A modal dialog blocks this button; a choice or the flyout's
            // confirmation leaves it live, so it cancels them.
            if(!notice_.empty()) DismissNotice(false);
            else if(dialog) navigation.Cancel();
            else backRequested=true;
        }
        ReportMenuCard("menu-back",ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
        const auto measured=ImGui::CalcTextSize(backLabel.c_str());
        ReportMenuText("menu-back",measured.y,measured.y,measured.x,room);
        if(home&&!roomy)ImGui::SetCursorScreenPos(afterHeader);
    }
    if(home){
        const float cardWidth=roomy?(std::min)(360*Scale(),windowSize.x*.35f):windowSize.x-2*homeMargin;
        const auto origin=ImGui::GetWindowPos();
        DrawPlayerCard(ImVec2(origin.x+(roomy?windowSize.x-homeMargin-cardWidth:homeMargin),origin.y+(roomy?homeTop:ImGui::GetCursorPosY())),cardWidth,!roomy);
        const float start=roomy?(std::max)(ImGui::GetCursorPosY()+36*Scale(),windowSize.y*.39f-homeLift):ImGui::GetCursorPosY()+110*Scale();
        ImGui::SetCursorPos(ImVec2(homeMargin,start));
    }
    // The legend and the list are measured before the status, so a long status
    // can be given exactly what they leave.
    const auto mouseDelta=ImGui::GetIO().MouseDelta;const bool pointerMoved=mouseDelta.x!=0||mouseDelta.y!=0;
    const auto focusedEntry=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.Focus();});
    MenuEntry presentationEntry;
    if(focusedEntry!=entries.end()) { presentationEntry=*focusedEntry; presentationEntry.enabled=feedback_.Enabled(*focusedEntry); }
    const bool adjustable=!dialog&&focusedEntry!=entries.end()&&presentationEntry.adjustable&&presentationEntry.enabled;
    const char* primary=dialog?(dialogSelect.empty()?nullptr:dialogSelect.c_str()):MenuPrimaryHint(focusedEntry==entries.end()?nullptr:&presentationEntry);
    const bool wide=ImGui::GetContentRegionAvail().x>=(flyout?640:820)*unit;
    // Reserve the actual footer items and their spacing, not a guessed margin.
    const float footerSpacing=8*unit+2*ImGui::GetStyle().ItemSpacing.y+
        (home?HomeStatusHeight*unit+ImGui::GetStyle().ItemSpacing.y:0);
    // Shortcut hints ride along only on an Xbox pad or the keyboard, which
    // have the buttons, and only when they fit
    // without another legend row; a narrow window keeps the standard legend.
    static const std::vector<LegendHint> noHints;
    const float legendWidth=ImGui::GetContentRegionAvail().x;
    // An open dialog takes the screen's shortcuts away.
    const char* back=dialog?dialogBack.c_str():backHint.c_str();
    const float standardLegend=MenuLegend(legendWidth,selectGlyph,backGlyph,false,adjustable,menuArt,unit,primary,noHints,back);
    const bool hintsFit=!dialog&&(!std::strcmp(selectGlyph,"A")||KeyboardPrompts())&&!shortcutHints.empty()&&
        MenuLegend(legendWidth,selectGlyph,backGlyph,false,adjustable,menuArt,unit,primary,shortcutHints,back)<=standardLegend;
    const auto& extras=hintsFit?shortcutHints:noHints;
    // The footer note goes in the bottom right corner, level with the legend's
    // last row when it fits beside it. Otherwise it takes a line of its own
    // under the legend while the list keeps its least height and a row more,
    // and else it is cut to the room beside the legend.
    const float noteFont=12*unit,noteGap=24*unit;
    std::string note=flyout?std::string():footerNote;
    float legendEnd=0;
    if(!note.empty())MenuLegend(legendWidth,selectGlyph,backGlyph,false,adjustable,menuArt,unit,primary,extras,back,&legendEnd);
    const float noteWidth=note.empty()?0.f:ImGui::GetFont()->CalcTextSizeA(noteFont,FLT_MAX,0,note.c_str()).x;
    const float besideRoom=legendWidth-legendEnd-noteGap;
    const float lineCost=noteFont+6*unit+ImGui::GetStyle().ItemSpacing.y;
    const bool noteOwnLine=noteWidth>besideRoom&&noteWidth<=legendWidth&&
        ImGui::GetContentRegionAvail().y-standardLegend-footerSpacing-lineCost>=60+44*unit;
    if(!note.empty()&&noteWidth>besideRoom&&!noteOwnLine) {
        if(besideRoom<90*unit)note.clear();
        else {
            const char* end=nullptr;auto* font=ImGui::GetFont();
            font->CalcTextSizeA(noteFont,besideRoom-font->CalcTextSizeA(noteFont,FLT_MAX,0,"...").x-1,0,note.c_str(),nullptr,&end);
            note=std::string(note.c_str(),end)+"...";
        }
    }
    const float footer=standardLegend+footerSpacing+(noteOwnLine?lineCost:0.f);
    if(flyout || stableStatus) {
        // Bound long command errors without displacing the list or its legend.
        // The room header shares its Back row on wide windows; this decision
        // depends only on width, so changing feedback cannot move the controls.
        const bool fitFeedback=fitStatus && stableStatus && !flyout;
        const bool inlineFeedback=stableStatus && !flyout && !fitFeedback && ImGui::GetContentRegionAvail().x>=820*unit;
        if(inlineFeedback) ImGui::SameLine();
        // Two reserved lines cost more than a short viewport can spare, and the
        // appearance galleries collapse their artwork to pay for it. Both the
        // width and the height tests read geometry only, never the status text,
        // so the reserved area still cannot move under a highlight or a click.
        const float feedbackLines=ImGui::GetContentRegionAvail().y>=520*unit?2.f:1.f;
        float feedbackHeight=inlineFeedback?ImGui::GetFrameHeight():ImGui::GetTextLineHeightWithSpacing()*feedbackLines;
        // The wrap width leaves room for the scrollbar, so a message that fits
        // never grows one and the measured height is the drawn height.
        const float wrapWidth=ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ScrollbarSize;
        if(fitFeedback){
            const float text=ImGui::CalcTextSize(status,nullptr,false,wrapWidth).y;
            // The message may take what the legend and a short list leave (the
            // list is at least 60 tall, and beside a detail pane less than half
            // the height it shares with it); past that it scrolls, and the
            // footer stays on screen.
            const float listMinimum=wide?60.f:(60.f+ImGui::GetStyle().ItemSpacing.y)/.52f;
            const float cap=(std::max)(2*ImGui::GetTextLineHeightWithSpacing(),ImGui::GetContentRegionAvail().y-footer-listMinimum);
            feedbackHeight=(std::min)(cap,(std::max)(ImGui::GetTextLineHeightWithSpacing(),text+ImGui::GetStyle().ItemSpacing.y));
            ReportMenuText("command-feedback",text,feedbackHeight,0,0);
        }
        ImGui::BeginChild("Command feedback",ImVec2(0,feedbackHeight));
        ImGui::PushStyleColor(ImGuiCol_Text,ToneColor(statusTone));
        // One line beside the Back button sits in the middle of its band, with a margin from the band's edge.
        if(inlineFeedback){
            ImGui::SetCursorPos(ImVec2(8*unit,(std::max)(0.f,(feedbackHeight-ImGui::CalcTextSize(status,nullptr,false,wrapWidth).y)*.5f)));
        }
        if(fitFeedback){ImGui::PushTextWrapPos(wrapWidth);ImGui::TextUnformatted(status);ImGui::PopTextWrapPos();}
        else ImGui::TextWrapped("%s",status);
        ImGui::PopStyleColor();ImGui::EndChild();
    }
    const float bodyTop=ImGui::GetCursorScreenPos().y;
    const auto available=ImGui::GetContentRegionAvail();
    const bool compactGallery=cardHeight>100&&!wide;
    // A list that wants the room: its detail pane keeps to a few lines of the row's own detail.
    const bool leanDetail=compactDetailLines>0&&!wide&&!home&&!flyout;
    auto preview=[&] {
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.Focus();});
        if(it!=entries.end()) {
            const bool visualEnabled=feedback_.Enabled(*it);
            ImGui::PushStyleColor(ImGuiCol_Text,ToneColor(visualEnabled?Tone::Neutral:Tone::Pending));
            // A compact gallery's card already carries its label, so its two
            // lines go to what the label does not say: the explanation, then
            // what is saved.
            if(!compactGallery&&!leanDetail){if(it->userText)NoteUserText(it->label);ImGui::TextWrapped("%s",it->label.c_str());}
            ImGui::PopStyleColor();
            if(!it->detail.empty()){NoteDetailText(it->detail,it->detailText);ImGui::TextWrapped("%s",it->detail.c_str());}
            if(!it->value.empty()){if(it->userText||it->text)NoteUserText(it->value);ImGui::TextWrapped("%s",it->value.c_str());}
            if(!visualEnabled&&!it->quiet) ImGui::TextDisabled("%s",loc::T(feedback_.Pending(*it)?"room.updating":"common.unavailable"));
            if(detail) detail(it->id);
        }
    };
    if(body){
        ImGui::BeginDisabled((modalAtStart&&!choosingAtStart)||navigation.Editing()||navigation.Confirming());
        body(entries,navigation,action,(std::max)(60.f,available.y-footer),feedback_);
        ImGui::EndDisabled();
    }
    else if(navigation.Reading()) DrawReader(entries,detail,(std::max)(60.f,available.y-footer),menuInput.held);
    else {
    if(!wide&&!home) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.1f,.09f,.08f,.52f));
        ImGui::BeginChild("Menu detail",ImVec2(0,compactGallery?2*ImGui::GetTextLineHeightWithSpacing()+4*unit:
            leanDetail?(std::min)(compactDetailLines*ImGui::GetTextLineHeightWithSpacing()+4*unit,(available.y-footer)*.48f):
            (std::min)((available.y-footer)*(flyout?.4f:.48f),(flyout?110:145)*unit)));
        preview(); ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    const float listWidth=home?(roomy?windowSize.x*.40f:windowSize.x-2*homeMargin):wide?available.x*wideListShare:available.x;
    if(changed) ImGui::SetNextWindowScroll(ImVec2(0,navigation.Scroll()));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
    ImGui::BeginChild("Menu list",ImVec2(listWidth,(std::max)(60.f,ImGui::GetContentRegionAvail().y-footer)),0,ImGuiWindowFlags_NoNavInputs);
    if(home&&entries.size()*(44*Scale()+ImGui::GetStyle().ItemSpacing.y)-ImGui::GetStyle().ItemSpacing.y>ImGui::GetContentRegionAvail().y)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(ImGui::GetStyle().FramePadding.x,2*Scale()));
    else if(home)ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImGui::GetStyle().FramePadding);
    const float homeHeight=home&&!entries.empty()?(std::max)(28*Scale()+2*ImGui::GetStyle().FramePadding.y,
        (std::min)(44*Scale(),(ImGui::GetContentRegionAvail().y-(entries.size()-1)*ImGui::GetStyle().ItemSpacing.y)/entries.size())):0;
    const float gap=8*Scale();
    const float fullWidth=ImGui::GetContentRegionAvail().x;
    const float cellWidth=(fullWidth-gap*(columns-1))/columns;
    const std::size_t cells=MenuGridCells(entries);
    // Tall appearance cards must fit as a whole in the focused scrolling pane,
    // including their saved marker and caption, even at narrow/high DPI sizes.
    const float gridHeight=cardHeight>100?(std::min)(cardHeight*Scale(),ImGui::GetContentRegionAvail().y):cardHeight*(flyout?unit:Scale());
    for(std::size_t i=0;i<entries.size();++i) {
        const auto& e=entries[i]; const bool focused=e.id==navigation.Focus();
        const bool visualEnabled=feedback_.Enabled(e);
        const bool gridCell=columns>1&&i<cells;
        const float rowWidth=gridCell?cellWidth:fullWidth;
        if(gridCell&&i%columns) ImGui::SameLine(0,gap);
        ImGui::PushID(e.id.c_str());
        const auto start=ImGui::GetCursorScreenPos();
        const float textSize=home?28*Scale():ImGui::GetFontSize();
        const bool valueRow=!card&&(e.adjustable||e.text||!e.value.empty());
        // A match's row keeps a portrait square at each end; its text sits between them.
        const bool match=!gridCell&&!home&&e.Match();
        const float faceInset=6*unit,faceGap=8*unit;
        const float stackedFace=match?48*unit:0,faceEnd=match?faceInset+stackedFace+faceGap:0;
        const bool stackedValue=valueRow&&rowWidth-2*faceEnd<420*unit;
        const float height=gridCell?gridHeight:e.height>0?e.height*unit:(std::max)(home?homeHeight:(stackedValue?64:flyout?42:52)*unit,textSize+2*ImGui::GetStyle().FramePadding.y);
        const float face=match?(std::min)(stackedFace,height-8*unit):0,lead=match?faceInset+face+faceGap:0;
        const float innerWidth=rowWidth-2*lead;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,card?cardRounding*unit:0);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,ImVec2(.03f,.5f));
        ImGui::PushStyleColor(ImGuiCol_Button,focused?(card&&cardRounding>0?ImVec4(.30f,.17f,.09f,.55f):ImVec4(.47f,.28f,.16f,.6f)):ImVec4(.105f,.10f,.095f,home?0.f:.45f));
        ImGui::PushStyleColor(ImGuiCol_Text,EntryTextColor(visualEnabled));
        if(home)ImGui::PushFont(HeadingFont());
        const float textWidth=innerWidth-2*ImGui::GetStyle().FramePadding.x;
        const float valueWidth=valueRow?(stackedValue?textWidth:(std::min)(textWidth*.45f,200*unit)):0;
        const float labelWidth=valueRow&&!stackedValue?textWidth-valueWidth-16*unit:textWidth;
        const std::string label=FitLabel(e.label,labelWidth);
        const std::string value=FitLabel(e.value.empty()?loc::T("common.not_set"):e.value,(std::max)(1.f,valueWidth-(e.adjustable?52*unit:0)));
        const auto geometry=LayOutRow(ImVec2(start.x+lead,start.y),innerWidth,height,stackedValue,valueWidth,unit);
        if(ImGui::Button("##entry",ImVec2(rowWidth,height))&&!modalAtStart&&!navigation.Editing()&&!navigation.Asking()) {
            // One intent per click, settled before anything changes: a value's
            // arrow adjusts it; anything else on the row is Select.
            navigation.Focus(e.id,entries);
            const int step=e.adjustable&&e.enabled?geometry.Step(ImGui::GetMousePos(),!e.choices.empty()):0;
            action=step?MenuAction{MenuAction::Adjust,e.id,{},step}:navigation.Choose(entries);
        }
        // A moving pointer selects what it is over, as on the room board; a
        // resting one never takes the selection back from the pad or keys.
        // It does not scroll either: lastFocus_ takes the new focus below.
        else if(pointerMoved&&!dialog&&ImGui::IsItemHovered()) navigation.Focus(e.id,entries);
        // Player text draws glyphs only while its row is inside the list's clip,
        // so a long roster or chat that has scrolled away stops holding them.
        const bool rowVisible=ImGui::IsItemVisible();
        const bool drawnCard=card&&card(e,start,ImVec2(start.x+rowWidth,start.y+height));
        if(!drawnCard){
            if(rowVisible&&(e.userText||e.text)){if(e.userText)NoteUserText(e.label);NoteUserText(e.value);}
            ImGui::GetWindowDrawList()->AddText(geometry.label,ImGui::GetColorU32(ImGuiCol_Text),label.c_str());
            if(valueRow){
                const float y=geometry.valueText.y,textWidth=ImGui::CalcTextSize(value.c_str()).x;
                const auto color=visualEnabled?palette::Ivory:palette::Muted;
                const float x=e.adjustable?geometry.valueMin.x+(valueWidth-textWidth)*.5f:geometry.valueMax.x-textWidth;
                ImGui::GetWindowDrawList()->AddText(ImVec2(x,y),color,value.c_str());
                if(e.adjustable){
                    ImGui::GetWindowDrawList()->AddText(ImVec2(geometry.valueMin.x,y),visualEnabled?palette::Ember:palette::Muted,"<");
                    ImGui::GetWindowDrawList()->AddText(ImVec2(geometry.valueMax.x-12*unit,y),visualEnabled?palette::Ember:palette::Muted,">");
                }
            }
            if(match&&rowVisible){
                const float top=start.y+(height-face)*.5f;
                DrawCharacterPortrait(e.fighter1,ImVec2(start.x+faceInset,top),ImVec2(start.x+faceInset+face,top+face));
                DrawCharacterPortrait(e.fighter2,ImVec2(start.x+rowWidth-faceInset-face,top),ImVec2(start.x+rowWidth-faceInset,top+face));
            }
        }
        // Row text is drawn through the draw list, so it is not hoverable and an
        // ellipsis would otherwise be unrecoverable. The row button carries it.
        if(ImGui::IsItemHovered()) {
            std::string full;
            if(label!=e.label) full=e.label;
            if(valueRow&&!e.value.empty()&&value!=e.value) full=full.empty()?e.value:full+"\n"+e.value;
            if(!full.empty()) ImGui::SetTooltip("%s",full.c_str());
        }
        // The probe measures the full label, so an ellipsis cannot pass for one
        // that fits. A player's own words as a label (a name) may elide, as
        // values may; the hover above and the detail pane carry them whole.
        if(textProbe&&!drawnCard){const auto measured=ImGui::CalcTextSize((e.userText?label:e.label).c_str());textProbe(e.id.c_str(),measured.y,height-2*ImGui::GetStyle().FramePadding.y,measured.x,labelWidth);
            if(valueRow)textProbe((e.id+"-value").c_str(),ImGui::GetTextLineHeight(),height-2*ImGui::GetStyle().FramePadding.y,ImGui::CalcTextSize(value.c_str()).x,valueWidth-(e.adjustable?52*unit:0));}
        if(home)ImGui::PopFont();ImGui::PopStyleColor(2);ImGui::PopStyleVar(3);
        if(cardProbe)cardProbe(e.id.c_str(),start,ImVec2(start.x+rowWidth,start.y+height));
        if(focused) {
            // A card with rounded corners draws its own outline; the bar would stick out of them.
            if(gridCell)ImGui::GetWindowDrawList()->AddRect(start,ImVec2(start.x+rowWidth,start.y+height),palette::Ember,card?cardRounding*unit:0,0,2*Scale());
            else if(!(drawnCard&&cardRounding>0))ImGui::GetWindowDrawList()->AddRectFilled(start,ImVec2(start.x+3*Scale(),start.y+height),palette::Ember);
            if(lastFocus_!=e.id||changed) ImGui::SetScrollHereY(.5f);
        }
        ImGui::PopID();
    }
    if(home)ImGui::PopStyleVar();
    navigation.Scroll()=ImGui::GetScrollY(); ImGui::EndChild();ImGui::PopStyleColor();
    if(wide&&!home) { ImGui::SameLine(0,20*unit); ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.1f,.09f,.08f,.52f));
        ImGui::BeginChild("Menu detail",ImVec2(0,(std::max)(60.f,available.y-footer))); preview(); ImGui::EndChild(); ImGui::PopStyleColor(); }
    }
    if(action.kind==MenuAction::Back){backRequested=true;action={};}
    if(!flyout){
        const auto p=ImGui::GetCursorScreenPos();const auto origin=ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(origin.x,p.y),ImVec2(origin.x+windowSize.x,origin.y+windowSize.y),IM_COL32(16,15,14,225));
    }
    if(home){
        DrawHomeStatusLine(entries,status,statusTone,homeMargin);
    }
    const float legendTop=ImGui::GetCursorScreenPos().y;
    ImGui::Dummy(ImVec2(0,8*unit));if(home)ImGui::SetCursorPosX(homeMargin);
    const auto legendStart=ImGui::GetCursorScreenPos();
    const float legendHeight=MenuLegend(ImGui::GetContentRegionAvail().x,selectGlyph,backGlyph,true,adjustable,menuArt,unit,primary,extras,back);
    ImGui::Dummy(ImVec2(0,legendHeight));
    if(!note.empty()) {
        // Right-aligned to the legend's own right edge.
        const float right=legendStart.x+legendWidth;
        const float width=ImGui::GetFont()->CalcTextSizeA(noteFont,FLT_MAX,0,note.c_str()).x;
        const float y=noteOwnLine?ImGui::GetCursorScreenPos().y:legendStart.y+legendHeight-19*unit-noteFont*.5f;
        const ImVec2 at((std::max)(legendStart.x,right-width),y);
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),noteFont,at,palette::Muted,note.c_str());
        ReportMenuCard(note==footerNote?"footer-note":"footer-note/cut",at,ImVec2(at.x+width,at.y+noteFont));
        if(noteOwnLine)ImGui::Dummy(ImVec2(0,noteFont+6*unit));
    }
    if(flyout) {
        // The flyout has no choice dialog, so a row that opens one does nothing.
        // Its text rows edit in the same popup as the shell's.
        if(navigation.Choosing()) navigation.Cancel();
        DrawFlyoutConfirmation(entries,unit,bodyTop,legendTop,action);
        DrawEditModal(entries,acceptEditText,action);
        lastEdit_=navigation.EditingId();
        lastFocus_=navigation.Focus();lastScreen_=navigation.Screen();
        if(backRequested) return navigation.Return();
        return action;
    }
    // A renderer popup may survive a model cancellation by one frame. Close it
    // without rendering stale controls or reading a draft that was already cleared.
    DrawConfirmationModal(entries,action);
    if(!body)DrawChoiceModal(entries,action);
    DrawNoticeModal(noticeOpen);
    DrawEditModal(entries,acceptEditText,action);
    // The editor takes the keyboard focus when it opens, so a notice that hid it
    // hands the focus back the same way when it returns.
    lastEdit_=notice_.empty()?navigation.EditingId():std::string();
    lastFocus_=navigation.Focus(); lastScreen_=navigation.Screen();
    if(backRequested) return navigation.Return();
    return action;
}
} }
