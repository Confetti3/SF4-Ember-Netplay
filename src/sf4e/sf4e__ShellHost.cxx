#include "sf4e__ShellHost.hxx"

#include <ctime>

#include <windows.h>

#include "sf4e__GameAdapter.hxx"

namespace ember {

void ShellHost::Summon(const ShellGame::RuntimeSnapshot& snapshot, bool menuAvailable, const std::function<void(const char*)>& open) {
    using sf4e::ui::SummonsAsk; using sf4e::ui::SummonsReason; using sf4e::ui::SummonsSequenceKey;
    const bool atMenu = snapshot.atMainMenu, menuFree = atMenu && menuAvailable;
    std::array<SummonsAsk, static_cast<std::size_t>(SummonsReason::Count)> asks;
    const auto ask = [&asks](SummonsReason reason, std::string key, bool ready, bool opens = true) {
        asks[static_cast<std::size_t>(reason)] = SummonsAsk{std::move(key), ready, opens};
    };
    // The WIN screen of a room game this PC fought: the post-match prompt opens over it, once per game.
    ask(SummonsReason::MatchResult, snapshot.menuContext == sf4e::input::MenuContext::MatchResult ? "1" : "", true);
    ask(SummonsReason::DiscordInvite, snapshot.discordPending ? "1" : "", atMenu);
    // A room link from the browser opens the menu at the main menu, where the shell puts it on the Join screen.
    ask(SummonsReason::JoinLink, SummonsSequenceKey(snapshot.pendingJoinSequence), menuFree);
    // Called out of Training: back at the main menu the menu opens on the player's table, which the shell turns to.
    ask(SummonsReason::TrainingCall, SummonsSequenceKey(snapshot.trainingCallSequence), menuFree);
    // A Discord connect link likewise, at the main menu only: during play it waits until the player opens Ember, which then asks before going on.
    ask(SummonsReason::DiscordConnectLink, SummonsSequenceKey(snapshot.tournament.connect.sequence), menuFree);
    // A replay link opens the menu at the main menu with no room, where the shell puts its question on the Replays screen.
    ask(SummonsReason::ReplayLink, snapshot.replays.link, menuFree && snapshot.session.room == sf4e::netplay::RoomState::Idle);
    // A public room link the player was free to follow opens the menu, where the shell takes them to Public rooms;
    // one that was not waits until the player opens Ember, and the shell says so.
    ask(SummonsReason::PublicRoomLink, SummonsSequenceKey(snapshot.tournament.roomLink.sequence), menuFree, snapshot.tournament.roomLink.free);
    summons_.Update(asks, open);
}

bool ShellHost::Submit(sf4e::RuntimeRequest request, ShellGame& game) {
    if (auto* ready = std::get_if<sf4e::commands::Ready>(&request)) game.AddPick(*ready);
    return sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request));
}

