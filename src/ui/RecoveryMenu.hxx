#pragma once
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
enum class RecoveryChoice { None, Folder, Retry, CheckUpdates, Install, Cancel, Close, Channel };
// The root screen names the header's "< Back / ..." breadcrumb (MenuScreenLabel).
inline MenuNavigation RecoveryNavigation(bool updates) { return MenuNavigation(updates?"updates":"recovery"); }
// Each newly found update takes the highlight once, so Select installs it
// rather than checking again; offered remembers the version that already did.
inline void OfferFoundUpdate(GameMenu& menu,const platform::ServiceSnapshot& state,std::string& offered) {
    if(!state.update.ok||!state.update.updateAvailable||state.pending||state.update.latestVersion==offered)return;
    offered=state.update.latestVersion;menu.navigation.Prefer("install");
}
// Rendering returns intent only. The launcher owns dialogs, services and exit.
// messageTone is the launcher message's own severity: a selected folder is
// good news, a launch failure is not. The status shows one of two messages: the
// launcher's (why recovery opened, the folder just picked) or the latest
// service result (an update check, a download). serviceNewer says which
// happened last, so a finished check does not bury a folder that was rejected
// afterwards, nor the failure that opened this window. With no launcher message
// the service result always shows.
// canStart adds a way into the game to the updater, which otherwise only
// closes: it answers RecoveryChoice::Retry like the launch window's Retry.
// The updater also shows the channel its checks use (Select on that row asks
// for the next one, RecoveryChoice::Channel) and the installed version, so an
// offered version can be read against it.
inline RecoveryChoice DrawRecoveryMenu(GameMenu& menu,const platform::ServiceSnapshot& state,const std::string& message,bool updates,
    Tone messageTone=Tone::Error,bool canStart=false,bool serviceNewer=true) {
    std::vector<MenuEntry> rows;
    if(!updates){
        rows.push_back(Row("folder",loc::T("recovery.choose_folder"),loc::T("recovery.choose_folder_detail"),!state.pending));
        rows.push_back(Row("retry",loc::T("recovery.retry"),state.pending?loc::T("recovery.retry_busy"):loc::T("recovery.retry_detail"),!state.pending));
    }
    // A found update comes first, named by its version: installing it is
    // what the player came here for.
    if(state.update.ok&&state.update.updateAvailable){
        rows.push_back(ConfirmRow("install",loc::T("updates.install"),state.update.expectedSha256.size()==64?
            loc::T("updates.install_detail"):loc::T("updates.unverified"),!state.pending&&state.update.expectedSha256.size()==64));
        rows.back().value=state.update.latestVersion;
    }
    rows.push_back(Row("check",loc::T("updates.check"),state.pending?loc::T("updates.busy"):loc::T("updates.check_detail"),!state.pending));
    if(updates){
        const auto& channel=launcher::GetUpdateChannelInfo(state.channel);
        rows.push_back(Row("channel",loc::T("updates.channel"),loc::T(channel.detailKey),!state.pending));
        rows.back().value=loc::T(channel.labelKey);
        if(!state.installedVersion.empty())rows.push_back(InfoRow("installed",loc::T("updates.installed_version"),state.installedVersion,loc::T("updates.installed_version_detail")));
    }
    if(state.pending)rows.push_back(Row("cancel",loc::T("updates.cancel"),loc::T("updates.cancel_detail")));
    if(updates&&canStart)
        rows.push_back(Row("retry",loc::T("updates.start_game"),state.pending?loc::T("recovery.retry_busy"):loc::T("updates.start_game_detail"),!state.pending));
    rows.push_back(Row("close",loc::T("common.close"),state.pending?loc::T("recovery.close_cancels_detail"):
        loc::T(updates?"updates.close_detail":"recovery.close_detail")));
    // Back on the root closes the window, so the legend says so.
    menu.backHint=updates?loc::T("updates.back_close"):loc::T("recovery.back_close");
    const auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
    const std::string windowName=std::string(loc::T("recovery.window"))+"###EmberRecovery";
    ImGui::Begin(windowName.c_str(),nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
    const bool service=!state.message.empty()&&(serviceNewer||message.empty());
    const std::string& status=service?state.message:message;
    const Tone tone=service?(state.pending?Tone::Pending:state.succeeded?Tone::Success:Tone::Error):
        !message.empty()?messageTone:state.pending?Tone::Pending:Tone::Neutral;
    // stableStatus: GameMenu only draws the status line for a flyout or a
    // stable-status screen, which recovery is not. fitStatus: launcher messages
    // run to several lines and name the files to move and the folders to look in.
    menu.fitStatus=true;
    const auto action=menu.Draw(updates?loc::T("updates.title"):loc::T("recovery.title"),rows,status.c_str(),[&](const std::string&){
        if(!state.pending||!state.downloadedBytes)return;
        if(state.totalBytes)ImGui::ProgressBar((std::min)(1.f,float(state.downloadedBytes)/state.totalBytes),ImVec2(-1,0));
        const auto progress=loc::Tf("updates.downloaded_mb",state.downloadedBytes/1048576.0);
        ImGui::TextWrapped("%s",progress.c_str());
    },1,{},{},0,100,true,tone);
    ImGui::End();
    if(action.kind==MenuAction::Close)return RecoveryChoice::Close;
    if(action.kind!=MenuAction::Activate)return RecoveryChoice::None;
    if(action.id=="folder")return RecoveryChoice::Folder;
    if(action.id=="retry")return RecoveryChoice::Retry;
    if(action.id=="check")return RecoveryChoice::CheckUpdates;
    if(action.id=="install")return RecoveryChoice::Install;
    if(action.id=="channel")return RecoveryChoice::Channel;
    if(action.id=="cancel")return RecoveryChoice::Cancel;
    if(action.id=="close")return RecoveryChoice::Close;
    return RecoveryChoice::None;
}
} }
