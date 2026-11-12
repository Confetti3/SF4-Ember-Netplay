#include "sf4e__ShellHost.hxx"

#include <ctime>

#include <windows.h>

#include "sf4e__GameAdapter.hxx"

namespace ember {

void ShellHost::Background(const ShellGame::RuntimeSnapshot& snapshot) {
    // The room is lent as a drawn frame lends it (copied once per change, not
    // per match frame), and only its few notices (at most RoomNoticesKept)
    // are copied.
    sf4e::ui::ShellView view;
    view.session = snapshot.session;
    roomCarry_.Lend(view, snapshot.room);
    view.watchedGame = snapshot.watchedGame;
    // The post-match prompt goes on while Ember is hidden (UpdatePostMatch).
    view.matchResult = snapshot.menuContext == sf4e::input::MenuContext::MatchResult;
    view.gamesInPlace = snapshot.gamesInPlace; view.foughtGame = snapshot.foughtGame;
    view.roomNotices = snapshot.roomNotices;
    view.identity = snapshot.identity;
    view.identityTicket = snapshot.identityTicket; view.identityRequest = snapshot.identityRequest;
    view.identityRefusal = snapshot.identityRefusal;
    shell_.Background(view, view.room, [](sf4e::ui::ShellAction action) {
        sf4e::NetplayFacade::RuntimeCommand request;
        request.command = std::move(action.command);
        request.roomAction = std::move(action.roomAction);
        request.leaveWatchedGame = action.leaveWatchedGame;
        request.identity = std::move(action.identity);
        request.publicTicket = std::move(action.publicTicket);
        request.createdRules = action.createdRules;
        request.arcadeRoom = action.arcadeRoom;
        request.wiredOnly = action.wiredOnly;
        request.tinyFighters = action.tinyFighters;
        return sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request));
    });
    roomCarry_.Return(view);
}

