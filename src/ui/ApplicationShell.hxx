#pragma once
#include <imgui.h>
#include "RoomCards.hxx"
#include "../common/ReplayInputDetails.hxx"
#include "../common/ReplayRequest.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/TrainingEntry.hxx"
#include "../common/GameDisplayConfig.hxx"
#include "../discord/Presence.hxx"
#include "../netplay/InputAssignment.hxx"
#include "../netplay/TableSetup.hxx"
#include "../netplay/SessionController.hxx"
#include "../netplay/MatchEndRules.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../platform/Speech.hxx"
#include "../netplay/MemberView.hxx"
#include "../netplay/IdentityView.hxx"
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/Answered.hxx"
#include "../netplay/Levels.hxx"
#include "../netplay/Rankings.hxx"
#include "../netplay/TournamentStatus.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../session/RoomFollow.hxx"
#include "../session/RoomModel.hxx"
#include "../session/MemberLatency.hxx"
#include "UpdateChannelPick.hxx"
#include "RoomBoardArea.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include "ChatTranscript.hxx"
#include "GameMenu.hxx"
#include "PostMatchPrompt.hxx"
#include "../common/ControllerView.hxx"
#include "../common/ControllerRequest.hxx"
#include "../common/ButtonLayoutCapture.hxx"
#include "IdentityPanel.hxx"
#include "PublicRoomsPanel.hxx"
#include "RankingsPanel.hxx"
#include "ProfilePanel.hxx"
#include "FriendsPanel.hxx"
#include "ToastAction.hxx"
#include "ReplaysPanel.hxx"
#include "ProblemReportPanel.hxx"
#include "UpdatePrompt.hxx"
#include "CountryPicker.hxx"
#include "ScreenPanel.hxx"
#include "ShellAction.hxx"
#include "ShellView.hxx"

