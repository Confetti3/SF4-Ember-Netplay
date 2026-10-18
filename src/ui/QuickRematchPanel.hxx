#pragma once
#include "../common/QuickRematch.hxx"
#include "../common/Localization.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
// Presentation only: it never reads or changes the native battle. The caller
// sends the returned choice together with the offer of the same snapshot.
class QuickRematchPanel {
public:
    quickrematch::Choice Draw(const quickrematch::View& view) {
        using quickrematch::Choice;
        if (!quickrematch::Shown(view)) { Clear(); return Choice::None; }
        if (offer_ != view.offer) {
            Clear(); offer_ = view.offer;
            menu_.navigation.Prefer("quick-rematch");
        }
        const char* back = loc::T("quick_rematch.return_lobby");
        std::vector<MenuEntry> entries={
            Row("quick-rematch",loc::T("quick_rematch.rematch"),loc::T("quick_rematch.rematch_detail"),view.canRematch&&!view.accepted&&!rematchSent_&&!lobbySent_),
            Row("quick-lobby",back,loc::T("quick_rematch.return_lobby_detail"),!lobbySent_)};
        const std::string status=queueFailed_?loc::T("error.queue_failed"):!view.status.empty()?view.status:
            (view.accepted||rematchSent_||lobbySent_)?loc::T("quick_rematch.waiting"):std::string();
        const float scale=Scale();const auto* viewport=ImGui::GetMainViewport();
        const ImVec2 size((std::min)(760*scale,viewport->Size.x*.94f),(std::min)(500*scale,viewport->Size.y*.88f));
        ImGui::SetNextWindowSize(size);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x+viewport->Size.x*.5f,viewport->Pos.y+viewport->Size.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
        ImGui::SetNextWindowBgAlpha(.97f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20*scale,16*scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,10*scale);
        ImGui::Begin("###EmberQuickRematch",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoNavInputs);
        menu_.backHint=back;menu_.rootName=loc::T("quick_rematch.title");
        menu_.cardRounding=6;menu_.compactDetailLines=2;
        const auto action=menu_.Draw(loc::T("quick_rematch.title"),entries,status.c_str(),{},1,{}, {},scale,64,true,
            queueFailed_?Tone::Error:(view.accepted||rematchSent_||lobbySent_)?Tone::Pending:Tone::Neutral);
        ImGui::End();ImGui::PopStyleVar(2);
        Choice choice=Choice::None;
        if(action.kind==MenuAction::Activate&&action.id=="quick-rematch"&&entries[0].enabled)choice=Choice::Rematch;
        else if(((action.kind==MenuAction::Activate&&action.id=="quick-lobby")||action.kind==MenuAction::Close)&&entries[1].enabled)choice=Choice::Lobby;
        if(choice==Choice::Rematch)rematchSent_=true;
        if(choice==Choice::Lobby)lobbySent_=true;
        if(choice!=Choice::None){queueFailed_=false;menu_.navigation.NeutralGate();}
        return choice;
    }
    // A rejected enqueue is retryable; a submitted choice is not repeated
    // while a later snapshot has yet to acknowledge it.
    void QueueFailed() { rematchSent_=lobbySent_=false;queueFailed_=true;menu_.navigation.NeutralGate(); }
    void Clear() {
        offer_=0;rematchSent_=lobbySent_=queueFailed_=false;
        menu_.navigation.Home();menu_.navigation.NeutralGate();
    }
private:

    GameMenu menu_;
    std::uint64_t offer_=0;
    bool rematchSent_=false,lobbySent_=false,queueFailed_=false;
};
} }
