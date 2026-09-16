#include "GameMenu.hxx"
#include "Theme.hxx"
#include "SelectionArt.hxx"
#include "MenuGlyphs.hxx"
#include "MenuPresentation.hxx"
#include "../common/FighterCatalog.hxx"
#include <imgui.h>
#include <cstring>

namespace sf4e { namespace ui {
namespace { MenuInput input; bool returnRequested=false; SelectionArt* menuArt=nullptr;
const char* selectGlyph="LP";const char* backGlyph="LK";PlayerCardView playerCard;
int menuDeviceType=0;
MenuTextProbe textProbe;
MenuCardProbe cardProbe;
MenuStatusProbe statusProbe;
MenuEntriesProbe entriesProbe;
PortraitProbe portraitProbe;
std::string FitLabel(const std::string& text,float width) {
    if(ImGui::CalcTextSize(text.c_str()).x<=width)return text;
    const char* end=nullptr;
    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(),(std::max)(1.f,width-ImGui::CalcTextSize("...").x),0,text.c_str(),nullptr,&end);
    return std::string(text.c_str(),end)+"...";
}
void DrawPlayerCard(ImVec2 p,float width,bool compact) {
    const float s=Scale(),height=(compact?94:142)*s;auto* d=ImGui::GetWindowDrawList();
    d->AddRectFilled(p,ImVec2(p.x+width,p.y+height),IM_COL32(17,16,15,215),3*s);
    d->AddLine(p,ImVec2(p.x+width,p.y),palette::Ember,2*s);
    const float portrait=(compact?48:66)*s;
    if(menuArt){const auto art=menuArt->Portrait(playerCard.fighter,true);if(art.texture){const float ratio=float(art.width)/art.height;
        const float w=(std::min)(portrait,portrait*ratio),h=w/ratio;
        d->AddImage(art.texture,ImVec2(p.x+12*s,p.y+12*s),ImVec2(p.x+12*s+w,p.y+12*s+h),art.uvMin,art.uvMax);}}
    const auto text=[&](float x,float y,const std::string& label,float font,ImU32 color){
        const float max=width-x-12*s;std::string shown=label;
        if(ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,label.c_str()).x>max){
            const char* end=nullptr;const float dots=ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,"...").x;
            ImGui::GetFont()->CalcTextSizeA(font,(std::max)(1.f,max-dots),0,label.c_str(),nullptr,&end);
            shown=std::string(label.c_str(),end)+"...";
        }
        d->AddText(ImGui::GetFont(),font,ImVec2(p.x+x,p.y+y),color,shown.c_str());};
    text(portrait+24*s,13*s,playerCard.name.empty()?"PLAYER":playerCard.name,18*s,palette::Ivory);
    text(portrait+24*s,38*s,playerCard.fighterName.empty()?"Choose your fighter":playerCard.fighterName,12*s,palette::Muted);
    if(!compact)text(12*s,82*s,playerCard.controllerReady?"CONTROLLER READY":"ASSIGN CONTROLLER",11*s,playerCard.controllerReady?palette::Ready:palette::Ember);
    const float y=(compact?70:116)*s;
    text(12*s,y,playerCard.recordAvailable?std::to_string(playerCard.wins)+"  WINS":"-- WINS",11*s,palette::Ivory);
    text(width*.35f,y,playerCard.recordAvailable?std::to_string(playerCard.losses)+"  LOSSES":"-- LOSSES",11*s,palette::Muted);
    const auto games=playerCard.wins+playerCard.losses;
    text(width*.7f,y,!playerCard.recordAvailable?std::string("-- WIN %"):
        games?std::to_string(playerCard.wins*100/games)+"% WIN":std::string("0% WIN"),11*s,palette::Muted);
}
}
void SetMenuPlayerCard(PlayerCardView view){playerCard=std::move(view);}
void SetMenuTextProbe(MenuTextProbe probe){textProbe=std::move(probe);}
void ReportMenuText(const char* id,float textHeight,float interiorHeight,float textWidth,float availableWidth){
    if(textProbe)textProbe(id,textHeight,interiorHeight,textWidth,availableWidth);
}
void SetMenuCardProbe(MenuCardProbe probe){cardProbe=std::move(probe);}
void SetMenuStatusProbe(MenuStatusProbe probe){statusProbe=std::move(probe);}
void SetMenuEntriesProbe(MenuEntriesProbe probe){entriesProbe=std::move(probe);}
void SetPortraitProbe(PortraitProbe probe){portraitProbe=std::move(probe);}
void DrawCharacterPortrait(int fighter,ImVec2 min,ImVec2 max){
    if(portraitProbe)portraitProbe(fighter,min,max);
    auto* d=ImGui::GetWindowDrawList();
    d->AddRectFilled(min,max,IM_COL32(38,34,30,255));
    if(menuArt){const auto art=menuArt->Portrait(fighter,true);if(art.texture){
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
    const std::string label=FitLabel(selection::FindFighter(fighter)->name,max.x-min.x-8*Scale());
    d->AddRectFilled(ImVec2(min.x,max.y-footer),max,IM_COL32(16,15,14,235));
    d->AddText(ImVec2(min.x+4*Scale(),max.y-footer),saved?palette::Ember:palette::Ivory,label.c_str());
    if(saved){const auto p=ImVec2(min.x+4*Scale(),min.y+4*Scale());
        d->AddRectFilled(p,ImVec2(p.x+ImGui::CalcTextSize("MAIN").x+8*Scale(),p.y+ImGui::GetTextLineHeight()+2*Scale()),IM_COL32(16,15,14,230));
        d->AddText(ImVec2(p.x+4*Scale(),p.y),palette::Ember,"MAIN");}
}
void SetMenuGlyphs(int type,unsigned select,unsigned back){
    menuDeviceType=type;
    selectGlyph=type!=3&&type!=4?"Enter":PhysicalGlyph(type,select,"LP");
    backGlyph=type!=3&&type!=4?"Esc":PhysicalGlyph(type,back,"LK");
}
void DrawTrainingOpenPrompt() {
    ImGui::TextWrapped("F6 Training controls | F5 Hide HUD");
}
void SetMenuArt(SelectionArt* art) { menuArt=art; }
void RequestMenuReturn() { returnRequested=true; }
bool TakeMenuReturn() { const bool value=returnRequested; returnRequested=false; return value; }
void SetMenuInput(MenuInput value) { input=value; }
MenuInput ReadMenuInput() {
    auto value=input; value.time=ImGui::GetTime();
    const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
    for(unsigned i=0;i<6;++i) if(ImGui::IsKeyDown(keys[i])) value.held|=1u<<i;
    value.acceptText=ImGui::IsKeyPressed(ImGuiKey_Enter,false);
    return value;
}
MenuAction GameMenu::Draw(const char* title,const std::vector<MenuEntry>& entries,const char* status,const Detail& detail,int columns,const Card& card,const Body& body,float flyoutScale,float cardHeight,bool stableStatus,Tone statusTone) {
    const bool flyout=flyoutScale>0;
    const float unit=flyout?flyoutScale:Scale();
    columns=(std::max)(1,columns);
    const double now=clock_.Update(ImGui::GetTime());
    // A newly visible component cannot reuse its parent's opening press.
    if(lastFrame_!=ImGui::GetFrameCount()-1) { navigation.NeutralGate(); lastEdit_.clear(); }
    lastFrame_=ImGui::GetFrameCount();
    const bool modalAtStart=navigation.Editing()||navigation.Confirming();
    auto menuInput=ReadMenuInput(); menuInput.time=now;
    // InputText consumes this frame's characters before the requested acceptance.
    // The navigation model still owns the neutral gate and Back/Enter ordering.
    auto action=navigation.Update(menuInput,entries,columns,true,true);
    const bool acceptEditText=action.kind==MenuAction::SubmitText;
    if(acceptEditText)action={};
    feedback_.Update(navigation.Screen(),entries,now);
    bool backRequested=action.kind==MenuAction::Back;
    if(statusProbe)statusProbe(status,statusTone);
    if(entriesProbe)entriesProbe(entries);
    const bool changed=lastScreen_!=navigation.Screen();
    const bool home=std::strcmp(title,"SF4 EMBER")==0;
    // Scaled like every other breakpoint, so a 1.5x interface on a 1024-wide
    // window takes the same layout decision as the header and the galleries.
    const auto windowSize=ImGui::GetWindowSize();const bool roomy=windowSize.x>=1000*Scale()&&windowSize.x/windowSize.y>=1.5f;
    const float homeMargin=roomy?windowSize.x*.10f:20*Scale();
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
    if(home)ImGui::SetCursorPos(ImVec2(homeMargin,roomy?windowSize.y*.10f:16*Scale()));
    if(flyout) {
        ImGui::TextColored(ToneColor(Tone::Pending),"EMBER / %s",title);
        ImGui::Separator();
    } else MenuHeader(title);
    if(home){
        const float cardWidth=roomy?(std::min)(360*Scale(),windowSize.x*.35f):windowSize.x-2*homeMargin;
        const auto origin=ImGui::GetWindowPos();
        DrawPlayerCard(ImVec2(origin.x+(roomy?windowSize.x-homeMargin-cardWidth:homeMargin),origin.y+(roomy?windowSize.y*.10f:ImGui::GetCursorPosY())),cardWidth,!roomy);
        const float start=roomy?(std::max)(ImGui::GetCursorPosY()+36*Scale(),windowSize.y*.39f):ImGui::GetCursorPosY()+110*Scale();
        ImGui::SetCursorPos(ImVec2(homeMargin,start));
    }
    std::string parent=MenuScreenLabel(navigation.Parent());
    if(flyout&&navigation.Parent()=="home")parent=navigation.Screen()=="home"?"SF4":"Training Lab";
    ImGui::BeginDisabled(modalAtStart||navigation.Editing()||navigation.Confirming());
    if(!home&&ImGui::Button(("< Back / "+parent).c_str())) backRequested=true;
    ImGui::EndDisabled();
    if(flyout || stableStatus) {
        // Bound long command errors without displacing the list or its legend.
        // The room header shares its Back row on wide windows; this decision
        // depends only on width, so changing feedback cannot move the controls.
        const bool inlineFeedback=stableStatus && !flyout && ImGui::GetContentRegionAvail().x>=820*unit;
        if(inlineFeedback) ImGui::SameLine();
        // Two reserved lines cost more than a short viewport can spare, and the
        // appearance galleries collapse their artwork to pay for it. Both the
        // width and the height tests read geometry only, never the status text,
        // so the reserved area still cannot move under a highlight or a click.
        const float feedbackLines=ImGui::GetContentRegionAvail().y>=520*unit?2.f:1.f;
        ImGui::BeginChild("Command feedback",ImVec2(0,inlineFeedback?ImGui::GetFrameHeight():ImGui::GetTextLineHeightWithSpacing()*feedbackLines));
        ImGui::PushStyleColor(ImGuiCol_Text,ToneColor(statusTone));
        ImGui::TextWrapped("%s",status);ImGui::PopStyleColor();ImGui::EndChild();
    }
    const auto focusedEntry=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.Focus();});
    MenuEntry presentationEntry;
    if(focusedEntry!=entries.end()) { presentationEntry=*focusedEntry; presentationEntry.enabled=feedback_.Enabled(*focusedEntry); }
    const bool adjustable=focusedEntry!=entries.end()&&presentationEntry.adjustable&&presentationEntry.enabled;
    const char* primary=MenuPrimaryHint(focusedEntry==entries.end()?nullptr:&presentationEntry);
    // Reserve the actual footer items and their spacing, not a guessed margin.
    const float footerSpacing=8*unit+2*ImGui::GetStyle().ItemSpacing.y+
        (home?36*unit+ImGui::GetStyle().ItemSpacing.y:0);
    const float footer=MenuLegend(ImGui::GetContentRegionAvail().x,selectGlyph,backGlyph,false,adjustable,menuArt,unit,primary)+footerSpacing;
    const auto available=ImGui::GetContentRegionAvail();
    const bool wide=available.x>=(flyout?640:820)*unit;
    const bool compactGallery=cardHeight>100&&!wide;
    auto preview=[&] {
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.Focus();});
        if(it!=entries.end()) {
            const bool visualEnabled=feedback_.Enabled(*it);
            ImGui::PushStyleColor(ImGuiCol_Text,ToneColor(visualEnabled?Tone::Neutral:Tone::Pending));
            ImGui::TextWrapped("%s",it->label.c_str());ImGui::PopStyleColor();
            if(!compactGallery)ImGui::TextWrapped("%s",it->detail.c_str());
            if(!it->value.empty())ImGui::TextWrapped("%s",it->value.c_str());
            if(!visualEnabled) ImGui::TextDisabled(feedback_.Pending(*it)?"Updating room...":"Unavailable");
            if(detail) detail(it->id);
        }
    };
    if(body){
        ImGui::BeginDisabled(modalAtStart||navigation.Editing()||navigation.Confirming());
        body(entries,navigation,action,(std::max)(60.f,available.y-footer),feedback_);
        ImGui::EndDisabled();
    }
    else {
    if(!wide&&!home) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.1f,.09f,.08f,.52f));
        ImGui::BeginChild("Menu detail",ImVec2(0,compactGallery?2*ImGui::GetTextLineHeightWithSpacing()+4*unit:
            (std::min)((available.y-footer)*(flyout?.4f:.48f),(flyout?110:145)*unit)));
        preview(); ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    const float listWidth=home?(roomy?windowSize.x*.40f:windowSize.x-2*homeMargin):wide?available.x*.53f:available.x;
    if(changed) ImGui::SetNextWindowScroll(ImVec2(0,navigation.Scroll()));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
    ImGui::BeginChild("Menu list",ImVec2(listWidth,(std::max)(60.f,ImGui::GetContentRegionAvail().y-footer)),0,ImGuiWindowFlags_NoNavInputs);
    const float gap=8*Scale();
    const float rowWidth=(ImGui::GetContentRegionAvail().x-gap*(columns-1))/columns;
    // Tall appearance cards must fit as a whole in the focused scrolling pane,
    // including their saved marker and caption, even at narrow/high DPI sizes.
    const float gridHeight=cardHeight>100?(std::min)(cardHeight*Scale(),ImGui::GetContentRegionAvail().y):cardHeight*Scale();
    for(std::size_t i=0;i<entries.size();++i) {
        const auto& e=entries[i]; const bool focused=e.id==navigation.Focus();
        const bool visualEnabled=feedback_.Enabled(e);
        if(i%columns) ImGui::SameLine(0,gap);
        ImGui::PushID(e.id.c_str());
        const auto start=ImGui::GetCursorScreenPos();
        const float textSize=home?28*Scale():ImGui::GetFontSize();
        const bool valueRow=!card&&(e.adjustable||e.text||!e.value.empty());
        const bool stackedValue=valueRow&&rowWidth<420*unit;
        const float height=columns>1?gridHeight:(std::max)(home?44*Scale():(stackedValue?64:flyout?42:52)*unit,textSize+2*ImGui::GetStyle().FramePadding.y);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,ImVec2(.03f,.5f));
        ImGui::PushStyleColor(ImGuiCol_Button,focused?ImVec4(.47f,.28f,.16f,.6f):ImVec4(.105f,.10f,.095f,home?0.f:.45f));
        ImGui::PushStyleColor(ImGuiCol_Text,visualEnabled?ImVec4(.95f,.92f,.87f,1):ImVec4(.55f,.52f,.48f,1));
        if(home)ImGui::PushFont(HeadingFont());
        const float textWidth=rowWidth-2*ImGui::GetStyle().FramePadding.x;
        const float valueWidth=valueRow?(stackedValue?textWidth:(std::min)(textWidth*.45f,200*unit)):0;
        const float labelWidth=valueRow&&!stackedValue?textWidth-valueWidth-16*unit:textWidth;
        const std::string label=FitLabel(e.label,labelWidth);
        const std::string value=FitLabel(e.value.empty()?"Not set":e.value,(std::max)(1.f,valueWidth-(e.adjustable?52*unit:0)));
        const float valueStart=stackedValue?ImGui::GetStyle().FramePadding.x:rowWidth-ImGui::GetStyle().FramePadding.x-valueWidth;
        if(ImGui::Button("##entry",ImVec2(rowWidth,height))&&!modalAtStart&&!navigation.Editing()&&!navigation.Confirming()) {
            navigation.Focus(e.id,entries); action=navigation.Choose(entries);
            if(e.adjustable&&e.enabled){const float x=ImGui::GetMousePos().x-start.x;
                if(x>=valueStart)action={MenuAction::Adjust,e.id,{},x<valueStart+valueWidth*.5f?-1:1};}
        }
        const bool drawnCard=card&&card(e,start,ImVec2(start.x+rowWidth,start.y+height));
        if(!drawnCard){
            const auto textPosition=ImVec2(start.x+ImGui::GetStyle().FramePadding.x,start.y+(stackedValue?6*unit:(height-ImGui::GetTextLineHeight())*.5f));
            ImGui::GetWindowDrawList()->AddText(textPosition,ImGui::GetColorU32(ImGuiCol_Text),label.c_str());
            if(valueRow){
                const float y=stackedValue?start.y+height-ImGui::GetTextLineHeight()-6*unit:textPosition.y;
                const auto color=visualEnabled?palette::Ivory:palette::Muted;
                const float right=start.x+rowWidth-ImGui::GetStyle().FramePadding.x;
                const float x=e.adjustable?start.x+valueStart+(valueWidth-ImGui::CalcTextSize(value.c_str()).x)*.5f:right-ImGui::CalcTextSize(value.c_str()).x;
                ImGui::GetWindowDrawList()->AddText(ImVec2(x,y),color,value.c_str());
                if(e.adjustable){
                    ImGui::GetWindowDrawList()->AddText(ImVec2(start.x+valueStart,y),visualEnabled?palette::Ember:palette::Muted,"<");
                    ImGui::GetWindowDrawList()->AddText(ImVec2(right-12*unit,y),visualEnabled?palette::Ember:palette::Muted,">");
                }
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
        if(textProbe&&!drawnCard){const auto measured=ImGui::CalcTextSize(label.c_str());textProbe(e.id.c_str(),measured.y,height-2*ImGui::GetStyle().FramePadding.y,measured.x,labelWidth);
            if(valueRow)textProbe((e.id+"-value").c_str(),ImGui::GetTextLineHeight(),height-2*ImGui::GetStyle().FramePadding.y,ImGui::CalcTextSize(value.c_str()).x,valueWidth-(e.adjustable?52*unit:0));}
        if(home)ImGui::PopFont();ImGui::PopStyleColor(2);ImGui::PopStyleVar(3);
        if(cardProbe)cardProbe(e.id.c_str(),start,ImVec2(start.x+rowWidth,start.y+height));
        if(focused) {
            if(columns>1)ImGui::GetWindowDrawList()->AddRect(start,ImVec2(start.x+rowWidth,start.y+height),palette::Ember,0,0,2*Scale());
            else ImGui::GetWindowDrawList()->AddRectFilled(start,ImVec2(start.x+3*Scale(),start.y+height),palette::Ember);
            if(lastFocus_!=e.id||changed) ImGui::SetScrollHereY(.5f);
        }
        ImGui::PopID();
    }
    navigation.Scroll()=ImGui::GetScrollY(); ImGui::EndChild();ImGui::PopStyleColor();
    if(wide&&!home) { ImGui::SameLine(0,20*unit); ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.1f,.09f,.08f,.52f));
        ImGui::BeginChild("Menu detail",ImVec2(0,(std::max)(60.f,available.y-footer))); preview(); ImGui::EndChild(); ImGui::PopStyleColor(); }
    }
    if(!flyout){
        const auto p=ImGui::GetCursorScreenPos();const auto origin=ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(origin.x,p.y),ImVec2(origin.x+windowSize.x,origin.y+windowSize.y),IM_COL32(16,15,14,225));
    }
    if(home){
        ImGui::SetCursorPosX(homeMargin);
        const auto current=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.Focus();});
        if(current!=entries.end()){
            // Home shows the status in place of the focused row's help text, so
            // it must carry the same severity rather than reading as help.
            const bool showStatus=*status&&std::strcmp(status,"Saved");
            const char* text=showStatus?status:current->detail.c_str();
            const auto color=showStatus&&statusTone!=Tone::Neutral?ImGui::ColorConvertFloat4ToU32(ToneColor(statusTone)):palette::Muted;
            const auto p=ImGui::GetCursorScreenPos();ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),12*Scale(),p,color,text,nullptr,ImGui::GetContentRegionAvail().x);
        }
        ImGui::Dummy(ImVec2(0,36*Scale()));ImGui::SetCursorPosX(homeMargin);
    }
    ImGui::Dummy(ImVec2(0,8*unit));if(home)ImGui::SetCursorPosX(homeMargin);
    const float legendHeight=MenuLegend(ImGui::GetContentRegionAvail().x,selectGlyph,backGlyph,true,adjustable,menuArt,unit,primary);
    ImGui::Dummy(ImVec2(0,legendHeight));
    if(flyout) {
        // A local child deliberately replaces the viewport-dimming modal. The
        // navigation model owns confirmation input; outside clicks do nothing.
        if(navigation.Confirming()) {
            const auto pos=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
            const auto padding=ImGui::GetStyle().WindowPadding;
            ImGui::SetCursorScreenPos(ImVec2(pos.x+padding.x,pos.y+padding.y));
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,.8f));
            ImGui::BeginChild("Training veil",ImVec2(size.x-2*padding.x,size.y-2*padding.y),0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
            const ImVec2 dialogSize(size.x-32*unit,(std::min)(270*unit,size.y-32*unit));
            ImGui::SetCursorScreenPos(ImVec2(pos.x+(size.x-dialogSize.x)*.5f,pos.y+(size.y-dialogSize.y)*.5f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.13f,.115f,.1f,1));
            ImGui::BeginChild("Training confirmation",dialogSize,ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
            const auto entry=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.DialogId();});
            ImGui::BeginChild("Confirmation explanation",ImVec2(0,ImGui::GetContentRegionAvail().y-60*unit));
            ImGui::TextWrapped("%s?",entry==entries.end()?"Confirm action":entry->label.c_str());
            if(entry!=entries.end())ImGui::TextWrapped("%s",entry->detail.c_str());
            ImGui::EndChild();
            const float width=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;
            for(int i=0;i<2;++i) {
                if(i)ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,navigation.ConfirmSelected()==bool(i)?ImVec4(.5f,.25f,.1f,1):ImVec4(.2f,.18f,.16f,1));
                ImGui::BeginDisabled(i!=0&&(entry==entries.end()||!entry->enabled));
                if(ImGui::Button(i?"Confirm":"Cancel",ImVec2(width,42*unit)))action=navigation.Confirm(i!=0,entries);
                ImGui::EndDisabled();
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();ImGui::PopStyleColor();
            ImGui::EndChild();ImGui::PopStyleColor();
        }
        lastFocus_=navigation.Focus();lastScreen_=navigation.Screen();
        if(backRequested) return navigation.Return();
        return action;
    }
    // A renderer popup may survive a model cancellation by one frame. Close it
    // without rendering stale controls or reading a draft that was already cleared.
    const auto popupSize=[&] {
        const auto viewport=ImGui::GetMainViewport()->Size;
        const float width=(std::max)(1.f,viewport.x-40*Scale());
        const float height=(std::max)(1.f,viewport.y-40*Scale());
        ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(width,height));
        ImGui::SetNextWindowSize(ImVec2((std::min)(600*Scale(),width),0));
    };
    if(navigation.Confirming()) ImGui::OpenPopup("Confirm action");
    popupSize();
    if(ImGui::BeginPopupModal("Confirm action",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNavInputs)) {
        if(!navigation.Confirming()) ImGui::CloseCurrentPopup();
        else {
            const auto entry=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.DialogId();});
            const bool canConfirm=entry!=entries.end()&&entry->enabled;
            ImGui::TextWrapped("%s?",entry==entries.end()?"Confirm action":entry->label.c_str());
            if(entry!=entries.end())ImGui::TextWrapped("%s",entry->detail.c_str());
            const bool visualConfirm=entry!=entries.end()&&feedback_.Enabled(*entry);
            ImGui::BeginChild("Confirmation feedback",ImVec2(0,2*ImGui::GetTextLineHeightWithSpacing()));
            if(!visualConfirm)ImGui::TextWrapped("Updating room. Cancel is still available.");
            ImGui::EndChild();
            const float buttonWidth=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;
            ImGui::PushStyleColor(ImGuiCol_Button,!navigation.ConfirmSelected()?ImVec4(.5f,.25f,.1f,1):ImVec4(.15f,.14f,.13f,1));
            if(ImGui::Button("Cancel",ImVec2(buttonWidth,48*Scale()))) action=navigation.Confirm(false,entries);
            ImGui::PopStyleColor(); ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,visualConfirm&&navigation.ConfirmSelected()?ImVec4(.5f,.25f,.1f,1):ImVec4(.15f,.14f,.13f,1));
            const std::string label=FitLabel(entry==entries.end()?"Confirm":entry->label,buttonWidth-2*ImGui::GetStyle().FramePadding.x);
            ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.f);
            ImGui::BeginDisabled(!canConfirm);
            if(ImGui::Button((label+"###Confirm").c_str(),ImVec2(buttonWidth,48*Scale()))) action=navigation.Confirm(true,entries);
            ImGui::EndDisabled(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
            if(!navigation.Confirming()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if(navigation.Editing()) ImGui::OpenPopup("Edit text");
    popupSize();
    if(ImGui::BeginPopupModal("Edit text",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNavInputs)) {
        if(!navigation.Editing()) ImGui::CloseCurrentPopup();
        else {
            const auto entry=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==navigation.EditingId();});
            const std::size_t limit=entry==entries.end()?4096:(std::min)(std::size_t(4096),entry->textLimit);
            const bool canAccept=entry!=entries.end()&&entry->enabled;
            ImGui::TextWrapped("%s",entry==entries.end()?"Edit text":entry->label.c_str());
            ImGui::TextWrapped("Type or paste. Enter accepts; Escape or controller Back cancels.");
            char draft[4097]={}; std::strncpy(draft,navigation.Draft().c_str(),sizeof(draft)-1);
            if(lastEdit_!=navigation.EditingId()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if(ImGui::InputText("##Draft",draft,limit+1)) navigation.Draft(draft);
            ImGui::TextDisabled("%u / %u bytes",static_cast<unsigned>(navigation.Draft().size()),static_cast<unsigned>(limit));
            const bool visualAccept=entry!=entries.end()&&feedback_.Enabled(*entry);
            ImGui::BeginChild("Edit feedback",ImVec2(0,2*ImGui::GetTextLineHeightWithSpacing()));
            if(!visualAccept)ImGui::TextWrapped("Updating room. Your draft is kept; you can continue typing or cancel.");
            ImGui::EndChild();
            ImGui::PushStyleColor(ImGuiCol_Text,visualAccept?ImVec4(.95f,.92f,.87f,1):ImVec4(.55f,.52f,.48f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.f);
            ImGui::BeginDisabled(!canAccept);
            const bool acceptClicked=ImGui::Button("Accept");
            ImGui::EndDisabled(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
            if(canAccept&&(acceptClicked||acceptEditText)) action=navigation.AcceptText(entries);
            ImGui::SameLine(); if(ImGui::Button("Cancel edit")) navigation.Cancel();
            if(!navigation.Editing()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    lastEdit_=navigation.EditingId();
    lastFocus_=navigation.Focus(); lastScreen_=navigation.Screen();
    if(backRequested) return navigation.Return();
    return action;
}
} }