namespace sf4e { namespace ui {

// A member's idle seconds `elapsedMs` after the room stamped them. A member
// in a game is not idle, so theirs stay as sent; none counts past
// room::MaximumIdleSeconds.
inline std::uint32_t RoomIdleSeconds(const room::Member& member, std::uint64_t elapsedMs) {
    if (member.status == room::MemberStatus::Playing) return member.idleSeconds;
    const auto since = static_cast<std::uint32_t>((std::min<std::uint64_t>)(elapsedMs / 1000, room::MaximumIdleSeconds));
    return (std::min)(member.idleSeconds + since, room::MaximumIdleSeconds);
}
// Where the fighter select opens from a room: on the roster, or on the Ultra
// for a winner who keeps the fighter (room::KeepsFighter), whose roster is locked.
inline SelectionStart RoomSelectionStart(const room::Snapshot& room) {
    return room::KeepsFighter(room, room.localMember) ? SelectionStart::LookUltra : SelectionStart::Roster;
}
// A member's "is typing" `elapsedMs` after the room stamped it; gone once it ran out.
inline std::uint32_t RoomTypingMs(const room::Member& member, std::uint64_t elapsedMs) {
    return elapsedMs < member.typingMs ? static_cast<std::uint32_t>(member.typingMs - elapsedMs) : 0;
}
// A countdown the room stamped (a table's holdRemainingMs or readyRemainingMs)
// `elapsedMs` later. None sent stays none; one that has run out still holds
// until the room says otherwise, so it keeps its last millisecond.
inline std::uint32_t RoomRemainingMs(std::uint32_t remainingMs, std::uint64_t elapsedMs) {
    if (!remainingMs) return 0;
    return elapsedMs < remainingMs ? static_cast<std::uint32_t>(remainingMs - elapsedMs) : 1;
}

// The room a drawn frame shows, kept from one frame to the next so that a
// room that has not changed is not copied again every frame. The runtime
// publishes one immutable room per change (RuntimeSnapshot::room) and shares
// it until the next, so the published pointer itself says whether the room
// changed; it is held here, so no later room can take its address. Lend moves
// the kept copy into the view, copying the published room over it first only
// when it is another; Return takes it back once the frame is drawn. Null is
// the empty room.
class RoomCarry {
public:
    void Lend(ShellView& view, const std::shared_ptr<const room::Snapshot>& room) {
        if (lent_ || !filled_ || room != source_) {
            kept_ = room ? *room : room::Snapshot();
            source_ = room; filled_ = true; ++copies_;
        }
        view.room = std::move(kept_);
        lent_ = true;
    }
    void Return(ShellView& view) { kept_ = std::move(view.room); lent_ = false; }
    // How many times Lend copied a room (for tests).
    std::uint64_t Copies() const { return copies_; }
private:
    std::shared_ptr<const room::Snapshot> source_;
    room::Snapshot kept_;
    std::uint64_t copies_ = 0;
    bool lent_ = false, filled_ = false;
};

// "Ready in N s" for a player called out of Training: ShellView::trainingReadySeconds, in words.
std::string TrainingReadyText(int seconds);

// Why the room chat can or cannot send now (ChatGateOf). Only Writable lets a message go; Recovering and
// CatchingUp hold one the player entered (queuedChat_) until the room can take it. ReadOnly is a
// tournament match's watcher: it reads the chat and has no box to write in.
enum class ChatGate { NotJoined, Recovering, CatchingUp, Writable, ReadOnly };

// One frame of the room board (RoomPanelBoard.hxx).
struct RoomBoardFrame;

class ApplicationShell {
public:
    using Submit = std::function<bool(ShellAction)>;
    using DrawSelection = std::function<void()>;
    void Draw(const ShellView& view, bool* open, const Submit& submit, const DrawSelection& selection,
              const DrawSelection& developer = {});
    void ShowPlay() {
        // Reopening after battle must retain the active room's navigation.
        if(previousRoomState_==netplay::RoomState::Idle) {
            if(reopenScreen_)menu_.navigation.Push(*reopenScreen_);
            else menu_.navigation.Home();
        }
    }
    // The game's Play offline > Training: Ember's Training select.
    void ShowTraining() { menu_.navigation.Home(); menu_.navigation.Push("training"); }
    // Fighter select is open from Training select's Dummy row: the overlay
    // draws the dummy's pick in it.
    bool SelectingDummy() const { return selectionForDummy_ && menu_.navigation.Screen() == "selection" && menu_.navigation.Parent() == "training"; }
    MenuNavigation& Navigation() { return menu_.navigation; }
    // A text field is being edited, on the menu or the problem report's own panel.
    bool TextEntryOpen() const { return menu_.navigation.Editing() || (menu_.navigation.Screen() == "report" && reportPanel_.Navigation().Editing()); }
    // A notice from outside the shell (the lab's toasts): the status line's
    // for `seconds`, and a toast.
    // A kind and up to two actions go on its toast.
    void Say(std::string text, Tone tone, double seconds, ToastKind kind = ToastKind::None, std::vector<ToastAction> actions = {}) {
        noticeExtra_ = {text, kind, std::move(actions)}; notice_ = std::move(text); noticeTone_ = tone; noticeUntil_ = ImGui::GetTime() + seconds;
    }
    // The game's battle log, opened from Replays, is back at the main menu: Replays again.
    void ShowReplays() { ReplaysPanel::Show(menu_.navigation); }
    void ShowPublicRooms() { auto& nav = menu_.navigation; nav.Cancel(); nav.Home(); nav.Push("online"); nav.Push("public-rooms"); }
    // Public rooms, where the panel judges a room link it was given, with the
    // trusted services probed afresh first.
    void ShowRoomLink() {
        identity_.Probe();
        if (menu_.navigation.Screen() != "public-rooms") ShowPublicRooms();
        error_.clear();
    }
    // What the screen on show needs the replay workers to read (ReplaysPanel::Wanted).
    ReplaysPanel::Wants ReplayWants() const { return replays_.Wanted(menu_.navigation.Screen()); }
    // The replay file the Inputs and stats screen wants read, as its row named it.
    const std::string& ReplayInputsFile() const { return replays_.InputsFile(); }
    std::uint64_t ReplayInputsRevision() const { return replays_.InputsRevision(); }
    // A modal notice is open, and whether it reads as an error (for tests).
    bool NoticeOpen() const { return menu_.NoticeOpen(); }
    // The last Draw drew the room board, whose ping the runtime asks for only while it is drawn.
    bool BoardDrawn() const { return boardDrawn_; }
    // The last Draw drew the fighter selector.
    bool SelectionDrawn() const { return selectionDrawn_; }
    // The last Draw had the player writing a chat draft, which the runtime tells the room.
    bool ChatTyping() const { return chatTyping_; }
    // The line above the chat box: who else is typing, empty when nobody is.
    std::string ChatTypingText(const ShellView& v) const;
    bool NoticeError() const { return menu_.NoticeError(); }
    // What this client keeps of the room chat (for tests).
    const ChatTranscript& Transcript() const { return transcript_; }
    // The room chat's unread count, as the room screen's Chat row shows it.
    // Kept while the shell is hidden (Background), so the training HUD can say it.
    unsigned UnreadChat() const { return transcript_.Unread(muted_); }
    // The shell is not being drawn (the overlay is hidden): nothing typed into
    // a passphrase field may wait in it until it next opens.
    void Conceal() {
        identity_.Conceal();
        publicRooms_.Conceal();
        rankings_.Conceal();
        profile_.Conceal();
        friends_.Conceal();
        // A link that was going on by itself starts again when Ember reopens,
        // with the services read again (identity_.Conceal dropped that read).
        roomLinkOpening_ = roomLinkOpening_ || publicRooms_.FollowingLink();
        publicBridgeAsked_ = false;
        if (menu_.navigation.EditingSecret()) menu_.navigation.Cancel();
    }
    // Every frame Ember is hidden: Conceal, then the identity requests that
    // must still finish, the room's following and the post-match prompt's
    // count of WINs, with only the session, identity, room notices, room,
    // watched and fought games and WIN of `view` read, and `room` taken into
    // the chat transcript as a drawn frame takes it.
    void Background(const ShellView& view, const room::Snapshot& room, const Submit& submit);
    // Where the language preference is written; the platform store unless a
    // test supplies its own to fail it.
    using LanguageSaver = std::function<bool(const std::string& preference, std::string& diagnostic)>;
    void SetLanguageSaver(LanguageSaver saver) { languageSaver_ = std::move(saver); }
    // Where the "Check for updates at launch" choice is written; the platform store unless a test supplies its own.
    using UpdateCheckSaver = std::function<bool(bool enabled, std::string& diagnostic)>;
    void SetUpdateCheckSaver(UpdateCheckSaver saver) { updateCheckSaver_ = std::move(saver); }
    // Where chat availability changes are written; nowhere unless the overlay or a test supplies one.
    void SetChatLog(std::function<void(const std::string&)> log) { chatLog_ = std::move(log); }
    // Where the tournament matches already announced are kept; the platform store unless a test supplies its own.
    void SetAnnouncedStore(IdentityPanel::AnnouncedLoader loader, IdentityPanel::AnnouncedSaver saver) {
        identity_.SetAnnouncedStore(std::move(loader), std::move(saver));
    }
private:
    LanguageSaver languageSaver_;
    UpdateCheckSaver updateCheckSaver_;
    GameMenu menu_;
    IdentityPanel identity_;
    PublicRoomsPanel publicRooms_;
    RankingsPanel rankings_;
    ProfilePanel profile_;
    FriendsPanel friends_;
    ReplaysPanel replays_;
    ProblemReportPanel reportPanel_;
    CountryPicker countryPicker_;
    // The panels that own screens (ApplicationShell__Screens.cxx): each takes
    // the parts of a drawn frame that belong to its screens (ScreenPanel).
    // Update, Conceal and IdentityPanel::Hidden stay calls to every panel.
    class IdentityScreens; class RoomMemberScreens; class PublicRoomsScreens; class RankingsScreens; class ReplaysScreens; class ProfileScreens; class FriendsScreens;
    // The panel that draws `screen`, one of those or of the shell's own below
    // (one table, ApplicationShell__Screens.cxx), else null for Home and the
    // few plain ones BuildRows makes itself.
    static const ScreenPanel* ScreenOf(const std::string& screen);
    // The shell's own screens as panels, each beside its rows: Settings
    // (ApplicationShell__Settings.cxx), Online play with Join and Create, the
    // welcome and a room's screens (__ShellScreens.cxx), News (__Updates.cxx),
    // the awards wall (__Awards.cxx) and Notifications (__Toasts.cxx).
    class OnlineScreens; class WelcomeScreens; class RoomScreens; class NewsScreens; class AwardsScreens; class NotificationScreens;
    static const ScreenPanel& RoomOwner();
    static const ScreenPanel& RoomMemberOwner();
    static const ScreenPanel& OnlineOwner();
    static const ScreenPanel& WelcomeOwner();
    static const ScreenPanel& NewsOwner();
    static const ScreenPanel& AwardsOwner();
    static const ScreenPanel& NotificationsOwner();
    // The width of the public rooms list: the whole window on a narrow one, else the left pane.
    static float PublicListWidth();
    // Parts of Draw, in the order it runs them.
    void UpdateRoomTransitions(const ShellView& view,double now);
    bool UpdateRoomFeedback(const ShellView& view);
    void UpdatePreferenceSave(const ShellView& view,const Submit& submit);
    void UpdateShortCopy(const ShellView& view,double now);
    void UpdateJoinLink(const ShellView& view,double now,const Submit& submit);
    void UpdatePublicRoomLink(const ShellView& view,double now);
    // Launcher --host-public-room (ApplicationShell__StreamHost.cxx).
    void UpdateHostedPublicRoom(const ShellView& view,double now);
    void FollowAsStreamHost(const ShellView& view);
    // A dialog, a notice or an Ember ID screen is open, whose drafts moving
    // the menu would discard.
    bool MenuBusy() const {
        const auto& nav = menu_.navigation;
        return nav.Editing() || nav.Reading() || nav.Confirming() || nav.Choosing() || menu_.NoticeOpen() || IdentityPanel::Owns(nav.Screen());
    }
    // The public room Create opens, from the saved settings.
    void CreatePublicRoom(room::RoomType type);
    void UpdatePublicBridge(const ShellView& view,const std::string& screen);
    void CopyShortInvitation(const ShellView& view,const Submit& submit);
    std::vector<MenuEntry> BuildRows(const ShellView& view,const std::string& screen,bool idle,bool opening,const DrawSelection& selection,const DrawSelection& developer,std::string& title);
    // Settings and the pages it opens (ApplicationShell__Settings.cxx): six tabs
    // of panels, each panel a heading and one row builder (SettingsPanel). A
    // tab's panels are built once a frame (settingsPanels_), and the rail, the
    // summaries, the open panel's rows and its picture all read that.
    class SettingsScreens; struct SettingsPanel;
    struct BuiltPanel { const SettingsPanel* panel; std::vector<MenuEntry> rows; };
    std::vector<BuiltPanel> settingsPanels_;
    // The focus the Settings board last drew: a new one scrolls into view.
    std::string settingsFocus_;
    static const ScreenPanel& SettingsOwner();
    // Whether a screen is a Settings tab or one of its panels, and the tab it
    // belongs to ("tab-matches" for its panel "tab-matches/defaults").
    static bool SettingsTab(const std::string& screen);
    static std::string SettingsTabOf(const std::string& screen);
    // A tab's page takes the place of the tab shown and of whatever is open on
    // it. A Settings panel ("tab-matches/defaults") stands on its tab's page,
    // focused on the panel's head, so Back from the panel lands there: the one
    // way a tab or a panel opens (Home's Settings, a tab, a rail head, Select
    // on a head).
    void ShowTab(const std::string& screen);
    // Left or Right on a Settings value row that is no on/off switch; false for any other row.
    static bool AdjustSetting(netplay::PlayerPreferences& preferences,const MenuAction& action);
    // Left or Right on an in-game theme row: the game's own, then the themes with that part.
    static bool AdjustGameTheme(netplay::PlayerPreferences& preferences,const ShellView& view,const MenuAction& action);
    // The installed version label, or the build's hash for a copy without one (a developer build).
    static std::string InstalledVersion(const ShellView& view);
    // Value panels (ApplicationShell__Values.cxx): each value row's values as choices.
    static void OfferValues(std::vector<MenuEntry>& rows);
    std::pair<std::string,Tone> UpdateStatus(const ShellView& view,const std::string& screen,bool opening,bool healthyRoom,std::string& title);
    // The screen shown, inside the shell's window: an embedded one (Button
    // layout, Pad setup, fighter select, post-match, a report) or the menu.
    // True when the shell's own menu read the frame's input. Draw ends every
    // frame after it the same way (the toasts). postMatch: the prompt's frame,
    // built by Draw to size its window, for the post-match screen.
    bool DrawScreen(const ShellView& view,bool* open,const Submit& submit,const DrawSelection& selection,const DrawSelection& developer,double now,bool healthyRoom,const PostMatchFrame& postMatch);
    void PublishPlayerCard(const ShellView& view);
    void HandleActivate(const MenuAction& action,const ShellView& view,const std::string& screen,bool idle,const Submit& submit);
    void HandleAdjust(const MenuAction& action,const ShellView& view,const std::string& screen,const Submit& submit);
    void SetLanguage(std::string preference);
    // A notice's buttons carry results, not actions (GameMenu::TakeNoticeResult). Every
    // draw of the menu is followed by AnswerDialog, which does what the result says with
    // what the notice's owner left in dialogContext_.
    void AnswerDialog(const Submit& submit);
    // The launch-time update check and its question (ApplicationShell__Updates.cxx).
    // UpdateLaunchCheck asks the service for the check on the first Home and puts the
    // question up when the offer is due and the player is free to answer; a closed
    // notice is the question's answer (FinishUpdatePrompt).
    void UpdateLaunchCheck(const ShellView& view,double now,const Submit& submit);
    void FinishUpdatePrompt(NoticeResult result,const Submit& submit);
    void SetLaunchCheck(bool enabled);
    // News: the channel's releases, newest first, under a tab for all and
    // tabs for each kind there is (news-updates, news-changelog), and Update
    // now under the release the launch check offers (ApplicationShell__Updates.cxx).
    static bool NewsScreen(const std::string& screen) { return screen=="news"||screen=="news-updates"||screen=="news-changelog"; }
    std::vector<MenuEntry> NewsRows(const ShellView& view,const std::string& screen,std::string& title) const;
    void NewsTabs(const ShellView& view,const std::string& screen);
    // A release's card (its kind, New, date and title), and its pane: the
    // hero band, the notes a line each and the Update now button when offered.
    bool DrawNewsCard(const ShellView& view,const MenuEntry& row,ImVec2 min,ImVec2 max) const;
    void DrawNewsDetail(const ShellView& view,const std::string& id,const std::vector<MenuEntry>& rows);
    // Settings, Accessibility, and what speech tells as the shell draws (ApplicationShell__Accessibility.cxx).
    std::vector<MenuEntry> AccessibilityRows(const ShellView& view) const;
    bool AdjustSpeech(const MenuAction& action);
    bool ActivateSpeech(const MenuAction& action);
    void UpdateSpeech();
    UpdatePrompt updatePrompt_;
    netplay::Generation updateGeneration_;
    // The failed skips the service had counted when the view was last drawn.
    unsigned skipFailuresSeen_=0;
    double launchCheckAskedAt_=-1e9;
    // The failure of Update now (the Launcher would not start) has been said.
    bool updateNowReported_=false;
    // The Settings row's value, seeded from the view once and then the shell's own.
    bool launchCheck_=true,launchCheckSeeded_=false;
    // Told once a session that speech is on but no voice could speak; speech
    // just turned on, to be told how it works; and the natural voice's
    // download state last told.
    bool speechNoticeShown_=false,speechIntro_=false;
    speech::Voice voiceTold_=speech::Voice::System;
    // The update offer last raised as a toast.
    std::string updateToastTag_;
    std::string launchCheckSaveError_;
    // Help & about's channel row, picked as in the Updates window.
    ChannelPick channelPick_;
    struct DialogContext {
        // The pad the Button layout button opens.
        std::optional<gamepads::PadView> pad;
        // What leaving a layout with unassigned attacks logs, and for which session.
        std::string log;
        netplay::Generation generation;
    } dialogContext_;
    // Whether the selector the shell is about to show was opened just now, not
    // reshown after a match or an overlay, so it starts on its first page.
    bool selectionFresh_=false;
    // The page fighter select opens on (EmbeddedReturn::openOn).
    SelectionStart selectionOpenOn_ = SelectionStart::Board;
    // Training select opened fighter select on its Dummy row (SelectingDummy).
    bool selectionForDummy_=false;
    // The screen an opening room was started from, as the controller recorded
    // it when it accepted the command: hosting, or joining (an invitation
    // or a Discord join). Read only while the room is opening.
    // A public room's own kind says it: its create opens from Create, its join from Public rooms.
    const char* OpeningScreen(const ShellView& view) const {
        if(const auto kind=publicRooms_.OpeningKind())return *kind==PublicRoomsPanel::OpenKind::Create?"create":"public-rooms";
        return view.session.isHost?"create":"join";
    }
    // Whether the room being opened is being created, for the words that say so.
    bool OpeningCreates(const ShellView& view) const {
        const auto kind=publicRooms_.OpeningKind();
        return kind?*kind==PublicRoomsPanel::OpenKind::Create:view.session.isHost;
    }
    // The Create screen and its neighbours (ApplicationShell__Create.cxx).
    // RoomSetupRows builds Create and Defaults, which edit the same room settings.
    std::vector<MenuEntry> RoomSetupRows(const ShellView& view,const std::string& screen,bool opening,const char* reason,std::string& title);
    void CreateActivate(const MenuAction& action,const ShellView& view,const Submit& submit);
    bool CreateAdjust(const MenuAction& action);
    // A saved template onto Create's rows, and the tables a room opened from
    // Create gets with these rules (createTables_).
    void ApplyRoomTemplate(int index);
    netplay::TableSetup CreateTables(const room::Rules& rules) const;
    // Room settings › Save as template: the room as it is now, under `name`.
    void SaveRoomTemplate(const room::Snapshot& room,const std::string& name);
    // Training select: both picks and the stage, then Start Training (ApplicationShell__Training.cxx).
    std::vector<MenuEntry> TrainingSelectRows(const ShellView& view,std::string& title) const;
    void TrainingSelectActivate(const MenuAction& action,const ShellView& view,const Submit& submit);
    // The awards wall, Profile's last tab (ApplicationShell__Awards.cxx).
    static const v2::Award* WornAward(const ShellView& v, const std::string& id);
    static std::string EarnedCount(const ShellView& v);
    static int AwardColumns();
    std::vector<MenuEntry> AwardRows(const ShellView& v, std::string& title) const;
    void DrawAwardsBoard(const ShellView& v, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height, const MenuVisualFeedback& feedback);
    // Create (GameMenu body): the Room and Rules panels, the focused row's words, and the Create button.
    void DrawCreateBoard(const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,const MenuVisualFeedback& feedback);
    // The Create screen, a Classic room with Practice room off (createType_, createPractice_).
    void OpenCreate();
    // Settings > Interface > Language, also the welcome's.
    MenuEntry LanguageRow() const;
    // The first-run welcome (ApplicationShell__FirstRun.cxx): speech, language,
    // controller and look, a step at a time, each a few rows, Next and Skip.
    std::vector<MenuEntry> WelcomeRows(const ShellView& view,std::string& title);
    // Its own rows and Back; false for a row another screen owns (Language, Choose controller).
    bool WelcomeActivate(const MenuAction& action);
    void DrawWelcomeBoard(const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height);
    // Create on Public: Create public room, the empty list's card and Quick join's notice all open it.
    void OpenPublicCreate();
    // A public room still named with the default is named for its host.
    void ApplyPublicDefaultName();
    void QuickJoin(const ShellView& view);
    // The notice line: `text` in `tone` until ImGui time `until`.
    void ShowNotice(std::string text,Tone tone,double until){notice_=std::move(text);noticeTone_=tone;noticeUntil_=until;}
    std::vector<MenuEntry> RoomEntries(const ShellView& view);
    // RoomEntries' builders, one per room screen: the board, the table's
    // options, the members, one member, the chat and the room's settings.
    struct RoomScreenRows;
    void RoomBoardRows(std::vector<MenuEntry>& rows,const RoomScreenRows& state);
    void RoomTableRows(std::vector<MenuEntry>& rows,const RoomScreenRows& state);
    void RoomMembersRows(std::vector<MenuEntry>& rows,const RoomScreenRows& state);
    void RoomMemberRows(std::vector<MenuEntry>& rows,const RoomScreenRows& state);
    void RoomChatRows(std::vector<MenuEntry>& rows,const RoomScreenRows& state);
    void RoomAdminRows(std::vector<MenuEntry>& rows,const RoomScreenRows& state);
    void RoomAction(const MenuAction& action, const ShellView& view, const Submit& submit);
    void RoomShortcut(const MenuAction& action, const ShellView& view);
    // The room's shortcut hints (Fighter, Options, Chat), those the device has.
    static std::vector<LegendHint> RoomShortcutHints();
    // The board's areas (BoardArea), as LB and RB step them (RoomActions.cxx):
    // the floor, the action bar, Members and Chat; a narrow board's three tabs.
    // Each remembers the row last focused in it (NoteRoomArea), which a step
    // to it returns to. FocusRoomTab: to that tab's row, false when it has none.
    void NoteRoomArea(const std::string& focus);
    void StepRoomArea(const ShellView& view, const std::vector<MenuEntry>& rows, MenuNavigation& nav, int step, bool narrow);
    bool FocusRoomTab(const ShellView& view, const std::vector<MenuEntry>& rows, MenuNavigation& nav, int tab, bool narrow);
    // The board's rows' last touches once all of them are there: the focus a
    // newly joined room opens on, and the action bar's Left and Right in its
    // own order (RoomBoardRows).
    void SettleRoomBoard(const ShellView& view, std::vector<MenuEntry>& rows, MenuNavigation& nav);
    // What B does on the board while your own table card is focused: leave your
    // seat or queue place (or say why the seat cannot be left yet). Empty
    // (plain Back) otherwise.
    const char* PlaceExitLabel(const ShellView& view) const;
    void OpenTableOptions(const ShellView& view, int table);
    void ToggleReady(const ShellView& view, const Submit& submit);
    // Sends a step of the pick's Ultra or color, which the overlay applies
    // (SelectionStep): the room's rows and the post-match prompt.
    static void StepPick(SelectionStep step, const Submit& submit);
    // Leaves the seat or queue place B was pressed on, once any confirmation is answered.
    void LeavePlace(const ShellView& view, const Submit& submit);
    // Leaves the game this PC watches (Stop watching, or the table's own
    // Unwatch of it), once confirmed: the press names the game, and the
    // runtime decides what leaving it takes. False when nothing was sent.
    bool LeaveWatchedGame(const ShellView& view, const Submit& submit);
    // Shows a refusal that stays true for as long as stillBlocked says so.
    void Refuse(std::string text, std::function<bool(const ShellView&)> stillBlocked = {});
    void TrackLiveGames(const ShellView& view, double now);
    // A lock-in that ended gets a notice saying why: the player stopped
    // watching, moved, or took a place, or their spectator view dropped
    // (ShellView::lockInDrops). The player's own Release gets none.
    void TrackLockIn(const ShellView& view, double now);
    // Follow the room (session/RoomFollow.hxx): this session's choice, the row
    // that starts and stops it, every watching action sent (NoteWatching), and
    // the move each frame's decision sends, drawn or hidden.
    room::RoomFollow follow_;
    MenuEntry FollowRow(const ShellView& view) const;
    void ToggleFollow(const ShellView& view, const Submit& submit);
    void NoteWatching(const room::Action& sent, const room::Snapshot& room);
    void UpdateFollow(const ShellView& view, double now, const Submit& submit);
    // The post-match prompt (ApplicationShell__PostMatch.cxx, PostMatchPrompt.hxx),
    // over the WIN screen on its own screen, "post-match"; what it leaves to do
    // once the game is over; its focused button last frame (a change scrolls it
    // into view); and the WIN and in-place games already seen.
    std::optional<PostMatchPrompt> postMatch_;
    AfterPostMatch afterPostMatch_ = AfterPostMatch::None;
    PostMatchBarView postMatchBarShown_;
    bool matchResultSeen_ = false;
    unsigned gamesInPlaceSeen_ = 0;
    // What the prompt shows this frame; Draw builds it once, sizes the panel by
    // it (PostMatchPanelFor) and hands it to DrawPostMatch.
    PostMatchModel BuildPostMatch(const ShellView& view) const;
    // Opens, times out and ends the prompt; open is null while Ember is hidden.
    void UpdatePostMatch(const ShellView& view, const room::Snapshot& room, double now, const Submit& submit, bool* open);
    void ClosePostMatch(const ShellView& view, AfterPostMatch after, bool* open);
    void DrawPostMatch(const ShellView& view, const PostMatchFrame& frame, double now, const Submit& submit, bool* open);
    int lockedInTable_=-1;
    std::uint64_t lockInEpoch_=0, lockInDropsSeen_=0;
    bool GameIsStale(std::size_t table) const;
    // A member's card as the room board and the Members list draw it; no id when `row` names no member.
    MemberCardView RoomMemberCard(const ShellView& view,const MenuEntry& row) const;
    void DrawRoomBoard(const ShellView& view,const std::vector<MenuEntry>& rows,MenuNavigation& navigation,MenuAction& action,float height,
                       const MenuVisualFeedback& feedback);
    // The board's Members panel in `box` (RoomPanelColumn.cxx): its heading
    // opens the Members list, each member stands under their group as a player
    // tag (RoomMemberCard), and `shown` false leaves the heading alone.
    void DrawRoomMembersPanel(const RoomBoardFrame& frame,v2::Box box,bool shown) const;
    // The board's Chat panel in `box`: the room's last lines (DrawChatLog, in
    // the panel's own scroll, following new ones while read at the bottom)
    // over the message box, which opens Chat, and the unread count on its
    // heading; `shown` false leaves the heading alone.
    void DrawRoomChatPanel(const RoomBoardFrame& frame,v2::Box box,bool shown,unsigned unread);
    // The room board's tables and its members-over-chat column in `area`, as the
    // player's layout grips set them (PlayerPreferences::roomColumn); a folded panel's
    // box is its heading's strip and the other takes the column, or, both folded, the
    // two strips stand in a narrow column. RoomGrips draws the grips on the edges
    // shown, after the panes, and a drag sets them again while `editable`
    // (ShellView::CanEditRoomSettings); otherwise they are drawn inert.
    static constexpr float FoldedPanelHeight = 48, FoldedColumnWidth = 280;
    void ArrangeRoom(v2::Box area,v2::Box& tables,v2::Box& members,v2::Box& chat,v2::Box* picture=nullptr) const;
    // The room's Small picture: a panel at the column's top, over Members.
    bool columnPicture_ = false;
    void RoomGrips(v2::Box area,bool editable);
    // Settings' picture of that layout, with the same grips.
    void DrawRoomLayoutPreview(bool editable);
    // A room's board in its type's layout, in place of the table cards
    // (RoomPanelArcade.cxx), `size` big. `scrolled`: it stands in a scroll of
    // the room board's with the room's actions after it, which then follows the
    // focus to the cabinets and matchups as it does to the actions. `reveal`:
    // the focused row scrolls into view this frame (RoomBoardFrame::focusChanged).
    void DrawArcadeBoard(const ShellView& view,const std::vector<MenuEntry>& rows,MenuNavigation& navigation,MenuAction& action,ImVec2 size,
                         room::RoomType type,bool scrolled,bool reveal);
    // The height that board needs `width` wide for its top slot, both rows of
    // cabinets at their least size and the matchups under them.
    static float ArcadeBoardHeight(const ShellView& view,const std::vector<MenuEntry>& rows,float width);
    std::string roomBoardFocus_;
    // The row last focused in each of the board's areas (StepRoomArea), and the
    // room (its session generation and epoch) whose board has had its opening
    // focus (SettleRoomBoard).
    std::string roomAreaFocus_[static_cast<int>(BoardArea::Count)];
    std::uint64_t roomFocusGeneration_ = 0, roomFocusEpoch_ = 0;
    // Whether the room's action bar followed the tables in their scroll last
    // frame, and the tables' size: a change shows the focused row again.
    struct RoomTablesShape {
        bool follows = false;
        float width = 0, height = 0;
        bool operator==(const RoomTablesShape& o) const { return follows == o.follows && width == o.width && height == o.height; }
    };
    RoomTablesShape roomTablesShape_;
    // The Create board's focus last frame: a new one scrolls into view.
    std::string createFocus_, awardsFocus_;
    // The room chat: what the room said and did, kept here (ChatTranscript). The Chat screen is its
    // own body (DrawChatScreen); the board's Recent chat panel shows the same lines. ObserveChat takes
    // in the room every frame, drawn or hidden; UpdateChat is the drawn frame's, which also reads it.
    void ObserveChat(const ShellView& view,const room::Snapshot& room);
    void UpdateChat(const ShellView& view,const Submit& submit);
    // Sends the message the player queued once the room can take it (see queuedChat_).
    void FlushChat(const ShellView& view,const Submit& submit);
    void NoteChatGate(ChatGate gate,double now);
    void DrawChatLog(const ShellView& view,bool compact);
    void DrawChatScreen(const ShellView& view,const std::vector<MenuEntry>& rows,MenuNavigation& navigation,MenuAction& action,float height,
                        const MenuVisualFeedback& feedback);
    // The Chat screen's message box, and the room left in the message at the right of the note line
    // under it (`noteHeight` tall). A tournament match's watcher has neither.
    void DrawChatBox(float x,float width,bool typable,float noteHeight);
    // The room left in the message, in bytes, which the counter says.
    int ChatBytesLeft() const;
    ChatTranscript transcript_;
    // A message sent and not yet seen in the room's chat. The draft stays in the box until it is, however
    // late, or until the room changes; `after` is the newest message already looked at. For 8 seconds
    // from `since` the same text is not sent again (ChatInFlight); after that it may be, and is still
    // looked for.
    struct PendingChat { std::string text; std::uint64_t after = 0; double since = 0; };
    std::optional<PendingChat> pendingChat_;
    bool ChatInFlight(double now) const { return pendingChat_ && now >= pendingChat_->since && now - pendingChat_->since <= 8; }
    // A message entered while the room could not take it, apart from the draft the player keeps editing and
    // from pendingChat_, whose receipt is still looked for. It goes out once, when the room can (FlushChat),
    // and no other is accepted until then. A retry of text that arrives first is not sent. It stays held
    // until the runtime has taken it: a refused submission is tried again after a second, then after one
    // more each time, to at most MaximumChatRetrySeconds. It lasts as long as the member is in the same open
    // room, whether or not Ember is shown.
    struct QueuedChat { std::string text; double retryAt = 0; unsigned attempts = 0; };
    static constexpr double MaximumChatRetrySeconds = 5;
    std::optional<QueuedChat> queuedChat_;
    // Chat availability changes go to the log at most once a second, with how many they cover.
    ChatGate chatGate_ = ChatGate::NotJoined, chatGateLogged_ = ChatGate::NotJoined;
    double chatGateLoggedAt_ = -1e9;
    unsigned chatGateChanges_ = 0;
    std::function<void(const std::string&)> chatLog_;
    // The Chat screen has been drawn since the player last left it.
    bool chatOpen_=false;
    // When the message box was last edited, and whether this frame's draft counts as typing (ChatTyping).
    double chatEditedAt_ = -1e9;
    bool chatTyping_ = false;
    // The text the message box last held. ImGui ignores the buffer it is given while the box has the
    // keyboard, so a draft the shell changed (sent, or a new room) is handed to the box again.
    std::string chatBoxText_;
    std::size_t chatShown_=0;
    double saveAt_ = 0;
    double lastUiTime_ = -1;
    bool saveFailed_ = false;
    bool saveQueued_ = false, retrySave_ = false;
    bool profileSavePending_ = false;
    netplay::PlayerPreferences savingPreferences_;
    room::MemberId selectedMember_ = 0;
// A member action from a popout that asks first (kick, transfer-host): asked on the member's page once it opens.
std::string memberAsk_;
    std::uint64_t inviteRevision_ = 0;
    std::uint64_t tableGeneration_ = 0;
    int generationTable_ = -1;
    double roomUpdateUntil_ = 0;
    double roomUpdateStarted_ = -1;
    bool roomUpdateVisible_ = false;
    // The room board drew the watched game's picture, cinema style, so the corner window stays away.
    bool boardPicture_ = false;
    // The same, kept to the next frame, for the room's rows (its Hide row).
    bool pictureOnBoard_ = false;
    bool boardDrawn_ = false;
    // The room board was last drawn narrow, its parts under tabs (DrawRoomBoard).
    bool roomBoardNarrow_ = false;
    bool selectionDrawn_ = false;
    std::map<std::string,std::string> roomDetails_;
    char invitation_[4097] = {};
    // Create's Practice room and Arcade choices for the private room it opens.
    // Never saved: every visit to Create starts with them off.
    bool createPractice_ = false;
    // The template Create was set from (-1 none), and its tables. Like the
    // Practice room choice, every visit to Create starts without one.
    int createTemplate_ = -1;
    std::optional<netplay::TableSetup> createTables_;
    // The welcome's step (0 speech to 3 look), whether it was offered this
    // launch, and the speech answer (ponytail: kept until speech settings land).
    int welcomeStep_ = 0;
    bool welcomeOffered_ = false;
    room::RoomType createType_ = room::RoomType::Classic;
    // An Arcade or King of the hill room's tables on Create, all of them until
    // changed; at one the winner stays. Not kept, like the type.
    int createArcadeTables_ = static_cast<int>(room::TableCount);
    // The table count Create opens its room with, of the type chosen.
    int CreateTableCount() const { return createType_ != room::RoomType::Classic ? createArcadeTables_ : preferences_.roomTableCount; }
    bool preferencesDirty_ = false;
    bool publicBridgeAsked_ = false; // the public-room service was asked for in this stay on a screen that needs it
    // Quick join's notice chose Create room: Draw opens it on the next frame, outside the menu's own draw.
    bool openPublicCreate_ = false;
    // The language is stored in its own file, so it debounces on its own
    // deadline rather than sharing saveAt_ with the netplay preferences.
    double languageSaveAt_ = 0;
    bool languageSeeded_ = false, languageDirty_ = false, gameSettingsChecked_ = false;
    std::string languagePreference_ = "auto", languageSaveError_;
    netplay::Generation generation_;
    netplay::RoomState previousRoomState_ = netplay::RoomState::Idle;
    // The room now closing was joined, not only opened: kept through Closing,
    // which every way out of a room passes, and cleared at Idle.
    bool roomWasJoined_ = false;
    // The tournament phase last seen, to announce a match that ended.
    netplay::tournament::Phase tournamentPhase_ = netplay::tournament::Phase::Idle;
    // The last match link outcome announced.
    std::uint64_t matchLinkSequence_ = 0;
    // The last watch notice shown (a match over, the watcher removed, the room closed).
    std::uint64_t watchNoticeSequence_ = 0;
    // The last Discord connect link seen, and its service until Connect
    // Discord can open (not in a room).
    std::uint64_t connectLinkSequence_ = 0;
    std::string pendingConnect_;
    // Whether it waited for a room.
    bool pendingConnectStale_ = false;
    netplay::PlayerPreferences preferences_;
    std::string error_;
    // A shell error has no natural clear point (a paste that failed, an
    // invalid value), so it ends with the screen it appeared on, with the
    // condition a refusal named (Refuse), or a few seconds after it appeared.
    std::string lastError_, errorScreen_, errorBlockedText_;
    double errorSince_=0;
    std::function<bool(const ShellView&)> errorBlocked_;
    // B on a seat that would lose a score or hand the seat over asks first: the
    // table whose card carries the question, and whether it is open yet.
    int leaveAsk_=-1;
    // More… asked again by the room's rows: after a switch it holds (moreChosen_
    // is the option it opens on), or as the picture's Small, Big and Off (morePicture_).
    bool moreAsk_=false,morePicture_=false;
    std::string moreChosen_;
    bool leaveAsked_=false;
    // When each table's live game was first seen, for the host's stuck-game row.
    struct LiveGame { std::uint64_t generation=0; double since=-1; };
    std::array<LiveGame,room::TableCount> liveGames_;
    std::string notice_;
    double noticeUntil_=0;
    // The notices and refusals said lately, newest first: the toasts while
    // they last and the Notifications screen's rows (ApplicationShell__Toasts.cxx).
    struct Toast { unsigned id; std::string text; Tone tone; double at, until; ToastKind kind; std::vector<ToastAction> actions; };
    std::deque<Toast> toasts_;
    std::string noticeSeen_, errorSeen_;
    unsigned toastCount_=0;
    // The kind and actions Say gave notice_, while it still says that text.
    struct NoticeExtra { std::string text; ToastKind kind=ToastKind::None; std::vector<ToastAction> actions; } noticeExtra_;
    // The action toast that holds focus over the open menu (0 for none), and
    // an action its buttons or rows asked for: the toast's id and the action's index.
    unsigned toastFocus_=0;
    std::pair<unsigned,std::size_t> toastAsked_{0,0};
    // The notices said this frame become toasts in Notifications; every frame
    // takes them (Background alone, while the game shows without Ember).
    // Returns a new toast with buttons, or 0.
    unsigned TakeNotices(double now);
    // Takes the notices and draws the toasts' cards over Ember's menus (Draw's
    // tail). listed: Notifications is shown, which lists them instead.
    // focusable: the shell's own menu read this frame's input, so a new
    // action toast may take its focus (not over an embedded screen).
    void DrawToasts(double now,bool listed=false,bool focusable=false);
    // Where Ember draws: the screen, inside TV mode's 5% safe area. The shell's
    // window, and the toasts over it.
    v2::Box ShellArea() const;
    void AnswerToast(const ShellView& view,double now,const Submit& submit);
    std::vector<MenuEntry> NotificationRows(double now,std::string& title) const;
    // Copy short link was pressed before the link existed: copy it when it
    // arrives, or the full invitation on a failure or at the deadline.
    bool shortCopyPending_=false;
    double shortCopyUntil_=0;
    std::uint64_t shortFailuresSeen_=0;
    // A room link from the browser waits here until no room is open. One
    // that arrived while the player was free joins by itself, for as long
    // as the runtime offers that.
    std::uint64_t joinLinkSeen_=0;
    // Ember was hidden on the last frame (Background): the next drawn one opens it.
    bool hidden_=true;
    std::optional<std::string> reopenScreen_;
    std::optional<gamepads::PadView> setupPad_, setupPrompt_;
    std::vector<gamepads::Identity> setupPrompted_;
    std::set<unsigned> setupPromptHeld_;
    bool setupStarting_=false, setupPromptReleased_=false, setupFromLayout_=false;
    std::uint64_t setupRevision_=0, setupMenuSequence_=0;
    gamepads::SetupMode setupPreviousMode_=gamepads::SetupMode::Capture;
    std::optional<gamepads::PadView> layoutPad_;
    bool layoutChanged_ = false, layoutAbsent_ = false;
    unsigned layoutButton_ = 0;
    // An index into LayoutFunctions or a BoundActionCapture; none for a pick.
    std::optional<unsigned> layoutCaptureFunction_;
    gamepads::LayoutCapture layoutCapture_;
    bool layoutNeutral_ = false;
    // The row that just took a press, shown in Ember until layoutFlashUntil_.
    std::string layoutFlash_;
    double layoutFlashUntil_ = 0;
    void SyncLayoutPaths(const ShellView& view);
    void SetGameplayLayoutPrompts(const ShellView& view);
    void CancelLayoutCapture(const ShellView& view, const Submit& submit);
    void OpenButtonLayout(const gamepads::PadView& pad);
    bool DrawButtonLayout(const ShellView& view, const Submit& submit);
    bool ControllerActivate(const MenuAction& action, const ShellView& view, const Submit& submit);
    bool SavePreferences(const ShellView& view, const Submit& submit);
    // bound and extras replace the pad's bound buttons and extra button copies; none keeps them.
    bool ChangeLayout(const gamepads::PadView& pad, const std::optional<gamepads::ButtonLayout>& layout,
        const ShellView& view, const Submit& submit, const std::string& log, const std::optional<gamepads::BoundButtons>& bound = {},
        const std::optional<gamepads::ExtraCopies>& extras = {});
    // Binds an action to a layout button, refusing one another action of its group has and, for one
    // that needs a free button, one the layout gives an attack.
    void ChangeBoundButton(const gamepads::PadView& pad, gamepads::BoundAction action, unsigned button, const ShellView& view, const Submit& submit);
    // Sets an extra button to copy the layout button a choice names, or nothing for "off".
    void ChangeExtraCopy(const gamepads::PadView& pad, unsigned extra, const std::string& choice, const ShellView& view, const Submit& submit);
    void LayoutLog(const ShellView& view, const Submit& submit, std::string text);
    void BeginPadSetup(const gamepads::PadView& pad,const ShellView& view,const Submit& submit);
    void UpdatePadSetup(const ShellView& view,const Submit& submit,bool visible);
    bool DrawPadSetup(const ShellView& view,const Submit& submit);
    void SetupControl(gamepads::SetupCommand command,const ShellView& view,const Submit& submit,int step=0);
    void ClosePadSetup(const ShellView& view,bool controllers=false);