void ShellHost::Background(const ShellGame::RuntimeSnapshot& snapshot, ShellGame& game) {
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
    view.hostPublicRoom = snapshot.hostPublicRoom;
    shell_.Background(view, view.room, [&game](sf4e::ui::ShellAction action) {
        auto request = sf4e::ui::ForRuntime(std::move(action));
        return request && Submit(std::move(*request), game);
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
    view.hostPublicRoom = snapshot.hostPublicRoom;
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
    view.offersSkipScenes = game::Game().caps.skipScenes;
    view.offersBannedStages = game::Game().caps.bannedStages;
    if (view.offersBannedStages) view.bannableStages = game::Game().state.BannableStages();
    view.offersHitboxes = game::Game().caps.hitboxes;
    view.settingsFeatures = game::Game().caps.settingsFeatures;
    view.replayGameList = game::Game().caps.replayGameList; view.replayVideo = game::Game().caps.replayVideo;
    view.replayMeter = game::Game().caps.replayMeter; view.replayInputs = game::Game().caps.replayInputs;
    view.offersArchive = game::Game().caps.archive;
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
    view.automaticArea = snapshot.automaticArea;
    view.unixNow = static_cast<std::uint64_t>(std::time(nullptr));
    view.members = snapshot.Members();
    view.network = snapshot.network; view.services = snapshot.services;
    view.controller = snapshot.controller;
    view.discordPending=snapshot.discordPending; view.discordConfirm=snapshot.discordConfirm;
    view.discordRevision=snapshot.discordRevision;
    view.inviteOrigin=snapshot.inviteOrigin;
    view.discordCanSwitch=snapshot.discordCanSwitch; view.discordStatus=snapshot.discordStatus;
    view.inputDevice = snapshot.inputDevice; view.menuButtons = snapshot.menuController.menu;
    view.gameplayBrand = snapshot.gameplayBrand; view.promptBrand = snapshot.promptBrand;
    view.padHint = snapshot.padHint;
    view.padHintUntil = snapshot.padHintUntil; view.padLayer = snapshot.padLayer;
    view.padSetup = snapshot.padSetup; view.atMainMenu = snapshot.atMainMenu;
    view.padLayerPending = snapshot.padLayerPending; view.runningControllerLayer = snapshot.runningControllerLayer;
    view.lobbyMusicAvailable = snapshot.lobbyMusicAvailable;
    view.gameThemes = snapshot.gameThemes;
    view.gameThemeNotes = snapshot.gameThemeNotes;
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
    view.archiveBrowse = snapshot.archive;
    view.archiveFetch = snapshot.archiveFetch;
    view.levels = snapshot.levels;
    view.memberCards = snapshot.memberCards;
    view.profile = snapshot.profile;
    view.playerSearch = snapshot.playerSearch;
    view.recentOpponents = snapshot.recentOpponents;
    view.profileSets = snapshot.profileSets;
    view.profilesService = snapshot.profilesService;
    view.ownLink = snapshot.ownLink;
    view.friends = snapshot.friends;
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
        if (const auto* step = std::get_if<sf4e::ui::SelectionStep>(&action)) return game.StepSelection(*step, snapshot);
        // Training select's Hitboxes row: the game keeps the choice; nothing
        // is sent, and a game without the viewer is never asked.
        if (const auto* hitboxes = std::get_if<sf4e::ui::ShowHitboxes>(&action))
            return game::Game().caps.hitboxes && game.ShowHitboxes(hitboxes->shown);
        // Settings' Background artwork row: the game keeps the choice; nothing
        // is sent, and a game that does not offer the row is never asked.
        if (const auto* artwork = std::get_if<sf4e::ui::ShowMenuArtwork>(&action))
            return view.offersMenuArtwork && game.ShowMenuArtwork(artwork->shown);
        // Settings' Screen mode row: the game keeps the choice for its next
        // start; a game that does not offer the row is never asked.
        if (const auto* mode = std::get_if<sf4e::ui::SetScreenMode>(&action))
            return view.offersScreenMode && game.SetScreenMode(mode->mode);
        // Training select's Start: the game enters Training with these once
        // its menu is idle (ShellGame::StartTraining). Not from a room or match, nor
        // in a game without Training.
        if (std::holds_alternative<sf4e::ui::StartTraining>(action)) {
            if (!game::Game().caps.training || !snapshot.atMainMenu || snapshot.session.room != sf4e::netplay::RoomState::Idle || snapshot.session.match != sf4e::netplay::MatchState::None)
                return false;
            game.StartTraining(snapshot);
            close = "Training select";
            return true;
        }
        const bool training = std::holds_alternative<sf4e::commands::Training>(action);
        auto request = sf4e::ui::ForRuntime(std::move(action));
        if (!request || !Submit(std::move(*request), game)) return false;
        // Training from a room or Home: the menu closes behind it while the runtime
        // sends the game on.
        if (training) close = "Training";
        return true;
    }, [&] { game.DrawSelection(shell_.SelectingDummy(), snapshot); }, developer);
    if (shell_.BoardDrawn()) sf4e::NetplayFacade::NoteLatencyBoard(view.room.roomEpoch);
    if (shell_.SelectionDrawn()) sf4e::NetplayFacade::NoteFighterSelector(view.room.roomEpoch);
    if (shell_.ChatTyping()) sf4e::NetplayFacade::NoteChatTyping(view.room.roomEpoch);
    sf4e::NetplayFacade::NotePostMatchAnswer(shell_.PostMatchAnswer());
    roomCarry_.Return(view);
    if (!open) close = "Back on Ember's Home";
    return close;
}

} // namespace ember
