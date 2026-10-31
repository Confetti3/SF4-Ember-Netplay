#include "GameMenu.hxx"
#include "V2.hxx"
#include "ControllerNavigation.hxx"
#include "RowId.hxx"
#include "../common/Localization.hxx"
#include "Theme.hxx"
#include "SelectionArt.hxx"
#include "MenuGlyphs.hxx"
#include "MenuPresentation.hxx"
#include "MenuDialogs.hxx"
#include "../common/FighterCatalog.hxx"
#include "../platform/Speech.hxx"
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace sf4e { namespace ui {
namespace { MenuInput input; MenuAction forwarded; SelectionArt* menuArt=nullptr;
const char* selectGlyph="LK";const char* backGlyph="MK";PlayerCardView playerCard;std::vector<MenuNewsItem> news;
int menuDeviceType=0;
// The pad's menu buttons: Select and Back, which are no shortcuts, and a
// stick's LP and HP buttons, which are its tab shortcuts (MenuShortcutGlyph).
input::MenuButtons menuButtons;
gamepads::PromptBrand promptBrand;
gamepads::FightNames promptNames=gamepads::NamesOf(gamepads::DefaultLayout);
MenuTextProbe textProbe;
MenuCardProbe cardProbe;
MenuCardTextProbe cardTextProbe;
MenuStatusProbe statusProbe;
MenuEntriesProbe entriesProbe;
PortraitProbe portraitProbe;
}
void SetMenuPlayerCard(PlayerCardView view){playerCard=std::move(view);}
void SetMenuNews(std::vector<MenuNewsItem> items){news=std::move(items);}
const std::vector<MenuNewsItem>& MenuNews(){return news;}
const PlayerCardView& MenuPlayerCard(){return playerCard;}
const char* MenuSelectGlyph(){return selectGlyph;}
const char* MenuBackGlyph(){return backGlyph;}
bool DrawMenuBackdrop(){
    const SelectionImage art=menuArt?menuArt->MenuBackdrop():SelectionImage{};
    if(!art.texture)return false;
    // The whole screen, also around a window kept inside TV mode's safe area.
    const auto* vp=ImGui::GetMainViewport();const auto p=vp->Pos,size=vp->Size,end=ImVec2(p.x+size.x,p.y+size.y);auto* d=ImGui::GetWindowDrawList();
    d->PushClipRect(p,end);
    ImVec2 uv0(0,0),uv1(1,1);
    const float sourceAspect=float(art.width)/art.height,targetAspect=size.x/size.y;
    if(targetAspect<sourceAspect){const float span=targetAspect/sourceAspect;uv0.x=1-span;}
    else {const float span=sourceAspect/targetAspect;uv0.y=(1-span)*.5f;uv1.y=1-uv0.y;}
    d->AddImage(art.texture,p,end,uv0,uv1);
    // The website's left-heavy tint, so the cards on the left read and the art shows on the right.
    d->AddRectFilledMultiColor(p,end,IM_COL32(20,19,18,245),IM_COL32(20,19,18,128),IM_COL32(20,19,18,160),IM_COL32(20,19,18,250));
    d->PopClipRect();
    return true;
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
    // The Fighter design board: eight across beside the preview, fewer on a narrow screen.
    const float s=Scale(),gap=8*s,room=v2::Narrow(windowWidth,s)?windowWidth:windowWidth*.55f;
    RosterGrid grid;
    grid.columns=(std::max)(3,(std::min)(8,static_cast<int>(room/(76*s))));
    grid.cardHeight=(std::min)(120.f,(room-gap*(grid.columns-1))/grid.columns/s*.9f+26);
    return grid;
}
void SetMenuTextProbe(MenuTextProbe probe){textProbe=std::move(probe);}
void ReportMenuText(const char* id,float textHeight,float interiorHeight,float textWidth,float availableWidth){
    if(textProbe)textProbe(id,textHeight,interiorHeight,textWidth,availableWidth);
}
bool MenuProbesActive(){return static_cast<bool>(textProbe)||static_cast<bool>(cardProbe);}
void SetMenuCardProbe(MenuCardProbe probe){cardProbe=std::move(probe);}
void ReportMenuCard(const char* id,ImVec2 min,ImVec2 max){if(cardProbe)cardProbe(id,min,max);}
void SetMenuCardTextProbe(MenuCardTextProbe probe){cardTextProbe=std::move(probe);}
void ReportMenuCardText(const std::string& id,const std::string& text){if(cardTextProbe)cardTextProbe(id,text);}
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
// The pad's glyphs are copied, so a caller's text need not outlive the call.
std::string padSelectGlyph="LK",padBackGlyph="MK";
bool keyboardLast=false;unsigned previousKeys=0,previousPad=0;
void ApplyMenuGlyphs(){
    const bool pad=menuDeviceType==input::PadXInput||menuDeviceType==input::PadDirectInput;
    const bool keys=!pad||keyboardLast;
    selectGlyph=keys?"Enter":padSelectGlyph.c_str();
    backGlyph=keys?"Esc":padBackGlyph.c_str();
}
void SetMenuGlyphs(int type,unsigned select,unsigned back,const char* selectFallback,const char* backFallback,gamepads::PromptBrand brand){
    menuDeviceType=type;
    menuButtons={};menuButtons.confirm=select;menuButtons.back=back;
    promptBrand=brand;
    padSelectGlyph=PhysicalGlyph(type,select,selectFallback);
    padBackGlyph=PhysicalGlyph(type,back,backFallback);
    ApplyMenuGlyphs();
}
void SetMenuGlyphs(int type,const input::MenuButtons& buttons,gamepads::PromptBrand brand){
    SetMenuGlyphs(type,buttons.confirm,buttons.back,buttons.confirmGlyph,buttons.backGlyph,brand);
    // A stick's LP and HP buttons too, which name its tab shortcuts (MenuShortcutGlyph).
    menuButtons=buttons;
}
namespace { netplay::SpeechPreferences menuSpeech; int menuDescribes=0; }
void SetMenuSpeech(const netplay::SpeechPreferences& speech){ menuSpeech=speech; }
int TakeMenuDescribes(){ return std::exchange(menuDescribes,0); }
void SayMenuNotice(const std::string& text){ if(menuSpeech.enabled&&!text.empty())speech::Say({text},false,menuSpeech.rate,menuSpeech.volume); }
std::string AccessibilityKeyName(){ return KeyboardPrompts()?std::string("F2"):SpokenButton("RT"); }
gamepads::PromptBrand MenuPromptBrand(){ return promptBrand; }
const gamepads::FightNames* MenuPromptNames(){ return &promptNames; }
void SetMenuPromptBrand(gamepads::PromptBrand brand,const gamepads::FightNames& names){ promptBrand=brand;promptNames=names; }
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
const char* MenuShortcutGlyph(unsigned press){ return MenuShortcutGlyph(press,MenuPromptDevice(),menuButtons); }
std::optional<LegendHint> ShortcutHint(unsigned press,std::string label){
    if(const char* glyph=MenuShortcutGlyph(press))return LegendHint{glyph,std::move(label),press};
    return std::nullopt;
}
std::vector<LegendHint> ShortcutHints(const std::vector<std::pair<unsigned,std::string>>& shortcuts){
    std::vector<LegendHint> hints;
    for(const auto& s:shortcuts)if(auto hint=ShortcutHint(s.first,s.second))hints.push_back(std::move(*hint));
    return hints;
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
unsigned KeyboardMenuBits(bool typesText) {
    struct Mapping { ImGuiKey key; unsigned bit; };
    static const Mapping always[]={
        {ImGuiKey_UpArrow,MenuInput::Up},{ImGuiKey_DownArrow,MenuInput::Down},{ImGuiKey_LeftArrow,MenuInput::Left},
        {ImGuiKey_RightArrow,MenuInput::Right},{ImGuiKey_Enter,MenuInput::Select},{ImGuiKey_KeypadEnter,MenuInput::Select},{ImGuiKey_Escape,MenuInput::Back},{ImGuiKey_F2,MenuInput::Describe}};
    // Letters, Space, Backspace and Delete belong to a text field while one
    // is being typed in; so do the shortcuts' keys (MenuShortcuts).
    static const Mapping menuOnly[]={{ImGuiKey_Space,MenuInput::Select},{ImGuiKey_Backspace,MenuInput::Back}};
    unsigned bits=0;
    for(const auto& mapping:always) if(ImGui::IsKeyDown(mapping.key)) bits|=mapping.bit;
    if(!typesText&&!ImGui::GetIO().WantTextInput) {
        for(const auto& mapping:menuOnly) if(ImGui::IsKeyDown(mapping.key)) bits|=mapping.bit;
        for(const auto& shortcut:MenuShortcuts) if(ImGui::IsKeyDown(shortcut.key)) bits|=shortcut.press;
    }
    return bits;
}
MenuInput ReadMenuInput(bool textEditing,bool typesText,unsigned additionalPad) {
    auto value=input; value.time=ImGui::GetTime();
    const unsigned padHeld=input.held|additionalPad; value.held=padHeld;
    const unsigned keys=KeyboardMenuBits(typesText);
    // Typing counts as using the keyboard, though its keys press no menu bit.
    const auto& io=ImGui::GetIO();
    const bool typed=!io.InputQueueCharacters.empty()||ImGui::IsKeyPressed(ImGuiKey_Backspace,false)||ImGui::IsKeyPressed(ImGuiKey_Delete,false);
    if((keys&~previousKeys)||typed) keyboardLast=true;
    if(padHeld&~previousPad) keyboardLast=false;
    previousKeys=keys; previousPad=padHeld;
    ApplyMenuGlyphs();
    value.held|=textEditing?keys&~MenuInput::Select:keys; value.keyboard=keys;
    return value;
}
void GameMenu::ReadFrame(Frame& f) {
    const auto& entries=f.entries;
    const double now=clock_.Update(ImGui::GetTime());
    // A newly visible component cannot reuse its parent's opening press.
    if(lastFrame_!=ImGui::GetFrameCount()-1) { navigation.Rearm(); lastEdit_.clear(); }
    lastFrame_=ImGui::GetFrameCount();
    f.modalAtStart=navigation.Editing()||navigation.Asking();
    // A choice belongs to the body that draws it, so the body stays live for it.
    f.choosingAtStart=navigation.Choosing();
    auto menuInput=ReadMenuInput(navigation.Editing()&&notice_.empty(),typesText,additionalInput); menuInput.time=now;
    const unsigned rawHeld=menuInput.held;
    if(!listEnabled) menuInput.held=0;
    // A hint pressed on the last frame (DrawHintBar) is its button pressed on
    // this one, and whatever owns the input answers it as it answers the
    // button. It counts even while the list takes no input (a capture's
    // Cancel hint is one), and is the key the hint named while the prompts
    // are the keyboard's (an Escape always goes back).
    menuInput.held|=hintPress_;
    if(KeyboardPrompts()) menuInput.keyboard|=hintPress_;
    hintPress_=0;
    // The notice, else the on-screen keyboard, reads the directions in the
    // list's place, through one repeat; with neither open it starts afresh.
    {const auto editing=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.EditingId();});
     if(notice_.empty()&&!OnScreenKeyboard(editing==entries.end()?nullptr:&*editing)) dialogRepeat_.Reset();}
    ReadEditInput(entries,menuInput);
    // A notice owns the input until dismissed, so the dismissing press can
    // neither activate the focused row nor leak into the list behind it.
    f.noticeOpen=!notice_.empty();
    if(f.noticeOpen) {
        const unsigned held=noticeGate_.Held(menuInput.held), pressed=noticeGate_.Pressed(held), stepped=dialogRepeat_.Step(held,now);
        if(stepped&(MenuInput::Up|MenuInput::Left)) noticeSelected_=(std::max)(0,noticeSelected_-1);
        if(stepped&(MenuInput::Down|MenuInput::Right)) noticeSelected_=(std::min)(static_cast<int>(noticeButtons_.size())-1,noticeSelected_+1);
        // Back always declines: only Select takes the highlighted button.
        if(pressed&MenuInput::Back) DismissNotice(NoticeResult::Dismissed);
        else if(pressed&MenuInput::Select) DismissNotice(noticeButtons_[noticeSelected_].result);
        menuInput.held=0; menuInput.acceptText=false;
    } else noticeGate_.Rearm();
    // A focused toast answers this frame's presses in the list's place,
    // through the list's own gate, so the press it takes is spent: still held
    // once the toast lets go, it does not act on the screen. Describe stays
    // the accessibility button's.
    const bool toast=toastFocused&&!f.noticeOpen;
    toastPressed=toast?navigation.Take(menuInput.held)&~MenuInput::Describe:0;
    if(toast)menuInput.held=0;
    held_=f.held=menuInput.held;
    // Read before a notice takes the input: the accessibility button works in one too.
    const bool describe=(rawHeld&MenuInput::Describe)&&!(describeHeld_&MenuInput::Describe);describeHeld_=rawHeld;
    // InputText consumes this frame's characters before the requested acceptance.
    // The navigation model still owns the neutral gate and Back/accept ordering.
    {const auto open=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.DialogId();});
     // As the value panel last drew it (its column's width), else as the popup does.
     navigation.choiceColumns=navigation.Choosing()&&open!=entries.end()?(open->popout?1:valuesAcross_?valuesAcross_:MenuChoiceLayout(open->choices).across):1;}
    f.action=toast?MenuAction{}:navigation.Update(menuInput,entries,f.columns,true,true);
    f.acceptEditText=f.action.kind==MenuAction::SubmitText;
    if(f.acceptEditText)f.action={};
    feedback_.Update(navigation.Screen(),entries,now);
    if(statusProbe)statusProbe(f.status,f.statusTone);
    if(entriesProbe)entriesProbe(entries);
    // LB/RB step the tabs, wrapping, as a Shortcut naming the tab. Without
    // tabs the press stays as it is, for a body with a strip of its own.
    if(const int step=TabDelta(f.action);step&&!tabs.empty())f.action=TabAction(step);
    // An open dialog owns the header and the legend: Back cancels it, and
    // Select names the button it highlights.
    std::string dialogSelect,dialogBack;
    const bool dialog=DialogLegend(entries,dialogSelect,dialogBack);
    // Speech reads the rows (MenuNarrator).
    const auto focusedEntry=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.Focus();});
    MenuEntry presentationEntry;
    if(focusedEntry!=entries.end()) { presentationEntry=*focusedEntry; presentationEntry.enabled=feedback_.Enabled(*focusedEntry); }
    const bool adjustable=!dialog&&focusedEntry!=entries.end()&&presentationEntry.adjustable&&presentationEntry.enabled;
    const char* primary=!listEnabled?nullptr:dialog?(dialogSelect.empty()?nullptr:dialogSelect.c_str()):MenuPrimaryHint(focusedEntry==entries.end()?nullptr:&presentationEntry);
    // The on-screen keyboard's key the pad is on is its highlighted button.
    const std::string key=SpokenKey(entries);
    Narrate(f.title,entries,focusedEntry==entries.end()?nullptr:&presentationEntry,!key.empty()?key:dialog?dialogSelect:std::string(),primary,adjustable,now,describe);
}
MenuAction GameMenu::Finish(Frame& f) {
    const auto& entries=f.entries;
    auto& action=f.action;
    // A tab step that neither the tabs nor the body took does nothing.
    if(TabDelta(action))action={};
    // A value picked in its panel (OfferSteps) is the steps to it, as Left and Right would take.
    if(const auto to=action.kind==MenuAction::Chosen?RowKey<int>(action.text,"#"):std::nullopt){
        const auto row=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==action.id;});
        const auto from=row==entries.end()?std::nullopt:RowKey<int>(row->chosen,"#");
        action=from&&*to!=*from?MenuAction{MenuAction::Adjust,action.id,{},*to-*from}:MenuAction{};
    }
    // A renderer popup may survive a model cancellation by one frame. Close it
    // without rendering stale controls or reading a draft that was already cleared.
    DrawConfirmationModal(entries,action);
    if(!f.body&&!f.valuePanel)DrawChoiceModal(entries,action);
    DrawNoticeModal(f.noticeOpen);
    DrawEditModal(entries,f.acceptEditText,action);
    // The editor takes the keyboard focus when it opens, so a notice that hid it
    // hands the focus back the same way when it returns.
    lastEdit_=notice_.empty()?navigation.EditingId():std::string();
    lastFocus_=navigation.Focus(); lastScreen_=navigation.Screen();
    if(f.backRequested) return ownsBack?MenuAction{MenuAction::Back}:navigation.Return();
    return action;
}
void GameMenu::DrawListCard(ImVec2 listMin,ImVec2 listMax,ImVec2 rowMin,ImVec2 rowMax,float unit) {
    const auto cursor=ImGui::GetCursorScreenPos();const auto padding=ImGui::GetStyle().WindowPadding;
    const float margin=16*unit,width=(std::max)(1.f,listMax.x-listMin.x-2*margin);
    const float room=(std::max)(1.f,listMax.y-listMin.y-2*margin);
    const float height=(std::min)(room,listCard.measure?listCard.measure((std::max)(1.f,width-2*padding.x))+2*padding.y:room);
    // Centred on the list, unless that hides the focused row: then centred in
    // the larger band above or below it, kept inside the list.
    float top=(listMin.y+listMax.y-height)*.5f;
    const bool row=rowMax.y>rowMin.y&&rowMax.y>listMin.y&&rowMin.y<listMax.y;
    if(row&&top<rowMax.y&&top+height>rowMin.y) {
        const float above=rowMin.y-listMin.y,below=listMax.y-rowMax.y;
        top=above>below?listMin.y+(above-height)*.5f:rowMax.y+(below-height)*.5f;
        top=(std::max)(listMin.y+margin,(std::min)(top,listMax.y-margin-height));
    }
    const ImVec2 at(listMin.x+margin,top);
    BeginDialogCard("List card",at,ImVec2(width,height));
    listCard.draw();
    EndDialogCard();
    ReportMenuCard("list-card",at,ImVec2(at.x+width,at.y+height));
    ImGui::SetCursorScreenPos(cursor);
}
// The narrator follows every frame, so turning speech on says the new value
// instead of the whole screen. describe is a press of the accessibility button
// (RT, F2); F1 also stops speech while a menu shows.
void GameMenu::Narrate(const char* title,const std::vector<MenuEntry>& entries,const MenuEntry* focus,const std::string& button,const char* primary,bool adjustable,double now,bool describe) {
    SpokenFrame frame{title,focus};
    frame.entries=&entries;
    frame.notice=!notice_.empty()&&!noticeAsked_;
    for(const auto& noticeButton:noticeButtons_)frame.buttons.push_back(noticeButton.label);
    frame.accessKey=AccessibilityKeyName();
    // The focused entry may be a copy, so its place comes from its id.
    if(focus){frame.index=static_cast<std::size_t>(std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==focus->id;})-entries.begin());frame.count=entries.size();}
    frame.button=button;
    if(!button.empty()) {
        const auto& owner=navigation.Editing()?navigation.EditingId():navigation.DialogId();
        const auto entry=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==owner;});
        frame.question=!notice_.empty()?notice_:entry!=entries.end()?entry->label:std::string();
    }
    if(primary)frame.hint=SpokenButton(selectGlyph)+": "+primary;
    if(adjustable)frame.hint+=(frame.hint.empty()?"":". ")+std::string(loc::T("speech.change"));
    const auto said=narrator_.Update(frame,now,menuSpeech,menuSpeech.enabled&&describe);
    if(!menuSpeech.enabled)return;
    if(said.stop||ImGui::IsKeyPressed(ImGuiKey_F1,false)){speech::Stop();return;}
    if(said.chime&&menuSpeech.chime)speech::Chime(menuSpeech.chimeVolume);
    if(!said.parts.empty())speech::Say(said.parts,said.interrupt,menuSpeech.rate,menuSpeech.volume,said.tick);
    if(describe&&!said.parts.empty())++menuDescribes;
    speech::Render(said.render,menuSpeech.rate);
}
} }
