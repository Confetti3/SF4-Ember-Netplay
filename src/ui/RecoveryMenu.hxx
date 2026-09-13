#pragma once
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../platform/ApplicationServices.hxx"
#include <cstdio>

namespace sf4e { namespace ui {
enum class RecoveryChoice { None, Folder, Retry, CheckUpdates, Install, Cancel, Close };
// Rendering returns intent only. The launcher owns dialogs, services and exit.
inline RecoveryChoice DrawRecoveryMenu(GameMenu& menu,const platform::ServiceSnapshot& state,const std::string& message,bool updates) {
    std::vector<MenuEntry> rows;
    if(!updates){
        rows.push_back(Row("folder","Choose game folder","Select the installed Ultra Street Fighter IV folder containing SSFIV.exe. Game files are not included in this package.",!state.pending));
        rows.push_back(Row("retry","Retry launch",state.pending?"Cancel or finish the current operation first.":"Retry with the selected game folder. A missing networking helper does not prevent offline play.",!state.pending));
    }
    rows.push_back(Row("check","Check for updates",state.pending?"An operation is already running.":"Check for a newer Ember package.",!state.pending));
    if(state.update.ok&&state.update.updateAvailable)
        rows.push_back(ConfirmRow("install","Download and install",state.update.expectedSha256.size()==64?
            "Close SF4 before installing. The package is verified before installation. Your personal settings are preserved.":
            "This update has no verification checksum and cannot be installed.",!state.pending&&state.update.expectedSha256.size()==64));
    if(state.pending)rows.push_back(Row("cancel","Cancel operation","Cancel the current download or check. Wait for cancellation to finish before retrying."));
    rows.push_back(Row("close","Close","Close recovery without starting SF4. Any running operation is cancelled."));
    const auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("Ember recovery",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
    const std::string status=state.message.empty()?message:state.message;
    const auto action=menu.Draw(updates?"UPDATES":"LAUNCH RECOVERY",rows,status.c_str(),[&](const std::string&){
        if(!state.pending||!state.downloadedBytes)return;
        if(state.totalBytes)ImGui::ProgressBar((std::min)(1.f,float(state.downloadedBytes)/state.totalBytes),ImVec2(-1,0));
        char progress[80];std::snprintf(progress,sizeof(progress),"Downloaded %.1f MB",state.downloadedBytes/1048576.0);
        ImGui::TextWrapped("%s",progress);
    });
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