const char* ShellHost::Draw(const ShellGame::RuntimeSnapshot& snapshot, const sf4e::NetplayStatus& status,
    const sf4e::ui::ControllerNavigation& navigation, ShellGame& game,
    const sf4e::ui::ApplicationShell::DrawSelection& developer) {
    sf4e::ui::ShellView view;
    view.controllerAvailable = navigation.Available();
    view.controllerUnavailable = navigation.Unavailable();
    view.controllerFocus = navigation.FocusRequested();
    view.controllerBack = navigation.BackRequested();
    view.session = snapshot.session;
    // The room is lent from the last frame's copy unless the published one
    // changed (it is copied once per change, not per frame) and given back
    // after the frame. Idle times and start holds were stamped when the
    // snapshot was sent; the shell counts them on from the stamp where it
    // reads them, so the room is never patched.
    roomCarry_.Lend(view, snapshot.room);
    if (snapshot.roomReceivedMs) view.roomElapsedMs = GetTickCount64() - snapshot.roomReceivedMs;
    view.roomNotices = snapshot.roomNotices;
    view.preferences = snapshot.preferences;
    view.lobbySettings = snapshot.lobbySettings;
    view.helperReady = snapshot.helperReady;
    view.canOpenRoom = snapshot.canOpenRoom;
    view.canReplaceRoom = snapshot.canReplaceRoom;
    view.canReady = snapshot.canReady;
    view.canEditSelection = snapshot.canEditSelection; view.canChangeUltra = snapshot.canChangeUltra;
    view.selectionLockReason = snapshot.selectionLockReason;
    view.readyPress = snapshot.readyPress; view.readyFailure = snapshot.readyFailure;
    view.readyFailureSequence = snapshot.readyFailureSequence;
    view.opponentChangedFighter = snapshot.opponentChangedFighter; view.opponentChangeSequence = snapshot.opponentChangeSequence;
    view.trainingCallSequence = snapshot.trainingCallSequence; view.trainingReadySequence = snapshot.trainingReadySequence;
    view.trainingReadySeconds = snapshot.trainingReadySeconds; view.canTrain = snapshot.canTrain; view.canWatch = snapshot.canWatch;
    view.offersTraining = game::Game().caps.training; view.offersReplays = game::Game().caps.replays;
    view.offersFrameMeter = game::Game().caps.frameMeter; view.offersSteamProfile = game::Game().caps.steamProfile;
    view.offersTinyFighters = game::Game().caps.tinyFighters;
    view.offersHitboxes = game::Game().caps.hitboxes;
    view.settingsFeatures = game::Game().caps.settingsFeatures;
    view.replayGameList = game::Game().caps.replayGameList; view.replayVideo = game::Game().caps.replayVideo;
    view.replayMeter = game::Game().caps.replayMeter; view.replayInputs = game::Game().caps.replayInputs;
    view.practiceFight = snapshot.menuContext == sf4e::input::MenuContext::PracticeMatch;
    view.watchedGame = snapshot.watchedGame;
    view.matchResult = snapshot.menuContext == sf4e::input::MenuContext::MatchResult;
    view.rematchChoices = snapshot.rematchChoices; view.rematchDeclines = snapshot.rematchDeclines; view.gamesInPlace = snapshot.gamesInPlace;
    view.foughtGame = snapshot.foughtGame;
    view.lockInDrops = snapshot.lockInDrops;
    view.canEditPreferences = snapshot.canEditPreferences;
    view.canEditLobby = snapshot.canEditLobby;
    view.settingsPending = snapshot.settingsPending;
    view.selectedDelay=snapshot.selectedDelay; view.recommendedDelay=snapshot.recommendedDelay; view.autoDelayMeasured=snapshot.autoDelayMeasured;
    view.opponentDelay=snapshot.opponentDelay;
    view.delayLocked=snapshot.delayLocked; view.canProbe=snapshot.canProbe; view.canApplyDelay=snapshot.canApplyDelay;
    view.probeRelay=snapshot.probeRelay; view.probeOpponent=snapshot.probeOpponent;
    view.probeOpponentNat=snapshot.probeOpponentNat; view.netReport=snapshot.netReport;
    view.probeRoute=snapshot.probeRoute; view.probeP50Us=snapshot.probeP50Us; view.probeP95Us=snapshot.probeP95Us;
    view.probeP99Us=snapshot.probeP99Us; view.probeJitterUs=snapshot.probeJitterUs; view.probeBenchmark=snapshot.probeBenchmark;
    view.probeStatus=snapshot.probeStatus; view.probeSamples=snapshot.probeSamples; view.probeLost=snapshot.probeLost;
    view.probeSent=snapshot.probeSent; view.probeExpected=snapshot.probeExpected;
    view.memberLatency=snapshot.memberLatency;
    view.localSlot = snapshot.localSlot;
    view.invitation = snapshot.invitation;
    view.shortInvitation = snapshot.shortInvitation;
    view.shortInvitationPending = snapshot.shortInvitationPending;
    view.shortInvitationFailures = snapshot.shortInvitationFailures;
    view.pendingJoinLink = snapshot.pendingJoinLink;
    view.pendingJoinSequence = snapshot.pendingJoinSequence;
    view.pendingJoinDirect = snapshot.pendingJoinDirect;
    view.error = snapshot.helperError;
    view.settingsError = snapshot.settingsError;
    view.languagePreference = snapshot.languagePreference;
    view.checkUpdatesAtLaunch = snapshot.checkUpdatesAtLaunch;
    view.automaticCountry = snapshot.automaticCountry;
    view.unixNow = static_cast<std::uint64_t>(std::time(nullptr));
    view.members = snapshot.Members();
    view.network = snapshot.network; view.services = snapshot.services;
    view.controller = snapshot.controller;
    view.discordPending=snapshot.discordPending; view.discordConfirm=snapshot.discordConfirm;
    view.discordRevision=snapshot.discordRevision;
    view.discordCanSwitch=snapshot.discordCanSwitch; view.discordStatus=snapshot.discordStatus;
    view.inputDevice = snapshot.inputDevice; view.menuButtons = snapshot.menuController.menu;
    view.gameplayBrand = snapshot.gameplayBrand; view.promptBrand = snapshot.promptBrand;
    view.padHint = snapshot.padHint;
    view.padHintUntil = snapshot.padHintUntil; view.padLayer = snapshot.padLayer;
    view.padSetup = snapshot.padSetup; view.atMainMenu = snapshot.atMainMenu;
    view.padLayerPending = snapshot.padLayerPending; view.runningControllerLayer = snapshot.runningControllerLayer;
    view.lobbyMusicAvailable = snapshot.lobbyMusicAvailable;
    view.pads = snapshot.pads; view.captureCandidate = snapshot.captureCandidate;
    view.inputCapture = snapshot.inputCapture;
    view.controllerReady = snapshot.controllerReady;
    view.canChangeController = snapshot.canChangeController;
    view.identity = snapshot.identity;
    view.identityTicket = snapshot.identityTicket; view.identityRequest = snapshot.identityRequest;
    view.identityRefusal = snapshot.identityRefusal;
    view.tournament = snapshot.tournament;
    view.publicRooms = snapshot.publicRooms;
    view.rankings = snapshot.rankings;
    // The archive is listed, and an entry's inputs read, off this thread (the
    // runtime's workers). The screen on show says what to ask for; the last
    // listing and detail they made come with the snapshot.
    const auto replayWants = shell_.ReplayWants();
    if (replayWants.listing) game::Game().replays.WantListing();
    if (replayWants.detail) game::Game().replays.WantDetail(replayWants.detailFile, replayWants.detailRevision);
    view.replays = snapshot.replays;
    game.FillView(snapshot, view);
    // Outside a fight the shell status line carries the notice; transient
    // info ("Connection restored.") belongs to the match HUD only.
    if (view.error.empty() && status.lastError[0] && status.lastErrorSeverity != sf4e::NoticeSeverity::Info) view.error = status.lastError;
    // Why the menu closes after this frame: the latest reason given, as the
    // presentation keeps it.
    const char* close = nullptr;
    bool open = true;
    shell_.Draw(view, &open, [&](sf4e::ui::ShellAction action) {
        // The Ultra (the board, the table page, the post-match prompt) and the
        // table page's Appearance edit the pick here; nothing is sent.
        if (const auto step = action.selectionStep; step.field != sf4e::ui::ShellAction::SelectionStep::Field::None)
            return game.StepSelection(step, snapshot);
        // Training select's Hitboxes row: the game keeps the choice; nothing
        // is sent, and a game without the viewer is never asked.
        if (action.showHitboxes) return game::Game().caps.hitboxes && game.ShowHitboxes(*action.showHitboxes);
        // Training select's Start: the game enters Training with these once
        // its menu is idle (ShellGame::StartTraining). Not from a room or match, nor
        // in a game without Training.
        if (action.startTraining) {
            if (!game::Game().caps.training || !snapshot.atMainMenu || snapshot.session.room != sf4e::netplay::RoomState::Idle || snapshot.session.match != sf4e::netplay::MatchState::None)
                return false;
            game.StartTraining(snapshot);
            close = "Training select";
            return true;
        }
        sf4e::NetplayFacade::RuntimeCommand request;
        request.command = std::move(action.command);
        request.service = action.service; request.servicePath = std::move(action.servicePath); request.steamProfile = action.steamProfile;
        request.updateOffer = std::move(action.updateOffer); request.updateChannel = action.updateChannel;
        request.replay = std::move(action.replay);
        request.reportSubmission = std::move(action.reportSubmission);
        request.inputAction = action.inputAction; request.discordAction = action.discordAction;
        request.padAction = action.padAction;
        request.discordRevision = action.discordRevision;
        request.displayName = snapshot.preferences.displayName;
        request.preferences = std::move(action.preferences);
        request.roomAction = std::move(action.roomAction);
        request.selectedDelay=action.selectedDelay;
        request.previewSoundVolume=action.previewSoundVolume;
        request.identity = std::move(action.identity);
        request.shortInvitation=action.shortInvitation;
        request.roomSettings=action.roomSettings;
        request.tournament = std::move(action.tournament);
        request.publicTicket = std::move(action.publicTicket);
        request.createdRules = action.createdRules;
        request.practiceRoom = action.practiceRoom;
        request.arcadeRoom = action.arcadeRoom;
        request.delayPolicy = action.delayPolicy;
        request.tableCount = action.tableCount;
        request.wiredOnly = action.wiredOnly;
        request.tinyFighters = action.tinyFighters;
        game.AddPick(request);
        request.training = action.training;
        request.leaveWatchedGame = action.leaveWatchedGame;
        if (!sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request))) return false;
        // Training from a room or Home: the menu closes behind it while the runtime
        // sends the game on.
        if (action.training != sf4e::TrainingEntry::None) close = "Training";
        return true;
    }, [&] { game.DrawSelection(shell_.SelectingDummy(), snapshot); }, developer);
    if (shell_.BoardDrawn()) sf4e::NetplayFacade::NoteLatencyBoard(view.room.roomEpoch);
    if (shell_.SelectionDrawn()) sf4e::NetplayFacade::NoteFighterSelector(view.room.roomEpoch);
    if (shell_.ChatTyping()) sf4e::NetplayFacade::NoteChatTyping(view.room.roomEpoch);
    roomCarry_.Return(view);
    if (!open) close = "Back on Ember's Home";
    return close;
}

} // namespace ember
