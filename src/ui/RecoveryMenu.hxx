#pragma once
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
enum class RecoveryChoice { None, Folder, Retry, CheckUpdates, Install, Cancel, Close };
// Rendering returns intent only. The launcher owns dialogs, services and exit.
inline RecoveryChoice DrawRecoveryMenu(GameMenu& menu,const platform::ServiceSnapshot& state,const std::string& message,bool updates) {
    std::vector<MenuEntry> rows;
    if(!updates){
        rows.push_back(Row("folder",loc::T("recovery.choose_folder"),loc::T("recovery.choose_folder_detail"),!state.pending));
        rows.push_back(Row("retry",loc::T("recovery.retry"),state.pending?loc::T("recovery.retry_busy"):loc::T("recovery.retry_detail"),!state.pending));
    }
    rows.push_back(Row("check",loc::T("updates.check"),state.pending?loc::T("updates.busy"):loc::T("updates.check_detail"),!state.pending));
    if(state.update.ok&&state.update.updateAvailable)
        rows.push_back(ConfirmRow("install",loc::T("updates.install"),state.update.expectedSha256.size()==64?
            loc::T("updates.install_detail"):loc::T("updates.unverified"),!state.pending&&state.update.expectedSha256.size()==64));
    if(state.pending)rows.push_back(Row("cancel",loc::T("updates.cancel"),loc::T("updates.cancel_detail")));
    rows.push_back(Row("close",loc::T("common.close"),loc::T("recovery.close_detail")));
    const auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
    const std::string windowName=std::string(loc::T("recovery.window"))+"###EmberRecovery";
    ImGui::Begin(windowName.c_str(),nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
    const std::string status=state.message.empty()?message:state.message;
    // stableStatus: GameMenu only draws the status line for a flyout or a
    // stable-status screen. Recovery is neither, so its launch and update
    // messages were never rendered.
    const auto action=menu.Draw(updates?loc::T("updates.title"):loc::T("recovery.title"),rows,status.c_str(),[&](const std::string&){
        if(!state.pending||!state.downloadedBytes)return;
        if(state.totalBytes)ImGui::ProgressBar((std::min)(1.f,float(state.downloadedBytes)/state.totalBytes),ImVec2(-1,0));
        const auto progress=loc::Tf("updates.downloaded_mb",state.downloadedBytes/1048576.0);
        ImGui::TextWrapped("%s",progress.c_str());
    },1,{},{},0,100,true,state.pending?Tone::Pending:message.empty()&&state.message.empty()?Tone::Neutral:Tone::Error);
    ImGui::End();
    if(action.kind==MenuAction::Close)return RecoveryChoice::Close;
    if(action.kind!=MenuAction::Activate)return RecoveryChoice::None;
    if(action.id=="folder")return RecoveryChoice::Folder;
    if(action.id=="retry")return RecoveryChoice::Retry;
    if(action.id=="check")return RecoveryChoice::CheckUpdates;
    if(action.id=="install")return RecoveryChoice::Install;
    if(action.id=="cancel")return RecoveryChoice::Cancel;
    if(action.id=="close")return RecoveryChoice::Close;
    return RecoveryChoice::None;
}
} }
