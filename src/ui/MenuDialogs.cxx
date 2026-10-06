#include "MenuDialogs.hxx"
#include "GameMenu.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cstring>

namespace sf4e { namespace ui {
std::string FitLabel(const std::string& text,float width) {
    if(ImGui::CalcTextSize(text.c_str()).x<=width)return text;
    const char* end=nullptr;
    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(),(std::max)(1.f,width-ImGui::CalcTextSize("...").x),0,text.c_str(),nullptr,&end);
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
int DrawDialogButtons(const char* probe,const std::vector<DialogButton>& buttons,int* selected,float height) {
    const auto& style=ImGui::GetStyle();const int count=static_cast<int>(buttons.size());
    const bool stacked=Stacked(buttons);
    const float width=stacked?ImGui::GetContentRegionAvail().x:(ImGui::GetContentRegionAvail().x-style.ItemSpacing.x*(count-1))/(std::max)(1,count);
    const auto delta=ImGui::GetIO().MouseDelta;const bool moved=delta.x!=0||delta.y!=0;
    int clicked=-1;
    for(int i=0;i<count;++i) {
        const auto& button=buttons[i];const float room=width-2*style.FramePadding.x;
        if(i&&!stacked)ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button,DialogButtonColor(button.lit&&selected&&*selected==i));
        ImGui::PushStyleColor(ImGuiCol_Text,EntryTextColor(button.lit));
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.f);
        ImGui::BeginDisabled(!button.enabled);
        if(ImGui::Button((FitLabel(button.label,room)+"###DialogButton").c_str(),ImVec2(width,height)))clicked=i;
        if(selected&&moved&&ImGui::IsItemHovered())*selected=i;
        ImGui::EndDisabled();ImGui::PopStyleVar();ImGui::PopStyleColor(2);
        const auto measured=ImGui::CalcTextSize(button.label.c_str());
        ReportMenuText((std::string(probe)+"/"+std::to_string(i)).c_str(),measured.y,height-2*style.FramePadding.y,measured.x,room);
        ReportMenuCard((std::string(probe)+"/"+std::to_string(i)).c_str(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
        ImGui::PopID();
    }
    return clicked;
}
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
}
bool GameMenu::DialogLegend(const std::vector<MenuEntry>& entries,std::string& select,std::string& back) const {
    back=loc::T("common.cancel");
    if(!notice_.empty()) {
        // Back declines a notice's alternative, so it reads as closing it.
        select=noticeAlternativeSelected_?noticeAlternative_:loc::T("common.ok");back=loc::T("common.close");
    } else if(navigation.Editing()) select=navigation.EditAccepts()?loc::T("common.accept"):loc::T("edit.cancel");
    else if(navigation.Reading()) { select.clear(); back=loc::T("common.close"); }
    else if(navigation.Choosing()) {
        const auto* entry=FindEntry(entries,navigation.DialogId());
        // An option that cannot be picked names no Select.
        select=entry&&navigation.ChoiceIndex()<entry->choices.size()&&entry->choices[navigation.ChoiceIndex()].enabled?
            entry->choices[navigation.ChoiceIndex()].label:std::string();
    } else if(navigation.Confirming()) {
        const auto* entry=FindEntry(entries,navigation.DialogId());
        select=navigation.ConfirmSelected()&&entry?entry->label:std::string(loc::T("common.cancel"));
    } else return false;
    return true;
}
// Cancel on the left, the entry's own action on the right, for both the modal
// and the flyout: the player confirms what the row said, not "Confirm".
std::vector<DialogButton> GameMenu::ConfirmationButtons(const std::vector<MenuEntry>& entries) const {
    const auto* entry=FindEntry(entries,navigation.DialogId());
    return {{loc::T("common.cancel")},{entry?entry->label:std::string(loc::T("common.confirm")),entry&&entry->enabled,entry&&feedback_.Enabled(*entry)}};
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
    ImGui::SetCursorScreenPos(ImVec2(pos.x+(size.x-dialogSize.x)*.5f,top+(height-dialogSize.y)*.5f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.13f,.115f,.1f,1));
    ImGui::BeginChild("Training confirmation",dialogSize,ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const auto* entry=FindEntry(entries,navigation.DialogId());
    const auto buttons=ConfirmationButtons(entries);
    ImGui::BeginChild("Confirmation explanation",ImVec2(0,ImGui::GetContentRegionAvail().y-DialogButtonsHeight(buttons,42*unit)-18*unit));
    const auto question=loc::Tf("confirm.question",entry?entry->label:std::string(loc::T("confirm.title")));
    ImGui::TextWrapped("%s",question.c_str());
    if(entry)ImGui::TextWrapped("%s",entry->detail.c_str());
    ImGui::EndChild();
    int selected=navigation.ConfirmSelected()?1:0;
    const int clicked=DrawDialogButtons("confirm",buttons,&selected,42*unit);
    AnswerConfirmation(entries,clicked,selected,action);
    ImGui::EndChild();ImGui::PopStyleColor();
    ImGui::EndChild();ImGui::PopStyleColor();
}
void GameMenu::DrawConfirmationModal(const std::vector<MenuEntry>& entries,MenuAction& action) {
    const std::string confirmationPopup=std::string(loc::T("confirm.title"))+"###ConfirmAction";
    // A choice is drawn by the body that owns the entry, in place. A notice
    // takes the popup level while it is open (see DrawNoticeModal), so this
    // dialog is opened again, unchanged, once the notice is answered.
    if(navigation.Confirming()&&notice_.empty()) ImGui::OpenPopup(confirmationPopup.c_str());
    NextPopupSize();
    if(ImGui::BeginPopupModal(confirmationPopup.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNavInputs)) {
        if(!navigation.Confirming()) ImGui::CloseCurrentPopup();
        else {
            const auto* entry=FindEntry(entries,navigation.DialogId());
            const auto question=loc::Tf("confirm.question",entry?entry->label:std::string(loc::T("confirm.title")));
            ImGui::TextWrapped("%s",question.c_str());
            if(entry)ImGui::TextWrapped("%s",entry->detail.c_str());
            ImGui::BeginChild("Confirmation feedback",ImVec2(0,2*ImGui::GetTextLineHeightWithSpacing()));
            if(!entry||!feedback_.Enabled(*entry))ImGui::TextWrapped("%s",loc::T("confirm.updating"));
            ImGui::EndChild();
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
    ImGui::PushFont(HeadingFont());ImGui::TextWrapped("%s",entry->label.c_str());ImGui::PopFont();
    ReportMenuCard("reader-heading",ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
    if(!entry->detail.empty()){NoteDetailText(entry->detail,entry->detailText);ImGui::TextWrapped("%s",entry->detail.c_str());}
    if(!entry->value.empty()){if(entry->userText||entry->text)NoteUserText(entry->value);ImGui::TextWrapped("%s",entry->value.c_str());}
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
            const float height=(std::min)(row*count+ImGui::GetStyle().ItemSpacing.y*count,ImGui::GetMainViewport()->Size.y*.55f);
            ImGui::BeginChild("Choice options",ImVec2(0,height),0,ImGuiWindowFlags_NoNavInputs);
            ReportMenuCard("choice-list",ImGui::GetWindowPos(),ImVec2(ImGui::GetWindowPos().x+ImGui::GetWindowSize().x,ImGui::GetWindowPos().y+ImGui::GetWindowSize().y));
            const auto delta=ImGui::GetIO().MouseDelta;const bool moved=delta.x!=0||delta.y!=0;
            for(std::size_t i=0;i<count&&navigation.Choosing();++i) {
                const auto& option=entry->choices[i];const bool selected=i==navigation.ChoiceIndex();
                ImGui::PushID(static_cast<int>(i));
                ImGui::PushStyleColor(ImGuiCol_Button,DialogButtonColor(selected));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,ImVec2(.03f,.5f));
                const float width=ImGui::GetContentRegionAvail().x,room=width-2*ImGui::GetStyle().FramePadding.x;
                if(ImGui::Button((FitLabel(option.label,room)+"###Option").c_str(),ImVec2(width,row))) action=navigation.Pick(i,entries);
                else if(moved&&ImGui::IsItemHovered()) navigation.ChoiceIndex(i);
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
                ImGui::TextWrapped("%s",entry->choices[navigation.ChoiceIndex()].detail.c_str());
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
    NextPopupSize();
    if(ImGui::BeginPopupModal(noticePopup.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNavInputs)) {
        if(notice_.empty()) ImGui::CloseCurrentPopup();
        else {
            if(noticeError_) ImGui::TextColored(ToneColor(Tone::Error),"%s",loc::T("notice.error_title"));
            else ImGui::TextColored(ToneColor(Tone::Pending),"%s",noticeHeading_.empty()?loc::T("notice.title"):noticeHeading_.c_str());
            ImGui::TextWrapped("%s",notice_.c_str());
            ImGui::Dummy(ImVec2(0,8*Scale()));
            std::vector<DialogButton> buttons{{loc::T("common.ok")}};
            if(!noticeAlternative_.empty())buttons.push_back({noticeAlternative_});
            int selected=noticeAlternativeSelected_?1:0;
            const int clicked=DrawDialogButtons("notice",buttons,&selected,48*Scale());
            noticeAlternativeSelected_=selected==1;
            if(clicked>=0) DismissNotice(clicked==1);
            if(notice_.empty()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if(noticeOpen&&notice_.empty()) navigation.NeutralGate();
}
void GameMenu::DrawEditModal(const std::vector<MenuEntry>& entries,bool acceptEditText,MenuAction& action) {
    const std::string editPopup=std::string(loc::T("edit.title"))+"###EditText";
    if(navigation.Editing()&&notice_.empty()) ImGui::OpenPopup(editPopup.c_str());
    NextPopupSize();
    if(ImGui::BeginPopupModal(editPopup.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNavInputs)) {
        if(!navigation.Editing()) ImGui::CloseCurrentPopup();
        else {
            const auto* entry=FindEntry(entries,navigation.EditingId());
            const std::size_t limit=entry?(std::min)(std::size_t(8192),entry->textLimit):4096;
            const bool canAccept=entry&&entry->enabled;
            ImGui::TextWrapped("%s",entry?entry->label.c_str():loc::T("edit.title"));
            ImGui::TextWrapped("%s",loc::T("edit.instructions"));
            const bool secret=navigation.EditingSecret();
            char draft[8193]={}; std::strncpy(draft,navigation.Draft().c_str(),limit);
            // A masked passphrase draws only asterisks, so its glyphs are never needed.
            if(!secret) NoteUserText(navigation.Draft(),UserTextRole::Draft);
            if(lastEdit_!=navigation.EditingId()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if(ImGui::InputText(secret?"##Secret":"##Draft",draft,limit+1,secret?ImGuiInputTextFlags_Password:0)) navigation.Draft(draft);
            if(secret) WipeText(draft,sizeof(draft));
            ImGui::TextDisabled("%s",loc::Tf("edit.bytes",static_cast<unsigned>(navigation.Draft().size()),static_cast<unsigned>(limit)).c_str());
            const bool visualAccept=entry&&feedback_.Enabled(*entry);
            ImGui::BeginChild("Edit feedback",ImVec2(0,2*ImGui::GetTextLineHeightWithSpacing()));
            if(!visualAccept)ImGui::TextWrapped("%s",loc::T("edit.updating"));
            ImGui::EndChild();
            // The highlight is what the controller's Select presses; a moving
            // pointer moves it, and the model keeps it.
            int accept=navigation.EditAccepts()?1:0;
            const int clicked=DrawDialogButtons("edit",{{loc::T("edit.cancel")},{loc::T("common.accept"),canAccept,visualAccept}},&accept,48*Scale());
            navigation.EditAccepts(accept==1);
            if(canAccept&&(clicked==1||acceptEditText)) action=navigation.AcceptText(entries);
            else if(clicked==0) navigation.Cancel();
            if(!navigation.Editing()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
} }
