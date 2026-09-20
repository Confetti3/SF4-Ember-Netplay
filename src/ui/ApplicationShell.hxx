#pragma once
#include "../discord/Presence.hxx"
#include "../netplay/InputAssignment.hxx"
#include "../netplay/SessionController.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/MemberView.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../session/RoomModel.hxx"
#include <functional>
#include <vector>
#include <set>
#include <map>
#include "GameMenu.hxx"

namespace sf4e { namespace ui {

// The shared shell has no game, transport, filesystem or bootstrap dependency.
// Its caller supplies a copied view and translates actions on the owning thread.
struct ShellView {
    bool controllerAvailable = false, controllerFocus = false, controllerBack = false;
    bool controllerUnavailable = false;
    netplay::Snapshot session;
    room::Snapshot room;
    netplay::PlayerPreferences preferences;
    netplay::LobbySettings lobbySettings;
    bool helperReady = false, canOpenRoom = false, canReady = false;
    bool canReplaceRoom = false;
    bool canEditSelection = false;
    bool canEditPreferences = false, canEditLobby = false, settingsPending = false;
    int selectedDelay=2, recommendedDelay=-1;
    bool delayLocked=false, canProbe=false, canApplyDelay=false;
    std::string probeStatus, probeRoute;
    std::uint64_t probeP50Us=0, probeP95Us=0, probeP99Us=0, probeJitterUs=0;
    bool probeBenchmark=false;
    unsigned probeSamples=0, probeLost=0, probeSent=0, probeExpected=0;
    int localSlot = -1;
    std::string invitation, error, settingsError, build;
    std::string languagePreference = "auto";
    std::vector<netplay::MemberView> members;
    netplay::NetworkAvailability network = netplay::NetworkAvailability::Starting;
    platform::ServiceSnapshot services;
    std::string controller;
    bool discordPending = false, discordConfirm = false, discordCanSwitch = false;
    std::uint64_t discordRevision = 0;
    std::string discordStatus;
            input::Capture inputCapture = input::Capture::Idle;
            input::Device inputDevice;
            bool canChangeController = false, controllerReady = false;
    std::string selectionSummary, selectionError;
    std::string selectionLockReason, readyLockReason;
    // A Ready press in flight (parked, sent or awaiting commit), and the
    // last failure with a sequence that changes per occurrence.
    bool readyRequested = false;
    std::string readyFailure;
    std::uint64_t readyFailureSequence = 0;
    int selectedFighter = 0;
};

struct ShellAction {
    netplay::Command command{netplay::CommandKind::HostRoom};
    platform::ServiceAction service = platform::ServiceAction::None;
            input::Action inputAction = input::Action::None;
    discord::InviteAction discordAction = discord::InviteAction::None;
    std::uint64_t discordRevision = 0;
    netplay::PlayerPreferences preferences;
    room::Action roomAction;
    int selectedDelay=-1;
};

class ApplicationShell {
public:
    using Submit = std::function<bool(ShellAction)>;
    using DrawSelection = std::function<void()>;
    void Draw(const ShellView& view, bool* open, const Submit& submit, const DrawSelection& selection,
              const DrawSelection& developer = {});
    void ShowPlay() {
        // Reopening after battle must retain the active room's navigation.
        if(previousRoomState_==netplay::RoomState::Idle)menu_.navigation.Home();
    }
    MenuNavigation& Navigation() { return menu_.navigation; }
private:
    GameMenu menu_;
    std::vector<MenuEntry> RoomEntries(const ShellView& view);
    void RoomAction(const MenuAction& action, const ShellView& view, const Submit& submit);
    void DrawRoomBoard(const ShellView& view,const std::vector<MenuEntry>& rows,MenuNavigation& navigation,MenuAction& action,float height,
                       const MenuVisualFeedback& feedback);
    std::string roomBoardFocus_;
    std::uint64_t chatSequence_=0;
    double saveAt_ = 0;
    double lastUiTime_ = -1;
    bool saveFailed_ = false;
    bool saveQueued_ = false, retrySave_ = false;
    bool profileSavePending_ = false;
    netplay::PlayerPreferences savingPreferences_;
    room::MemberId selectedMember_ = 0;
    std::uint64_t inviteRevision_ = 0;
    std::uint64_t tableGeneration_ = 0;
    double roomUpdateUntil_ = 0;
    double roomUpdateStarted_ = -1;
    bool roomUpdateVisible_ = false;
    std::map<std::string,std::string> roomDetails_;
    char invitation_[4097] = {};
    bool preferencesDirty_ = false;
    // The language is stored in its own file, so it debounces on its own
    // deadline rather than sharing saveAt_ with the netplay preferences.
    double languageSaveAt_ = 0;
    bool languageSeeded_ = false, languageDirty_ = false;
    std::string languagePreference_ = "auto", languageSaveError_;
    netplay::Generation generation_;
    netplay::RoomState previousRoomState_ = netplay::RoomState::Idle;
    netplay::PlayerPreferences preferences_;
    netplay::LobbySettings lobby_;
    std::string error_;
    // A shell error has no natural clear point (a paste that failed, an
    // invalid value), so it expires after a while instead of following the
    // player across every screen.
    std::string lastError_;
    double errorSince_=0;
    std::string notice_;
    double noticeUntil_=0;
    Tone noticeTone_=Tone::Success;
    std::uint64_t roomEpoch_ = 0, rulesRevision_ = 0, nextActionId_ = 1, readyFailureSequence_ = 0;
    int selectedTable_ = 0, roomCapacity_ = 16;
    char roomName_[65] = {}, chat_[257] = {};
    room::Rules tableRules_;
    bool rulesDirty_ = false;
    std::set<room::MemberId> muted_;
    bool Service(platform::ServiceAction action, const ShellView& view, const Submit& submit);
    bool SendRoom(room::Action action, const ShellView& view, const Submit& submit);
    bool Send(netplay::CommandKind kind, const ShellView& view, const Submit& submit);
};

} }
