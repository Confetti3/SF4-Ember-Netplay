#pragma once
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "UpdateChannelPick.hxx"
#include "VersionLine.hxx"
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
inline const char* UpdateChannelLabel(launcher::UpdateChannel channel) { return loc::T(launcher::GetUpdateChannelInfo(channel).labelKey); }
// What switching the update channel from `from` to `to` means, for the row's
// pane and its confirm: the switch, the channel's own warning, and on a move to
// an older channel that going back may install an older version.
inline std::string ChannelSwitchDetail(launcher::UpdateChannel from,launcher::UpdateChannel to) {
    std::string text=loc::Tf("updates.channel.switch_detail",UpdateChannelLabel(from),UpdateChannelLabel(to));
    text+="\n\n";text+=loc::T(launcher::GetUpdateChannelInfo(to).detailKey);
    if(ClassifyChannelMove(from,to)==ChannelMove::GoesBack)text+="\n\n"+loc::Tf("updates.channel.going_back",UpdateChannelLabel(from),UpdateChannelLabel(to));
    return text;
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
// The updater also shows the channel its checks use and the installed
// version, so an offered version can be read against it. With channelPick,
// Left and Right on the channel row pick a channel and Select asks to switch
// to it (RecoveryChoice::Channel, the pick in channelPick->picked); without
// one the row only informs. The release notes of the offered version open in
// a reader from the "What's new" row.
inline RecoveryChoice DrawRecoveryMenu(GameMenu& menu,const platform::ServiceSnapshot& state,const std::string& message,bool updates,
    Tone messageTone=Tone::Error,bool canStart=false,bool serviceNewer=true,ChannelPick* channelPick=nullptr) {
    if(channelPick)channelPick->Settle(state.channel,state.pending);
    std::vector<MenuEntry> rows;
    // A row that waits while the worker is busy: the status line and the bar
    // say with what, so its pane does not add "Unavailable".
    const auto waits=[&](MenuEntry row){row.quiet=state.pending;return row;};
    if(!updates){
        rows.push_back(waits(Row("folder",loc::T("recovery.choose_folder"),loc::T("recovery.choose_folder_detail"),!state.pending)));
        rows.push_back(waits(Row("retry",loc::T("recovery.retry"),state.pending?loc::T("recovery.retry_busy"):loc::T("recovery.retry_detail"),!state.pending)));
    }
    // A found update comes first, named by its version: installing it is
    // what the player came here for.
    if(state.update.ok&&state.update.updateAvailable){
        const bool verified=state.update.expectedSha256.size()==64;
        std::string detail=verified?loc::T("updates.install_detail"):loc::T("updates.unverified");
        // Going back: the offer is older than what is installed.
        if(state.update.goesBack)detail=std::string(loc::T("updates.install_going_back"))+"\n\n"+detail;
        rows.push_back(waits(ConfirmRow("install",loc::T("updates.install"),detail,!state.pending&&verified)));
        rows.back().value=state.update.latestVersion;
    }
    // The offered (or current) release's notes, as plain text in a reader the
    // pad and keys scroll. They are GitHub text: only ever drawn as text.
    if(updates&&state.update.ok){
        // Plain text already, made once as the check read them.
        const auto& notes=state.update.releaseNotes;
        if(!notes.empty()){
            auto row=Row("notes",loc::T("updates.whats_new"),loc::Tf("updates.whats_new_detail",state.update.latestVersion)+"\n\n"+notes);
            row.reading=true;row.detailText=DetailText::Chat;rows.push_back(std::move(row));
        }
    }
    rows.push_back(waits(Row("check",loc::T("updates.check"),state.pending?loc::T("updates.busy"):loc::T("updates.check_detail"),!state.pending)));
    if(updates){
        const auto shown=channelPick?channelPick->Shown(state.channel):state.channel;
        const bool switching=shown!=state.channel;
        auto row=Value("channel",loc::T("updates.channel"),UpdateChannelLabel(shown),
            switching?ChannelSwitchDetail(state.channel,shown):std::string(loc::T(launcher::GetUpdateChannelInfo(state.channel).detailKey)),!state.pending);
        // Left and Right pick; Select on a pick other than the saved channel
        // opens its confirm, which repeats this row's label.
        if(!channelPick){row.adjustable=false;row.info=true;}
        else if(switching){row.opens=true;row.confirm=true;}
        rows.push_back(waits(std::move(row)));
        if(!state.installedVersion.empty())rows.push_back(InfoRow("installed",loc::T("updates.installed_version"),state.installedVersion,loc::T("updates.installed_version_detail")));
    }
    if(state.pending)rows.push_back(Row("cancel",loc::T("updates.cancel"),loc::T("updates.cancel_detail")));
    if(updates&&canStart)
        rows.push_back(waits(Row("retry",loc::T("updates.start_game"),state.pending?loc::T("recovery.retry_busy"):loc::T("updates.start_game_detail"),!state.pending)));
    rows.push_back(Row("close",loc::T("common.close"),state.pending?loc::T("recovery.close_cancels_detail"):
        loc::T(updates?"updates.close_detail":"recovery.close_detail")));
    // The launch window names the version in its corner, as Home does; the
    // updater has its own rows for it.
    menu.footerNote=updates?std::string():VersionLine(state.installedVersion,state.channel);
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
        if(!state.pending)return;
        // An installing update with no total (extracting, or a step just begun) gets Dear ImGui's moving bar.
        if(state.stageTotal)ImGui::ProgressBar((std::min)(1.f,float(state.stageDone)/state.stageTotal),ImVec2(-1,0));
        else if(state.lastAction==platform::ServiceAction::InstallUpdate)ImGui::ProgressBar(-1.f*float(ImGui::GetTime()),ImVec2(-1,0));
        if(state.updateStage!=launcher::UpdateStage::Downloading||!state.stageDone)return;
        const auto progress=loc::Tf("updates.downloaded_mb",state.stageDone/1048576.0);
        ImGui::TextWrapped("%s",progress.c_str());
    },1,{},{},0,100,true,tone);
    ImGui::End();
    if(action.kind==MenuAction::Close)return RecoveryChoice::Close;
    if(action.kind==MenuAction::Adjust&&action.id=="channel"&&channelPick&&!state.pending)channelPick->Step(state.channel,action.delta);
    if(action.kind!=MenuAction::Activate)return RecoveryChoice::None;
    if(action.id=="folder")return RecoveryChoice::Folder;
    if(action.id=="retry")return RecoveryChoice::Retry;
    if(action.id=="check")return RecoveryChoice::CheckUpdates;
    if(action.id=="install")return RecoveryChoice::Install;
    if(action.id=="channel")return channelPick&&channelPick->Pending(state.channel)?RecoveryChoice::Channel:RecoveryChoice::None;
    if(action.id=="cancel")return RecoveryChoice::Cancel;
    if(action.id=="close")return RecoveryChoice::Close;
    return RecoveryChoice::None;
}
} }