    // The training call's sequences as last acted on, and the opponent's
    // fighter change as the board last saw it (it focuses the Ultra).
    std::uint64_t trainingCallSeen_=0,trainingReadySeen_=0,opponentChangeSeen_=0;
    std::string joinLink_;
    bool joinLinkDirect_=false;
    // A public room link from the browser: the last one seen, and whether
    // Public rooms still has to open for it.
    std::uint64_t roomLinkSeen_=0;
    bool roomLinkOpening_=false;
    // --host-public-room: the next time a refused or lost create may be asked again.
    double hostRoomAt_=0;
    // --host-public-room: the room it started following in.
    std::uint64_t streamFollowEpoch_=0;
    Tone noticeTone_=Tone::Success;
    std::uint64_t roomEpoch_ = 0, rulesRevision_ = 0, nextActionId_ = 1, readyFailureSequence_ = 0;
    int selectedTable_ = 0, roomCapacity_ = 16;
    // Room settings' Tables value, before Apply tables sends it.
    int roomTableCount_ = static_cast<int>(room::ClassicTableCount);
    char roomName_[65] = {}, chat_[257] = {};
    room::Rules tableRules_;
    bool rulesDirty_ = false;
    // Members this client muted, in the room of mutedEpoch_ only: an id names
    // someone else in another room (ObserveChat clears them on a new room).
    std::set<room::MemberId> muted_;
    std::uint64_t mutedEpoch_ = 0;
    bool Service(platform::ServiceAction action, const ShellView& view, const Submit& submit);
    bool SendRoom(room::Action action, const ShellView& view, const Submit& submit);
    // Hands a command to the runtime: a full queue is said on the status
    // line, which is cleared otherwise. False when it was not taken.
    bool Send(ShellAction action, const Submit& submit);
    // The room commands sent from more than one place.
    bool SendReady(const ShellView& view, bool rematch, const Submit& submit);
    bool SendJoin(const ShellView& view, const Submit& submit);
    bool SendLeave(const ShellView& view, const Submit& submit);
    // The preferences being edited, as a save or a new room sends them.
    netplay::PlayerPreferences PreferencesToSend() const;
};

} }
