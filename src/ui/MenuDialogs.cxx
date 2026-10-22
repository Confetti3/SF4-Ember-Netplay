#include "MenuDialogs.hxx"
#include "V2.hxx"
#include "GameMenu.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>

namespace sf4e { namespace ui {
std::string FitLabel(const std::string& text,float width) {
    if(ImGui::CalcTextSize(text.c_str()).x<=width)return text;
    const char* end=nullptr;
    // A measured width is rounded up, so the cut leaves a pixel for that.
    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(),(std::max)(1.f,width-ImGui::CalcTextSize("...").x-1),0,text.c_str(),nullptr,&end);
    return std::string(text.c_str(),end)+"...";
}
namespace {
// Side by side while every label fits; otherwise stacked full width, so a
// long translation is never cut off.
bool Stacked(const std::vector<DialogButton>& buttons) {
    const auto& style=ImGui::GetStyle();const int count=static_cast<int>(buttons.size());
    const float width=(ImGui::GetContentRegionAvail().x-style.ItemSpacing.x*(count-1))/(std::max)(1,count);
    return std::any_of(buttons.begin(),buttons.end(),[&](const DialogButton& b){
        return ImGui::CalcTextSize(b.label.c_str()).x>width-2*style.FramePadding.x;});
}
}
float DialogButtonsHeight(const std::vector<DialogButton>& buttons,float height) {
    const float rows=Stacked(buttons)?static_cast<float>(buttons.size()):1.f;
    return rows*height+(rows-1)*ImGui::GetStyle().ItemSpacing.y;
}
// V2 (the Settings board's choice popup): up to three side by side, four as two by two, more three across.
std::size_t MenuChoiceColumns(std::size_t count) { return !v2::On()?1:count<=3?(std::max<std::size_t>)(1,count):count==4?2:3; }
MenuChoiceGrid MenuChoiceLayout(const std::vector<MenuChoice>& choices) {
    const float s=Scale(),gap=6*s,room=(std::min)(720*s,ImGui::GetMainViewport()->Size.x*.6f);
    float longest=0;
    for(const auto& c:choices)longest=(std::max)(longest,ImGui::CalcTextSize(c.label.c_str()).x);
    // The tick, the padding either side.
    float cell=(std::max)((choices.size()>8?190:170)*s,longest+46*s);
    std::size_t across=MenuChoiceColumns(choices.size());
    while(across>1&&across*cell+(across-1)*gap>room)--across;
    return {across,(std::min)(cell,room)};
}
// A dialog button's ground; V2 marks the selected one with its focus marks after drawing it (V2Mark).
static ImVec4 ButtonGround(bool selected) {
    if(!v2::On())return DialogButtonColor(selected);
    return ImGui::ColorConvertU32ToFloat4(selected?v2::Colors().header:v2::Colors().button);
}
static void V2Mark(bool selected) { if(v2::On()&&selected)v2::Focus(ImGui::GetWindowDrawList(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax()); }
int DrawDialogButtons(const char* probe,const std::vector<DialogButton>& buttons,int* selected,float height,bool focusOnAppearing) {
    const auto& style=ImGui::GetStyle();const int count=static_cast<int>(buttons.size());
    const bool stacked=Stacked(buttons);
    const float width=stacked?ImGui::GetContentRegionAvail().x:(ImGui::GetContentRegionAvail().x-style.ItemSpacing.x*(count-1))/(std::max)(1,count);
    // Opening a dialog under a moving pointer must preserve the caller's
    // default. Only movement after it appears can change the selection.
    const bool appearing=ImGui::IsWindowAppearing();
    const auto delta=ImGui::GetIO().MouseDelta;
    const bool moved=!appearing&&(delta.x!=0||delta.y!=0);
    int clicked=-1;
    for(int i=0;i<count;++i) {
        const auto& button=buttons[i];const float room=width-2*style.FramePadding.x;
        if(i&&!stacked)ImGui::SameLine();
        ImGui::PushID(i);
        const bool lit=button.lit&&selected&&*selected==i;
        ImGui::PushStyleColor(ImGuiCol_Button,ButtonGround(lit));
        ImGui::PushStyleColor(ImGuiCol_Text,EntryTextColor(button.lit));
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.f);
        ImGui::BeginDisabled(!button.enabled);
        // Menus own navigation through MenuNavigation (NoNavInputs), so
        // explicitly focus the default instead of ImGui's automatic nav init.
        if(focusOnAppearing&&appearing&&selected&&button.enabled&&*selected==i)ImGui::SetKeyboardFocusHere();
        if(ImGui::Button((FitLabel(button.label,room)+"###DialogButton").c_str(),ImVec2(width,height)))clicked=i;
        V2Mark(lit);
        if(selected&&moved&&ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))*selected=i;
        ImGui::EndDisabled();ImGui::PopStyleVar();ImGui::PopStyleColor(2);
        const auto measured=ImGui::CalcTextSize(button.label.c_str());
        ReportMenuText((std::string(probe)+"/"+std::to_string(i)).c_str(),measured.y,height-2*style.FramePadding.y,measured.x,room);
        ReportMenuCard((std::string(probe)+"/"+std::to_string(i)).c_str(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
        ImGui::PopID();
    }
    return clicked;
}
void BeginDialogCard(const char* id,ImVec2 at,ImVec2 size) {
    const ImVec4 panel=v2::On()?ImGui::ColorConvertU32ToFloat4(v2::Colors().panel):ImVec4(.13f,.115f,.1f,1);
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,panel);
    ImGui::BeginChild(id,size,ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    // ImGui may draw a child's background in its parent's draw list, under a
    // sibling drawn earlier (the menu list). The card paints its own panel so
    // it covers whatever it is drawn over.
    const auto min=ImGui::GetWindowPos(),max=ImVec2(min.x+ImGui::GetWindowSize().x,min.y+ImGui::GetWindowSize().y);
    auto* draw=ImGui::GetWindowDrawList();
    draw->PushClipRect(min,max);
    draw->AddRectFilled(min,max,ImGui::GetColorU32(panel),ImGui::GetStyle().ChildRounding);
    draw->AddRect(min,max,ImGui::GetColorU32(ImGuiCol_Border),ImGui::GetStyle().ChildRounding,0,ImGui::GetStyle().ChildBorderSize);
    draw->PopClipRect();
}
void EndDialogCard() { ImGui::EndChild();ImGui::PopStyleColor(); }
namespace {
void NextPopupSize() {
    const auto viewport=ImGui::GetMainViewport()->Size;
    const float width=(std::max)(1.f,viewport.x-40*Scale());
    const float height=(std::max)(1.f,viewport.y-40*Scale());
    ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(width,height));
    ImGui::SetNextWindowSize(ImVec2((std::min)(600*Scale(),width),0));
}
const MenuEntry* FindEntry(const std::vector<MenuEntry>& entries,const std::string& id) {
    const auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==id;});
    return it==entries.end()?nullptr:&*it;
}
// The dialogs' popup. V2: a card `width` wide without a title bar, edged in
// `edge` (the 3 px Ember rule down its left with `rule`), over the screen dimmed.
bool BeginDialogPopup(const char* id,float width,ImU32 edge,bool rule=false) {
    NextPopupSize();
    const bool look=v2::On();const float s=Scale();const auto& c=v2::Colors();
    if(look){
        ImGui::SetNextWindowSize(ImVec2((std::min)(width*s,ImGui::GetMainViewport()->Size.x-40*s),0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(24*s,22*s));ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,4*s);
        ImGui::PushStyleColor(ImGuiCol_PopupBg,ImGui::ColorConvertU32ToFloat4(c.ink));
        ImGui::PushStyleColor(ImGuiCol_Border,ImGui::ColorConvertU32ToFloat4(edge));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,ImGui::ColorConvertU32ToFloat4(v2::color::Alpha(c.ink,.72f)));
    }
    const bool open=ImGui::BeginPopupModal(id,nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNavInputs|(look?ImGuiWindowFlags_NoTitleBar:0));
    if(look){ImGui::PopStyleColor(3);ImGui::PopStyleVar(2);}
    if(open&&look&&rule){
        auto* d=ImGui::GetWindowDrawList();const auto at=ImGui::GetWindowPos();
        d->PushClipRect(at,ImVec2(at.x+3*s,at.y+ImGui::GetWindowHeight()));
        d->AddRectFilled(at,ImVec2(at.x+3*s,at.y+ImGui::GetWindowHeight()),c.accent,4*s,ImDrawFlags_RoundCornersLeft);
        d->PopClipRect();
    }
    return open;
}
// A V2 dialog's top: the brand's mark and `eyebrow`, `chip` at the right end,
// then `heading` (none when null) at `size` in `tone`.
void DialogTop(const std::string& eyebrow,const v2::HeaderChip& chip,const char* heading,ImU32 tone,float size) {
    const float s=Scale();const auto& c=v2::Colors();
    auto* d=ImGui::GetWindowDrawList();const auto at=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
    const float mark=v2::BrandMark(d,ImVec2(at.x,at.y+3*s),18*s);
    d->AddText(ImGui::GetFont(),14*s,ImVec2(at.x+mark+10*s,at.y+5*s),c.muted,eyebrow.c_str());
    if(!chip.text.empty())v2::Chip(d,ImVec2(at.x+width-v2::ChipWidth(chip.text),at.y),chip.text,chip.tone);
    ImGui::Dummy(ImVec2(width,24*s));
    if(!heading)return;
    size*=s;const auto h=HeadingFont()->CalcTextSizeA(size,FLT_MAX,width,heading);
    d->AddText(HeadingFont(),size,ImGui::GetCursorScreenPos(),tone,heading,nullptr,width);
    ImGui::Dummy(ImVec2(width,h.y+4*s));
}
}
void GameMenu::ShowNotice(NoticeDialog notice) {
    notice_=std::move(notice.text); noticeHeading_=std::move(notice.heading); noticeButtons_=std::move(notice.buttons); noticeAsked_=notice.asked;
    noticeEyebrow_=std::move(notice.eyebrow); noticeFacts_=std::move(notice.facts); noticeChip_=std::move(notice.chip);
    // A notice always has a button to close it by.
    if(noticeButtons_.empty()) noticeButtons_.push_back({loc::T("common.ok")});
    noticeSelected_=(std::max)(0,(std::min)(notice.selected,static_cast<int>(noticeButtons_.size())-1)); noticeError_=false; noticeResult_=NoticeResult::None;
}
void GameMenu::ShowNotice(std::string text,std::string heading,std::string okLabel) {
    ShowNotice(NoticeDialog{std::move(text),std::move(heading),{{okLabel.empty()?std::string(loc::T("common.ok")):std::move(okLabel)}}});
}
bool GameMenu::DialogLegend(const std::vector<MenuEntry>& entries,std::string& select,std::string& back) const {
    back=loc::T("common.cancel");
    if(!notice_.empty()) {
        // Back declines whichever button is highlighted, so it reads as closing the notice.
        select=noticeButtons_[noticeSelected_].label;back=loc::T("common.close");
    } else if(navigation.Editing()) select=KeyboardPrompts()||navigation.EditAccepts()?loc::T("common.accept"):loc::T("edit.cancel");
    else if(navigation.Reading()) { select.clear(); back=loc::T("common.close"); }
    else if(navigation.Choosing()) {
        const auto* entry=FindEntry(entries,navigation.DialogId());
        // An option that cannot be picked names no Select.
        select=entry&&navigation.ChoiceIndex()<entry->choices.size()&&entry->choices[navigation.ChoiceIndex()].enabled?
            entry->choices[navigation.ChoiceIndex()].label:std::string();
    } else if(navigation.Confirming()) {
        const auto* entry=FindEntry(entries,navigation.DialogId());
        if(entry&&!entry->cancelLabel.empty())back=entry->cancelLabel;
        select=navigation.ConfirmSelected()&&entry?entry->label:back;
    } else return false;
    return true;
}
// Cancel on the left, the entry's own action on the right, for both the modal
// and the flyout: the player confirms what the row said, not "Confirm".
std::vector<DialogButton> GameMenu::ConfirmationButtons(const std::vector<MenuEntry>& entries) const {
    const auto* entry=FindEntry(entries,navigation.DialogId());
    return {{entry&&!entry->cancelLabel.empty()?entry->cancelLabel:std::string(loc::T("common.cancel"))},
        {entry?entry->label:std::string(loc::T("common.confirm")),entry&&entry->enabled,entry&&feedback_.Enabled(*entry)}};
}
void GameMenu::AnswerConfirmation(const std::vector<MenuEntry>& entries,int clicked,int selected,MenuAction& action) {
    navigation.ConfirmSelected(selected==1);
    if(clicked>=0)action=navigation.Confirm(clicked==1,entries);
}
void GameMenu::DrawFlyoutConfirmation(const std::vector<MenuEntry>& entries,float unit,float top,float bottom,MenuAction& action) {
    // A local child deliberately replaces the viewport-dimming modal. It veils
    // only the body, so the header's Cancel and the legend stay in view; the
    // navigation model owns confirmation input and outside clicks do nothing.
    if(!navigation.Confirming())return;
    const auto pos=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
    const auto padding=ImGui::GetStyle().WindowPadding;
    const float height=(std::max)(1.f,bottom-top);
    ImGui::SetCursorScreenPos(ImVec2(pos.x+padding.x,top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,.8f));
    ImGui::BeginChild("Training veil",ImVec2(size.x-2*padding.x,height),0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 dialogSize(size.x-32*unit,(std::min)(270*unit,height-16*unit));
    BeginDialogCard("Training confirmation",ImVec2(pos.x+(size.x-dialogSize.x)*.5f,top+(height-dialogSize.y)*.5f),dialogSize);
    const auto* entry=FindEntry(entries,navigation.DialogId());
    const auto buttons=ConfirmationButtons(entries);
    ImGui::BeginChild("Confirmation explanation",ImVec2(0,ImGui::GetContentRegionAvail().y-DialogButtonsHeight(buttons,42*unit)-18*unit));
    const auto question=loc::Tf("confirm.question",entry?entry->label:std::string(loc::T("confirm.title")));
    v2::Wrapped(question);
    if(entry)v2::Wrapped(entry->detail);
    ImGui::EndChild();
    int selected=navigation.ConfirmSelected()?1:0;
    const int clicked=DrawDialogButtons("confirm",buttons,&selected,42*unit);
    AnswerConfirmation(entries,clicked,selected,action);
    EndDialogCard();
    ImGui::EndChild();ImGui::PopStyleColor();
}
void GameMenu::DrawConfirmationModal(const std::vector<MenuEntry>& entries,MenuAction& action) {
    const std::string confirmationPopup=std::string(loc::T("confirm.title"))+"###ConfirmAction";
    // A choice is drawn by the body that owns the entry, in place. A notice
    // takes the popup level while it is open (see DrawNoticeModal), so this
    // dialog is opened again, unchanged, once the notice is answered.
    if(navigation.Confirming()&&notice_.empty()) ImGui::OpenPopup(confirmationPopup.c_str());
    // V2: the question is the card's heading, under the dialog's name.
    if(BeginDialogPopup(confirmationPopup.c_str(),560,v2::Colors().border,true)) {
        if(!navigation.Confirming()) ImGui::CloseCurrentPopup();
        else {
            const auto* entry=FindEntry(entries,navigation.DialogId());
            const auto question=loc::Tf("confirm.question",entry?entry->label:std::string(loc::T("confirm.title")));
            if(v2::On())DialogTop(loc::T("confirm.title"),{},question.c_str(),v2::Colors().text,24);
            else v2::Wrapped(question);
            if(entry)v2::Wrapped(entry->detail);
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
            ImGui::BeginChild("Confirmation feedback",ImVec2(0,2*ImGui::GetTextLineHeightWithSpacing()));
            if(!entry||!feedback_.Enabled(*entry))ImGui::TextWrapped("%s",loc::T("confirm.updating"));
            ImGui::EndChild();ImGui::PopStyleColor();
            int selected=navigation.ConfirmSelected()?1:0;
            const int clicked=DrawDialogButtons("confirm",ConfirmationButtons(entries),&selected,48*Scale());
            AnswerConfirmation(entries,clicked,selected,action);
            if(!navigation.Confirming()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
void GameMenu::DrawReader(const std::vector<MenuEntry>& entries,const Detail& detail,float height,unsigned held) {
    const auto* entry=FindEntry(entries,navigation.ReadingId());
    if(!entry)return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.1f,.09f,.08f,.52f));
    // Each document has its own reader and opens at its top, so one never
    // inherits another's scroll, nor its own from an earlier reading.
    ImGui::BeginChild(("Menu reader/"+entry->id).c_str(),ImVec2(0,height),0,ImGuiWindowFlags_NoNavInputs);
    if(ImGui::IsWindowAppearing())ImGui::SetScrollY(0);
    const auto window=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
    ReportMenuCard("reader",window,ImVec2(window.x+size.x,window.y+size.y));
    if(entry->userText)NoteUserText(entry->label);
    ImGui::PushFont(HeadingFont());v2::Wrapped(entry->label);ImGui::PopFont();
    ReportMenuCard("reader-heading",ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
    if(!entry->detail.empty()){NoteDetailText(entry->detail,entry->detailText);v2::Wrapped(entry->detail);}
    if(!entry->value.empty()){if(entry->userText||entry->text)NoteUserText(entry->value);v2::Wrapped(entry->value);}
    if(detail)detail(entry->id);
    // Held Up/Down scroll smoothly; the mouse wheel and scrollbar still work.
    const float step=ImGui::GetTextLineHeightWithSpacing()*14*ImGui::GetIO().DeltaTime;
    if(held&MenuInput::Up)ImGui::SetScrollY(ImGui::GetScrollY()-step);
    if(held&MenuInput::Down)ImGui::SetScrollY(ImGui::GetScrollY()+step);
    ImGui::EndChild();ImGui::PopStyleColor();
}
void GameMenu::DrawChoiceModal(const std::vector<MenuEntry>& entries,MenuAction& action) {
    // A list's choice (the language) is drawn here; a body draws its own in place.
    const auto* entry=navigation.Choosing()?FindEntry(entries,navigation.DialogId()):nullptr;
    const std::string choicePopup=(entry?entry->label:std::string(loc::T("menu.choose")))+"###Choice";
    if(entry&&notice_.empty()) ImGui::OpenPopup(choicePopup.c_str());
    NextPopupSize();
    if(ImGui::BeginPopupModal(choicePopup.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNavInputs)) {
        // Each opening shows the selected option, wherever the list was
        // scrolled when it last closed.
        const bool appearing=ImGui::IsWindowAppearing();
        if(!entry) ImGui::CloseCurrentPopup();
        else {
            const float s=Scale(),row=40*s;const auto count=entry->choices.size();
            // V2 shows every option at once: a grid of up to three columns once a list passes six.
            const std::size_t columns=MenuChoiceColumns(count),lines=(count+columns-1)/columns;
            const float height=(std::min)(row*lines+ImGui::GetStyle().ItemSpacing.y*lines,ImGui::GetMainViewport()->Size.y*.55f);
            ImGui::BeginChild("Choice options",ImVec2(0,height),0,ImGuiWindowFlags_NoNavInputs);
            ReportMenuCard("choice-list",ImGui::GetWindowPos(),ImVec2(ImGui::GetWindowPos().x+ImGui::GetWindowSize().x,ImGui::GetWindowPos().y+ImGui::GetWindowSize().y));
            const auto delta=ImGui::GetIO().MouseDelta;const bool moved=delta.x!=0||delta.y!=0;
            for(std::size_t i=0;i<count&&navigation.Choosing();++i) {
                const auto& option=entry->choices[i];const bool selected=i==navigation.ChoiceIndex();
                ImGui::PushID(static_cast<int>(i));
                ImGui::PushStyleColor(ImGuiCol_Button,ButtonGround(selected));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,ImVec2(.03f,.5f));
                if(i%columns)ImGui::SameLine();
                const float full=ImGui::GetWindowContentRegionMax().x-ImGui::GetWindowContentRegionMin().x;
                const float width=(full-ImGui::GetStyle().ItemSpacing.x*(columns-1))/columns,room=width-2*ImGui::GetStyle().FramePadding.x;
                if(ImGui::Button((FitLabel(option.label,room)+"###Option").c_str(),ImVec2(width,row))) action=navigation.Pick(i,entries);
                else if(moved&&ImGui::IsItemHovered()) navigation.ChoiceIndex(i);
                V2Mark(selected);
                if(selected&&(appearing||lastChoice_!=i)) ImGui::SetScrollHereY(.5f);
                ImGui::PopStyleVar();ImGui::PopStyleColor();
                const auto measured=ImGui::CalcTextSize(option.label.c_str());
                ReportMenuText(("choice/"+option.id).c_str(),measured.y,row-2*ImGui::GetStyle().FramePadding.y,measured.x,room);
                ReportMenuCard(("choice/"+option.id).c_str(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
                ImGui::PopID();
            }
            ImGui::EndChild();
            lastChoice_=navigation.Choosing()?navigation.ChoiceIndex():~std::size_t(0);
            if(navigation.Choosing()&&navigation.ChoiceIndex()<count&&!entry->choices[navigation.ChoiceIndex()].detail.empty())
                v2::Wrapped(entry->choices[navigation.ChoiceIndex()].detail);
            // The header's Cancel sits under this modal, so the modal has its own.
            if(navigation.Choosing()&&DrawDialogButtons("choice-cancel",{{loc::T("common.cancel")}},nullptr,40*s)==0) navigation.Cancel();
            if(!navigation.Choosing()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
void GameMenu::DrawNoticeModal(bool noticeOpen) {
    const std::string noticePopup=std::string(loc::T("notice.title"))+"###Notice";
    // The dialogs open their popups at the same stack level, where opening one
    // closes another, so each stands aside while a notice is open instead of
    // trading places with it every frame. The navigation state they answer to
    // (a draft, a question) is untouched and their popup returns afterwards.
    if(!notice_.empty()) ImGui::OpenPopup(noticePopup.c_str());
    // V2: a card without a title bar, the brand's mark and the notice's eyebrow and chip on its top line.
    const bool look=v2::On();const float s=Scale();const auto& c=v2::Colors();
    const bool open=BeginDialogPopup(noticePopup.c_str(),560,noticeChip_.text.empty()?c.border:c.accent);
    if(open) {
        if(notice_.empty()) ImGui::CloseCurrentPopup();
        else {
            const char* heading=noticeError_?loc::T("notice.error_title"):noticeHeading_.empty()?loc::T("notice.title"):noticeHeading_.c_str();
            if(look){
                DialogTop(noticeEyebrow_,noticeChip_,heading,noticeError_?ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Error)):c.text,28);
                auto* d=ImGui::GetWindowDrawList();const float width=ImGui::GetContentRegionAvail().x;
                if(!noticeFacts_.empty()){
                    const auto box=ImGui::GetCursorScreenPos();const float bh=44*s;
                    d->AddRectFilled(box,ImVec2(box.x+width,box.y+bh),c.frame,4*s);d->AddRect(box,ImVec2(box.x+width,box.y+bh),c.border,4*s,0,s);
                    NoteUserText(noticeFacts_.c_str());
                    d->AddText(ImGui::GetFont(),16*s,ImVec2(box.x+12*s,box.y+(bh-16*s)*.5f),c.text,noticeFacts_.c_str());
                    ImGui::Dummy(ImVec2(width,bh+4*s));
                }
            }
            else{
                ImGui::TextColored(noticeError_?ToneColor(Tone::Error):ToneColor(Tone::Pending),"%s",heading);
                if(!noticeFacts_.empty()){NoteUserText(noticeFacts_.c_str());v2::Wrapped(noticeFacts_);}
            }
            NoteUserText(notice_.c_str());
            v2::Wrapped(notice_);
            ImGui::Dummy(ImVec2(0,8*Scale()));
            std::vector<DialogButton> buttons;
            for(const auto& button:noticeButtons_) buttons.push_back({button.label});
            int selected=noticeSelected_;
            const int clicked=DrawDialogButtons("notice",buttons,&selected,48*Scale());
            noticeSelected_=selected;
            if(clicked>=0) DismissNotice(noticeButtons_[clicked].result);
            if(notice_.empty()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if(noticeOpen&&notice_.empty()) navigation.NeutralGate();
}
namespace {
// The on-screen keyboard's keys (Keyboard.dc.html), its rows top to bottom.
const std::vector<std::vector<const char*>> KeyRows={{"1","2","3","4","5","6","7","8","9","0"},{"q","w","e","r","t","y","u","i","o","p"},
    {"a","s","d","f","g","h","j","k","l","-"},{"shift","z","x","c","v","b","n","m","_","delete"},{"space","done"}};
// The column straight above or below `column` of row `from` in row `to`: Space
// sits over the first six letter keys, Done over the rest.
int KeyBelow(int from,int to,int column,int count) {
    if(to==4)return column<6?0:1;
    if(from==4)return column?7:3;
    return (std::min)(column,count-1);
}
}
bool GameMenu::OnScreenKeyboard(const MenuEntry* editing) const {
    return v2::On()&&editing&&!editing->multiline&&navigation.Editing()&&notice_.empty()&&!KeyboardPrompts();
}
bool GameMenu::PressKey(const MenuEntry& entry,const std::string& key) {
    std::string draft=navigation.Draft();
    const std::size_t limit=(std::min)(std::size_t(4096),entry.textLimit);
    if(key=="done")return true;
    if(key=="shift")capitals_=!capitals_;
    else if(key=="delete"){while(!draft.empty()&&(static_cast<unsigned char>(draft.back())&0xC0)==0x80)draft.pop_back();if(!draft.empty())draft.pop_back();}
    else if(key.compare(0,7,"recent-")==0)draft=recentText_[entry.id][std::stoul(key.substr(7))];
    else{
        std::string typed=key=="space"?" ":key;
        if(capitals_)typed[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(typed[0])));
        if(draft.size()+typed.size()<=limit)draft+=typed;
    }
    keyTyped_=draft!=navigation.Draft();
    if(keyTyped_)navigation.Draft(std::move(draft));
    return false;
}
std::string GameMenu::SpokenKey(const std::vector<MenuEntry>& entries) const {
    const auto* entry=navigation.Editing()?FindEntry(entries,navigation.EditingId()):nullptr;
    if(!OnScreenKeyboard(entry))return {};
    if(keyRow_<0){const auto recent=recentText_.find(entry->id);return recent!=recentText_.end()&&keyColumn_<static_cast<int>(recent->second.size())?recent->second[keyColumn_]:std::string();}
    const std::string key=KeyRows[keyRow_][keyColumn_];
    if(key=="shift")return loc::T("keyboard.capitals");
    if(key=="delete"||key=="space"||key=="done")return loc::T(("keyboard."+key).c_str());
    return capitals_?std::string(1,static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])))):key;
}
void GameMenu::ReadEditInput(const std::vector<MenuEntry>& entries,MenuInput& input) {
    const unsigned pressed=input.held&~keyHeld_;keyHeld_=input.held;
    if(!navigation.Editing()) return;
    const auto* entry=FindEntry(entries,navigation.EditingId());
    // Match InputTextMultiline's default: either Enter inserts a newline,
    // Ctrl with either Enter validates. Single-line editors accept either key.
    const bool enter=ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false);
    input.acceptText|=entry&&enter&&(!entry->multiline||ImGui::GetIO().KeyCtrl);
    if(!OnScreenKeyboard(entry))return;
    // The pad types; only Back (Cancel) still reaches the editor's model.
    const int recent=static_cast<int>(recentText_[entry->id].size()),top=recent?-1:0;
    const auto count=[&](int row){return row<0?recent:static_cast<int>(KeyRows[row].size());};
    if(pressed&(MenuInput::Up|MenuInput::Down)){
        const int row=(std::max)(top,(std::min)(4,keyRow_+(pressed&MenuInput::Up?-1:1)));
        if(row!=keyRow_){keyColumn_=row<0||keyRow_<0?0:KeyBelow(keyRow_,row,keyColumn_,count(row));keyRow_=row;}
    }
    keyRow_=(std::max)(top,keyRow_);
    if(pressed&MenuInput::Left)keyColumn_=(std::max)(0,keyColumn_-1);
    if(pressed&MenuInput::Right)keyColumn_=(std::min)(count(keyRow_)-1,keyColumn_+1);
    keyColumn_=(std::min)(keyColumn_,count(keyRow_)-1);
    const std::string key=keyRow_<0?"recent-"+std::to_string(keyColumn_):KeyRows[keyRow_][keyColumn_];
    bool done=pressed&MenuInput::Start;
    if(pressed&MenuInput::Select)done|=PressKey(*entry,key);
    if(pressed&MenuInput::Fighter)PressKey(*entry,"delete");
    if(pressed&MenuInput::Options)PressKey(*entry,"space");
    if(pressed&MenuInput::PrevTab)PressKey(*entry,"shift");
    input.held&=MenuInput::Back;input.acceptText|=done;
}
void GameMenu::DrawKeyboard(const MenuEntry& entry,const std::vector<MenuEntry>& entries,MenuAction& action) {
    const float s=Scale();const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
    const float width=ImGui::GetContentRegionAvail().x,gap=6*s,h=52*s,key=(std::min)(76*s,(width-9*gap)/10);
    const auto mouse=ImGui::GetIO().MouseDelta;const bool moved=mouse.x!=0||mouse.y!=0;
    std::string clicked;
    // A key's ground, its focus and the pointer: moving over a key focuses it, a click presses it.
    const auto cell=[&](v2::Box b,int row,int column,const std::string& id,ImU32 ground){
        ImGui::SetCursorScreenPos(b.min);
        ImGui::PushID(id.c_str());const bool pressed=ImGui::InvisibleButton("##key",ImVec2(b.W(),b.H()));ImGui::PopID();
        if(ImGui::IsItemHovered()&&moved){keyRow_=row;keyColumn_=column;}
        if(pressed)clicked=id;
        const bool focused=keyRow_==row&&keyColumn_==column;
        d->AddRectFilled(b.min,b.max,focused?c.header:ground,4*s);
        d->AddRect(b.min,b.max,ground==c.accent||ground==c.hover?c.accent:c.border,4*s,0,s);
        if(focused)v2::Focus(d,b.min,b.max);
        return focused;
    };
    const auto label=[&](v2::Box b,const char* text,float size,ImU32 tone){
        const float w=HeadingFont()->CalcTextSizeA(size,FLT_MAX,0,text).x;
        d->AddText(HeadingFont(),size,ImVec2(b.min.x+(b.W()-w)*.5f,b.min.y+(b.H()-size)*.5f),tone,text);
    };
    auto at=ImGui::GetCursorScreenPos();
    const auto& recent=recentText_[entry.id];
    if(!recent.empty()){
        // The Recent names: the texts this field took before, one press away.
        const char* named=loc::T("keyboard.recent");const float rh=36*s;
        d->AddText(ImGui::GetFont(),14*s,ImVec2(at.x,at.y+(rh-14*s)*.5f),c.muted,named);
        float x=at.x+ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,0,named).x+8*s;
        for(int i=0;i<static_cast<int>(recent.size());++i){
            NoteUserText(recent[i]);
            const float w=ImGui::CalcTextSize(recent[i].c_str()).x+24*s;
            const v2::Box b{ImVec2(x,at.y),ImVec2(x+w,at.y+rh)};
            cell(b,-1,i,"recent-"+std::to_string(i),c.button);
            d->AddText(ImVec2(b.min.x+12*s,b.min.y+(rh-ImGui::GetFontSize())*.5f),c.text,recent[i].c_str());
            x+=w+8*s;
        }
        at.y+=rh+12*s;
    }
    for(int row=0;row<5;++row){
        const auto& keys=KeyRows[row];
        // Space and Done: five keys and two and a half, as the board's 420 and 200 of 76.
        const auto wide=[&](int i){return row<4?key:i?2.6f*key:5.5f*key;};
        float total=-gap;for(int i=0;i<static_cast<int>(keys.size());++i)total+=wide(i)+gap;
        float x=at.x+(width-total)*.5f;
        for(int i=0;i<static_cast<int>(keys.size());++i){
            const std::string id=keys[i];const v2::Box b{ImVec2(x,at.y),ImVec2(x+wide(i),at.y+h)};x+=wide(i)+gap;
            const bool done=id=="done",shift=id=="shift";
            const bool focused=cell(b,row,i,id,done?c.accent:shift&&capitals_?c.hover:c.button);
            const ImU32 tone=done&&!focused?c.ink:c.text;
            if(shift){
                // An outlined arrow up, filled while capitals are on.
                const ImVec2 m(b.min.x+b.W()*.5f,b.min.y+h*.5f);const float u=7*s;
                const ImVec2 arrow[]={{m.x,m.y-u*1.4f},{m.x+u*1.2f,m.y},{m.x+u*.5f,m.y},{m.x+u*.5f,m.y+u*1.2f},{m.x-u*.5f,m.y+u*1.2f},{m.x-u*.5f,m.y},{m.x-u*1.2f,m.y}};
                if(capitals_){d->AddTriangleFilled(arrow[0],arrow[1],arrow[6],c.accent);d->AddRectFilled(arrow[5],arrow[3],c.accent);}
                else d->AddPolyline(arrow,7,tone,ImDrawFlags_Closed,2*s);
            }else if(id=="delete"){
                // A key cap pointing left with a cross on it.
                const ImVec2 m(b.min.x+b.W()*.5f,b.min.y+h*.5f);const float u=8*s;
                const ImVec2 cap[]={{m.x-u*1.6f,m.y},{m.x-u*.6f,m.y-u},{m.x+u*1.4f,m.y-u},{m.x+u*1.4f,m.y+u},{m.x-u*.6f,m.y+u}};
                d->AddPolyline(cap,5,tone,ImDrawFlags_Closed,2*s);
                d->AddLine(ImVec2(m.x,m.y-u*.4f),ImVec2(m.x+u*.8f,m.y+u*.4f),tone,2*s);d->AddLine(ImVec2(m.x+u*.8f,m.y-u*.4f),ImVec2(m.x,m.y+u*.4f),tone,2*s);
            }else if(id=="space"||done)label(b,loc::T(done?"keyboard.done":"keyboard.space"),17*s,tone);
            else{std::string shown=id;if(capitals_)shown[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(shown[0])));label(b,shown.c_str(),20*s,tone);}
        }
        at.y+=h+gap;
    }
    // What the pad's buttons do, each also a button for the mouse.
    const std::vector<v2::Hint> hints{{MenuSelectGlyph(),loc::T("keyboard.type")},{"X",loc::T("keyboard.delete")},{"Y",loc::T("keyboard.space")},
        {"LB",loc::T("keyboard.capitals")},{"Start",loc::T("keyboard.done")},{MenuBackGlyph(),loc::T("common.cancel")}};
    const int hint=v2::HintBar({ImVec2(at.x-24*s,at.y+6*s),ImVec2(at.x+width+24*s,at.y+50*s)},hints,"",c.muted);
    ImGui::SetCursorScreenPos(ImVec2(at.x,at.y+50*s));ImGui::Dummy(ImVec2(width,0));
    const char* hinted[]={"","delete","space","shift","done",""};
    if(hint==0&&keyRow_>=0)clicked=KeyRows[keyRow_][keyColumn_];
    else if(hint==0)clicked="recent-"+std::to_string(keyColumn_);
    else if(hint>0&&hint<5)clicked=hinted[hint];
    if(hint==5)navigation.Cancel();
    if(!clicked.empty()&&PressKey(entry,clicked)&&entry.enabled)action=navigation.AcceptText(entries);
}
void GameMenu::DrawEditModal(const std::vector<MenuEntry>& entries,bool acceptEditText,MenuAction& action) {
    const std::string editPopup=std::string(loc::T("edit.title"))+"###EditText";
    if(navigation.Editing()&&notice_.empty()) ImGui::OpenPopup(editPopup.c_str());
    const bool look=v2::On();const float s=Scale();const auto& c=v2::Colors();
    // The on-screen keyboard's card sits at the bottom, under the field, as on the Deck.
    const bool keyboard=OnScreenKeyboard(navigation.Editing()?FindEntry(entries,navigation.EditingId()):nullptr);
    if(keyboard){const auto* vp=ImGui::GetMainViewport();ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x*.5f,vp->Pos.y+vp->Size.y-24*s),ImGuiCond_Always,ImVec2(.5f,1));}
    if(BeginDialogPopup(editPopup.c_str(),keyboard?1040:720,c.border)) {
        if(!navigation.Editing()) ImGui::CloseCurrentPopup();
        else {
            const auto* entry=FindEntry(entries,navigation.EditingId());
            const std::size_t limit=entry?(std::min)(entry->multiline?std::size_t(8000):std::size_t(4096),entry->textLimit):4096;
            const bool canAccept=entry&&entry->enabled;
            const char* label=entry?entry->label.c_str():loc::T("edit.title");
            const char* instructions=entry&&entry->multiline?loc::T("edit.instructions_multiline"):loc::T("edit.instructions");
            const auto bytes=loc::Tf("edit.bytes",static_cast<unsigned>(navigation.Draft().size()),static_cast<unsigned>(limit));
            // V2: the field's name and its count above it, the field edged in Ember, how to type under it.
            if(look)DialogTop(label,{bytes,c.muted},nullptr,0,0);
            else{ImGui::TextWrapped("%s",label);ImGui::TextWrapped("%s",instructions);}
            const bool secret=navigation.EditingSecret();
            std::vector<char> draft(limit+1,0); std::strncpy(draft.data(),navigation.Draft().c_str(),limit);
            // A masked passphrase draws only asterisks, so its glyphs are never needed.
            if(!secret) NoteUserText(navigation.Draft(),UserTextRole::Draft);
            if(lastEdit_!=navigation.EditingId()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            // What the on-screen keyboard typed replaces the field's own copy, the cursor at its end.
            if(keyTyped_)if(auto* typed=ImGui::GetInputTextState(ImGui::GetID(secret?"##Secret":"##Draft")))typed->ReloadUserBufAndMoveToEnd();
            keyTyped_=false;
            if(look){
                ImGui::PushStyleColor(ImGuiCol_FrameBg,ImGui::ColorConvertU32ToFloat4(c.frame));ImGui::PushStyleColor(ImGuiCol_Border,ImGui::ColorConvertU32ToFloat4(c.accent));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,2*s);ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,4*s);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(14*s,(std::max)(4*s,(52*s-ImGui::GetFontSize())*.5f)));
            }
            const bool changed=entry&&entry->multiline?
                ImGui::InputTextMultiline("##Draft",draft.data(),draft.size(),ImVec2(-1,4*ImGui::GetTextLineHeightWithSpacing())):
                ImGui::InputText(secret?"##Secret":"##Draft",draft.data(),draft.size(),secret?ImGuiInputTextFlags_Password:0);
            if(look){ImGui::PopStyleVar(3);ImGui::PopStyleColor(2);}
            if(changed) navigation.Draft(draft.data());
            if(secret) WipeText(draft.data(),draft.size());
            if(keyboard&&entry){
                ImGui::Dummy(ImVec2(0,4*s));DrawKeyboard(*entry,entries,action);
                if(canAccept&&acceptEditText)action=navigation.AcceptText(entries);
            }else{
                if(look)ImGui::TextWrapped("%s",instructions);
                else ImGui::TextDisabled("%s",bytes.c_str());
                const bool visualAccept=entry&&feedback_.Enabled(*entry);
                ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
                ImGui::BeginChild("Edit feedback",ImVec2(0,2*ImGui::GetTextLineHeightWithSpacing()));
                if(!visualAccept)ImGui::TextWrapped("%s",loc::T("edit.updating"));
                ImGui::EndChild();ImGui::PopStyleColor();
                // The highlight is what the controller's Select presses; a moving
                // pointer moves it, and the model keeps it.
                int accept=navigation.EditAccepts()?1:0;
                const int clicked=DrawDialogButtons("edit",{{loc::T("edit.cancel")},{loc::T("common.accept"),canAccept,visualAccept}},&accept,48*Scale(),false);
                navigation.EditAccepts(accept==1);
                if(canAccept&&(clicked==1||acceptEditText)) action=navigation.AcceptText(entries);
                else if(clicked==0) navigation.Cancel();
            }
            // An accepted text leads the field's Recent names on the on-screen keyboard; never a passphrase.
            if(action.kind==MenuAction::TextAccepted&&!secret&&!action.text.empty()){
                auto& recent=recentText_[action.id];
                recent.erase(std::remove(recent.begin(),recent.end(),action.text),recent.end());
                recent.insert(recent.begin(),action.text);if(recent.size()>3)recent.pop_back();
            }
            if(!navigation.Editing()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
} }
