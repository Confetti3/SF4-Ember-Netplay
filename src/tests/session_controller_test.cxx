#include "../netplay/SessionController.hxx"
#include "../netplay/BoundedMailbox.hxx"
#include "../netplay/PlayerPreferences.hxx"

#include <cstdio>
#include <thread>
#include <atomic>
#include "test_support.hxx"

using namespace sf4e::netplay;

static Decision CommandNow(SessionController& controller, CommandKind kind, const char* invite = "") {
    Command command;
    command.kind = kind; command.generation = controller.GetSnapshot().generation; command.invitation = invite;
    return controller.Execute(command);
}
static void TestTypedLanJoin() {
    SessionController controller;
    Command join{CommandKind::JoinInvite};
    join.generation=controller.GetSnapshot().generation;
    join.lanHost="invalid";
    EXPECT(!controller.Execute(join).accepted);
    join.lanHost=std::string(64,'a');join.invitation="pasted invitation";
    EXPECT(!controller.Execute(join).accepted);
    join.invitation.clear();
    EXPECT(controller.Execute(join).effect==Effect::JoinInvite);
    EXPECT(!controller.Execute(join).accepted);
}
static Decision EventNow(SessionController& controller, EventKind kind) {
    Event event;
    event.kind = kind; event.generation = controller.GetSnapshot().generation;
    return controller.Apply(event);
}
// A battle that closes before GGPO reaches Running (the peer never
// synchronised) must still end the match. Rejecting MatchEnded from
// Preparing left the controller there for good, and Ready stays refused
// while the match state is not None or PostMatch.
static void TestMatchEndedFromPreparing() {
    SessionController controller;
    EXPECT(CommandNow(controller, CommandKind::HostRoom).accepted);
    EXPECT(EventNow(controller, EventKind::RoomJoined).accepted);
    EXPECT(CommandNow(controller, CommandKind::Ready).effect == Effect::SendReady);
    EXPECT(EventNow(controller, EventKind::MatchPreparing).accepted);
    EXPECT(controller.GetSnapshot().match == MatchState::Preparing);
    EXPECT(EventNow(controller, EventKind::MatchEnded).accepted);
    EXPECT(controller.GetSnapshot().match == MatchState::PostMatch);
    EXPECT(CommandNow(controller, CommandKind::Rematch).effect == Effect::SendReady);
    // From None there is still nothing to end.
    SessionController idle;
    EXPECT(CommandNow(idle, CommandKind::HostRoom).accepted);
    EXPECT(EventNow(idle, EventKind::RoomJoined).accepted);
    EXPECT(!EventNow(idle, EventKind::MatchEnded).accepted);
}

// A match still Playing or Preparing refuses the next grant. The runtime must
// end the old one (a battle that closed without its session, a start that
// failed) before a new grant can be entered, and once it does the grant is taken.
static void TestNextGrantNeedsTheOldMatchEnded() {
    SessionController controller;
    EXPECT(CommandNow(controller, CommandKind::HostRoom).accepted);
    EXPECT(EventNow(controller, EventKind::RoomJoined).accepted);
    EXPECT(EventNow(controller, EventKind::MatchPreparing).accepted);
    EXPECT(EventNow(controller, EventKind::GameplayReady).accepted);
    EXPECT(EventNow(controller, EventKind::MatchStarted).accepted);
    EXPECT(controller.GetSnapshot().match == MatchState::Playing);
    EXPECT(!EventNow(controller, EventKind::MatchPreparing).accepted);
    EXPECT(EventNow(controller, EventKind::MatchEnded).accepted);
    EXPECT(controller.GetSnapshot().match == MatchState::PostMatch);
    EXPECT(EventNow(controller, EventKind::MatchPreparing).accepted);
    // The same from Preparing.
    EXPECT(!EventNow(controller, EventKind::MatchPreparing).accepted);
    EXPECT(EventNow(controller, EventKind::MatchEnded).accepted);
    EXPECT(EventNow(controller, EventKind::MatchPreparing).accepted);
}

static void StartMatch(SessionController& controller) {
    EXPECT(CommandNow(controller, CommandKind::Ready).effect == Effect::SendReady);
    EXPECT(!CommandNow(controller, CommandKind::Ready).accepted);
    EXPECT(EventNow(controller, EventKind::MatchPreparing).accepted);
    EXPECT(!EventNow(controller, EventKind::MatchStarted).accepted);
    EXPECT(EventNow(controller, EventKind::GameplayReady).accepted);
    EXPECT(EventNow(controller, EventKind::MatchStarted).accepted);
}

// A room that is still being created or joined has no control to recover.
// Its forming coordination stream must not show the recovery banner or,
// 15 seconds later, offer to replace a room that was never joined.
static void TestOpeningIsNotRecovery() {
    SessionController joining;
    EXPECT(CommandNow(joining, CommandKind::JoinInvite, "sf4e3:invite").accepted);
    EXPECT(joining.ObserveCoordination(1, 1, false, 100));
    EXPECT(joining.GetSnapshot().recovery == Recovery::None);
    EXPECT(joining.GetSnapshot().control == Health::Connecting);
    EXPECT(joining.GetSnapshot().error.empty());
    EXPECT(!joining.GetSnapshot().authorityWritable);
    EXPECT(joining.ObserveCoordination(1, 1, false, 20000));
    joining.AdvanceRecovery(20000);
    EXPECT(joining.GetSnapshot().recovery == Recovery::None);
    EXPECT(!CommandNow(joining, CommandKind::ReplaceRoom).accepted);
    // A long wait is named, not ended: a slow relay can still finish.
    EXPECT(joining.ObserveCoordination(1, 1, false, 100 + SessionController::OpeningStallMs - 1));
    EXPECT(!joining.GetSnapshot().openingStalled);
    EXPECT(joining.ObserveCoordination(1, 1, false, 100 + SessionController::OpeningStallMs));
    EXPECT(joining.GetSnapshot().openingStalled && joining.GetSnapshot().room == RoomState::Opening);
    EXPECT(joining.GetSnapshot().recovery == Recovery::None && joining.GetSnapshot().error.empty());
    EXPECT(!joining.ControlPlaneEstablished());
    // Stopping the join still works while the stream forms.
    EXPECT(CommandNow(joining, CommandKind::LeaveRoom).accepted);

    // Once joined, the same loss is a real outage and recovers as before.
    SessionController joined;
    EXPECT(CommandNow(joined, CommandKind::JoinInvite, "sf4e3:invite").accepted);
    EXPECT(joined.ObserveCoordination(1, 1, true, 100));
    EXPECT(EventNow(joined, EventKind::RoomJoined).accepted);
    EXPECT(joined.ObserveCoordination(1, 1, false, 1000));
    EXPECT(joined.ControlPlaneEstablished() && !joined.GetSnapshot().openingStalled);
    EXPECT(joined.GetSnapshot().recovery == Recovery::Recovering);
    EXPECT(joined.GetSnapshot().control == Health::Lost);
    // The condition is a kind the interface words in the player's language, not
    // an English sentence the controller writes into `error`.
    EXPECT(joined.GetSnapshot().fault == Fault::ControlRecovering && joined.GetSnapshot().error.empty());
    // Leaving during the outage leaves nothing of it for the menu to show.
    EXPECT(CommandNow(joined, CommandKind::LeaveRoom).accepted);
    EXPECT(joined.GetSnapshot().fault == Fault::None && joined.GetSnapshot().error.empty());
    EXPECT(EventNow(joined, EventKind::RoomClosed).accepted);
    EXPECT(joined.GetSnapshot().fault == Fault::None && joined.GetSnapshot().error.empty());
    // A failure the room reported is the player's to read after the room closes.
    SessionController failed;
    EXPECT(CommandNow(failed, CommandKind::JoinInvite, "sf4e3:invite").accepted);
    Event failure;
    failure.kind = EventKind::RoomFailed; failure.generation = failed.GetSnapshot().generation; failure.error = "Could not join";
    EXPECT(failed.Apply(failure).accepted);
    EXPECT(EventNow(failed, EventKind::RoomClosed).accepted);
    EXPECT(failed.GetSnapshot().error == "Could not join");
}

// A server-owned room has no replacement host: its recovery never offers one,
// and a ReplaceRoom command is refused. Leaving still closes the room.
static void TestServerOwnedRoomIsNeverReplaced() {
    SessionController room;
    EXPECT(CommandNow(room, CommandKind::JoinInvite, "sf4e3:invitation").accepted);
    room.SetServerOwned(true);
    EXPECT(room.ObserveCoordination(1, 1, true, 100));
    EXPECT(EventNow(room, EventKind::RoomJoined).accepted);
    EXPECT(room.ObserveCoordination(1, 1, false, 1000));
    EXPECT(room.GetSnapshot().recovery == Recovery::Recovering && room.GetSnapshot().fault == Fault::ControlRecovering);
    room.AdvanceRecovery(16000);
    EXPECT(room.GetSnapshot().recovery == Recovery::Recovering);
    EXPECT(room.ObserveCoordination(1, 1, false, 40000));
    EXPECT(room.GetSnapshot().recovery == Recovery::Recovering);
    EXPECT(!CommandNow(room, CommandKind::ReplaceRoom).accepted);
    // A connected stream whose checkpoint never applies offers nothing either.
    EXPECT(room.ObserveCoordination(1, 2, true, 41000));
    EXPECT(room.ObserveCoordination(1, 3, true, 42000, false));
    EXPECT(room.ObserveCoordination(1, 4, true, 80000, false));
    EXPECT(room.GetSnapshot().recovery == Recovery::None && room.GetSnapshot().fault == Fault::CatchingUp);
    EXPECT(!CommandNow(room, CommandKind::ReplaceRoom).accepted);
    EXPECT(CommandNow(room, CommandKind::LeaveRoom).effect == Effect::CloseSession);
    EXPECT(EventNow(room, EventKind::RoomClosed).accepted);
    EXPECT(!room.GetSnapshot().serverOwned && room.GetSnapshot().room == RoomState::Idle);

    // A private room keeps its offer.
    SessionController priv;
    EXPECT(CommandNow(priv, CommandKind::JoinInvite, "sf4e3:invitation").accepted);
    priv.SetServerOwned(false);
    EXPECT(priv.ObserveCoordination(1, 1, true, 100));
    EXPECT(EventNow(priv, EventKind::RoomJoined).accepted);
    EXPECT(priv.ObserveCoordination(1, 1, false, 1000));
    priv.AdvanceRecovery(16000);
    EXPECT(CommandNow(priv, CommandKind::ReplaceRoom).effect == Effect::ReplaceRoom);
}

int main() {
    TestTypedLanJoin();
    TestMatchEndedFromPreparing();
    TestServerOwnedRoomIsNeverReplaced();
    TestNextGrantNeedsTheOldMatchEnded();
    TestOpeningIsNotRecovery();
    // Ordinary checkpoint delivery can trail the healthy coordination watch.
    // It pauses mutation, but must not announce a lost connection or erase Ready.
    SessionController syncing;
    EXPECT(CommandNow(syncing, CommandKind::HostRoom).accepted);
    EXPECT(syncing.ObserveCoordination(1, 1, true, 100));
    EXPECT(EventNow(syncing, EventKind::RoomJoined).accepted);
    EXPECT(CommandNow(syncing, CommandKind::Ready).accepted);
    // The few ticks between a commit and its checkpoint stay writable, so
    // room controls do not flicker with every commit.
    EXPECT(syncing.ObserveCoordination(1, 2, true, 200, false));
    EXPECT(syncing.GetSnapshot().authorityWritable);
    EXPECT(syncing.ObserveCoordination(1, 2, true, 449, false));
    EXPECT(syncing.GetSnapshot().authorityWritable);
    EXPECT(syncing.ObserveCoordination(1, 2, true, 450, false));
    EXPECT(syncing.GetSnapshot().control == Health::Healthy);
    EXPECT(syncing.GetSnapshot().recovery == Recovery::None);
    EXPECT(syncing.GetSnapshot().readyPending);
    EXPECT(!syncing.GetSnapshot().authorityWritable);
    EXPECT(!CommandNow(syncing, CommandKind::RoomAction).accepted);
    EXPECT(syncing.ObserveCoordination(1, 2, true, 460, true));
    EXPECT(syncing.GetSnapshot().authorityWritable && syncing.GetSnapshot().authorityStalledMs == 0);
    EXPECT(syncing.GetSnapshot().readyPending);

    // A completed local abort must return to the room even if the remote
    // authority vanished. Otherwise the UI remains Playing forever.
    SessionController dropped;
    EXPECT(CommandNow(dropped, CommandKind::HostRoom).accepted);
    EXPECT(dropped.ObserveCoordination(1, 1, true, 100));
    EXPECT(EventNow(dropped, EventKind::RoomJoined).accepted);
    StartMatch(dropped);
    EXPECT(dropped.ObserveCoordination(1, 1, false, 1000));
    EXPECT(EventNow(dropped, EventKind::MatchRecovered).accepted);
    EXPECT(dropped.GetSnapshot().match == MatchState::PostMatch);
    EXPECT(dropped.GetSnapshot().control == Health::Lost);
    EXPECT(!CommandNow(dropped, CommandKind::Ready).accepted);
    dropped.AdvanceRecovery(16000);
    EXPECT(CommandNow(dropped, CommandKind::ReplaceRoom).accepted);

    // Coordination loss pauses room commands without closing healthy gameplay.
    // The same applied authority state must not reset the 15-second offer.
    SessionController migration;
    EXPECT(CommandNow(migration, CommandKind::HostRoom).accepted);
    EXPECT(migration.ObserveCoordination(1, 1, true, 100));
    EXPECT(EventNow(migration, EventKind::RoomJoined).accepted);
    StartMatch(migration);
    const auto activeGeneration = migration.GetSnapshot().generation;
    EXPECT(migration.ObserveCoordination(1, 1, false, 1000));
    EXPECT(migration.GetSnapshot().match == MatchState::Playing);
    EXPECT(migration.GetSnapshot().generation == activeGeneration);
    EXPECT(!CommandNow(migration, CommandKind::Ready).accepted);
    EXPECT(!CommandNow(migration, CommandKind::RoomAction).accepted);
    EXPECT(!CommandNow(migration, CommandKind::SetLobbySettings).accepted);
    EXPECT(!CommandNow(migration, CommandKind::ReplaceRoom).accepted);
    EXPECT(migration.ObserveCoordination(1, 1, false, 15999));
    EXPECT(migration.GetSnapshot().recovery == Recovery::Recovering);
    migration.AdvanceRecovery(16000);
    EXPECT(migration.GetSnapshot().recovery == Recovery::ReplacementOffered);
    EXPECT(!CommandNow(migration, CommandKind::ReplaceRoom).accepted);
    EXPECT(EventNow(migration, EventKind::MatchEnded).effect == Effect::None);
    EXPECT(migration.GetSnapshot().match == MatchState::PostMatch);
    EXPECT(migration.ObserveCoordination(2, 2, true, 17000));
    EXPECT(migration.GetSnapshot().recovery == Recovery::None);
    EXPECT(migration.GetSnapshot().generation == activeGeneration);
    EXPECT(!migration.ObserveCoordination(1, 99, true, 18000));
    EXPECT(!CommandNow(migration, CommandKind::ReplaceRoom).accepted);
    EXPECT(CommandNow(migration, CommandKind::CheckConnection).accepted);
    EXPECT(migration.ObserveCoordination(2, 2, false, 19000));
    migration.AdvanceRecovery(34000);
    EXPECT(CommandNow(migration, CommandKind::ReplaceRoom).effect == Effect::ReplaceRoom);
    EXPECT(!CommandNow(migration, CommandKind::HostRoom).accepted);
    EXPECT(EventNow(migration, EventKind::RoomClosed).accepted);
    EXPECT(CommandNow(migration, CommandKind::HostRoom).accepted);

    // Room controls stay available while one table plays. Recovering that
    // table preserves the room epoch and rejects callbacks from earlier games.
    SessionController rooms;
    EXPECT(!CommandNow(rooms, CommandKind::RoomAction).accepted);
    EXPECT(CommandNow(rooms, CommandKind::HostRoom).accepted);
    EXPECT(EventNow(rooms, EventKind::RoomJoined).accepted);
    StartMatch(rooms);
    const auto roomGeneration = rooms.GetSnapshot().generation;
    EXPECT(CommandNow(rooms, CommandKind::RoomAction).effect == Effect::SendRoomAction);
    EXPECT(EventNow(rooms, EventKind::MatchRecovered).accepted);
    EXPECT(rooms.GetSnapshot().room == RoomState::Joined && rooms.GetSnapshot().generation == roomGeneration);
    EXPECT(!rooms.GetSnapshot().verificationAvailable && rooms.GetSnapshot().match == MatchState::PostMatch);
    StartMatch(rooms);
    Event obsolete{EventKind::MatchRecovered, roomGeneration, {}};
    EXPECT(!rooms.Apply(obsolete).accepted);
    EXPECT(EventNow(rooms, EventKind::ControlLost).accepted);
    EXPECT(!CommandNow(rooms, CommandKind::RoomAction).accepted);
    PlayerPreferences preferences;
    EXPECT(preferences.Valid());
    preferences.displayName = ""; EXPECT(!preferences.Valid());
    preferences.displayName = std::string(32, 'x'); EXPECT(!preferences.Valid());
    preferences.displayName = "Name\n"; EXPECT(!preferences.Valid());
    preferences.displayName = "Player";
    preferences.inputDelay = 0; EXPECT(preferences.Valid());
    preferences.inputDelay = 1; EXPECT(preferences.Valid());
    preferences.inputDelay = -1; EXPECT(!preferences.Valid());
    preferences.inputDelay = 11; EXPECT(!preferences.Valid());
    preferences.inputDelay = sf4e::MaximumInputDelay + 1; EXPECT(!preferences.Valid());
    preferences.inputDelay = sf4e::MaximumInputDelay;
    preferences.lobby.roundCount = 99; preferences.lobby.roundTime = 9999; EXPECT(preferences.Valid());
    preferences.lobby.roundTime = 0; EXPECT(!preferences.Valid());
    preferences.lobby.roundTime = 99; preferences.lobby.roundCount = 2; EXPECT(!preferences.Valid());
    SessionController settingsController;
    EXPECT(CommandNow(settingsController, CommandKind::SavePreferences).effect == Effect::SavePreferences);
    EXPECT(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    EXPECT(CommandNow(settingsController, CommandKind::HostRoom).accepted);
    EXPECT(!CommandNow(settingsController, CommandKind::SavePreferences).accepted);
    EXPECT(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    EXPECT(EventNow(settingsController, EventKind::RoomJoined).accepted);
    EXPECT(CommandNow(settingsController, CommandKind::SetLobbySettings).effect == Effect::SetLobbySettings);
    EXPECT(CommandNow(settingsController, CommandKind::Ready).accepted);
    EXPECT(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    EXPECT(EventNow(settingsController, EventKind::MatchPreparing).accepted);
    EXPECT(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    EXPECT(!CommandNow(settingsController, CommandKind::SavePreferences).accepted);
    SessionController controller;
    EXPECT(CommandNow(controller, CommandKind::StartOffline).effect == Effect::StartOffline);
    EXPECT(!CommandNow(controller, CommandKind::JoinInvite).accepted);
    EXPECT(CommandNow(controller, CommandKind::HostRoom).effect == Effect::HostRoom);
    const Generation firstRoom = controller.GetSnapshot().generation;
    EXPECT(!CommandNow(controller, CommandKind::HostRoom).accepted);
    EXPECT(CommandNow(controller, CommandKind::LeaveRoom).effect == Effect::CloseSession);
    EXPECT(!EventNow(controller, EventKind::RoomJoined).accepted);
    EXPECT(!CommandNow(controller, CommandKind::StartOffline).accepted);
    EXPECT(EventNow(controller, EventKind::RoomClosed).accepted);
    EXPECT(CommandNow(controller, CommandKind::JoinInvite, "sf4e1:private-capability").effect == Effect::JoinInvite);
    Event stale; stale.kind = EventKind::RoomJoined; stale.generation = firstRoom;
    EXPECT(!controller.Apply(stale).accepted);
    EXPECT(EventNow(controller, EventKind::RoomJoined).accepted);
    StartMatch(controller);
    for (int match = 0; match < 50; ++match) {
        const Generation oldMatch = controller.GetSnapshot().generation;
        EXPECT(EventNow(controller, EventKind::MatchEnded).accepted);
        EXPECT(CommandNow(controller, CommandKind::Rematch).effect == Effect::SendReady);
        EXPECT(EventNow(controller, EventKind::MatchPreparing).accepted);
        stale.kind = EventKind::GameplayLost; stale.generation = oldMatch;
        EXPECT(!controller.Apply(stale).accepted);
        EXPECT(EventNow(controller, EventKind::GameplayReady).accepted);
        EXPECT(EventNow(controller, EventKind::MatchStarted).accepted);
    }
    EXPECT(controller.GetSnapshot().match == MatchState::Playing);
    Event lost;
    lost.kind = EventKind::ControlLost;
    lost.generation.room = controller.GetSnapshot().generation.room;
    lost.generation.match = 0; // Worker was started before the first match.
    const Decision degraded = controller.Apply(lost);
    EXPECT(degraded.accepted && degraded.effect == Effect::None);
    EXPECT(controller.GetSnapshot().match == MatchState::Playing);
    EXPECT(!controller.GetSnapshot().verificationAvailable);
    EXPECT(!CommandNow(controller, CommandKind::Rematch).accepted);
    EXPECT(EventNow(controller, EventKind::MatchEnded).effect == Effect::FinishDegradedMatch);
    EXPECT(EventNow(controller, EventKind::RoomClosed).accepted);
    EXPECT(CommandNow(controller, CommandKind::HostRoom).accepted);
    EXPECT(EventNow(controller, EventKind::RoomJoined).accepted);
    StartMatch(controller);
    EXPECT(EventNow(controller, EventKind::GameplayLost).effect == Effect::AbortMatch);
    EXPECT(controller.GetSnapshot().match == MatchState::Failed);
    EXPECT(!EventNow(controller, EventKind::GameplayReady).accepted);
    // A game cut short has no PostMatch: the closed room goes straight to
    // None, which is where Ember's menu comes back (OverlayPresentation).
    EXPECT(EventNow(controller, EventKind::RoomClosed).accepted);
    EXPECT(controller.GetSnapshot().match == MatchState::None);
    EXPECT(EventNow(controller, EventKind::HelperLost).accepted);
    EXPECT(CommandNow(controller, CommandKind::StartOffline).accepted);

    BoundedMailbox<int> queue(2, 8);
    EXPECT(queue.TryPush(1, 4)); EXPECT(queue.TryPush(2, 4));
    EXPECT(!queue.TryPush(3, 1));
    int value = 0; EXPECT(queue.TryPop(value) && value == 1);
    EXPECT(!queue.TryPush(3, 5));
    EXPECT(queue.TryPush(3, 4));
    EXPECT(queue.TryPop(value) && value == 2); EXPECT(queue.TryPop(value) && value == 3);
    EXPECT(!queue.TryPush(0, 0));
    EXPECT(queue.Rejections() == 3);
    queue.Close(); EXPECT(!queue.TryPush(4, 1)); EXPECT(!queue.TryPop(value));

    BoundedMailbox<int> concurrent(32, 128);
    std::atomic<bool> producerDone(false);
    std::thread producer([&]() {
        for (int i = 0; i < 10000; ++i) {
            while (!concurrent.TryPush(i, sizeof(i))) { std::this_thread::yield(); }
        }
        producerDone = true;
    });
    int expected = 0;
    while (expected < 10000) {
        if (concurrent.TryPop(value)) { EXPECT(value == expected); ++expected; }
        else { std::this_thread::yield(); }
    }
    producer.join(); EXPECT(producerDone); concurrent.Close();
    // A connected same-term stream whose checkpoint never applies locally used to
    // fence every room mutation for good: recovery None, no error, and Replace
    // room refused, so the player sat in the lobby unable to queue, ready or
    // watch. Brief lag must stay silent; a persistent stall must become visible
    // and recoverable.
    {
        SessionController stalled;
        EXPECT(CommandNow(stalled, CommandKind::HostRoom).accepted);
        EXPECT(stalled.ObserveCoordination(1, 1, true, 1000));
        EXPECT(EventNow(stalled, EventKind::RoomJoined).accepted);
        EXPECT(stalled.GetSnapshot().authorityWritable);

        // Ordinary lag: fenced, but no alarm and nothing to recover from.
        EXPECT(stalled.ObserveCoordination(1, 2, true, 1500, false));
        EXPECT(stalled.GetSnapshot().authorityWritable);
        EXPECT(stalled.ObserveCoordination(1, 2, true, 1750, false));
        EXPECT(!stalled.GetSnapshot().authorityWritable);
        EXPECT(stalled.GetSnapshot().control == Health::Healthy);
        EXPECT(stalled.GetSnapshot().recovery == Recovery::None);
        EXPECT(stalled.GetSnapshot().error.empty() && stalled.GetSnapshot().fault == Fault::None);
        EXPECT(!CommandNow(stalled, CommandKind::RoomAction).accepted);
        // H-006: the runtime can tell a fenced refusal from any other, so it
        // parks or reports it instead of dropping it silently.
        {
            Command fenced;
            fenced.generation = stalled.GetSnapshot().generation;
            for (CommandKind kind : {CommandKind::RoomAction, CommandKind::SetLobbySettings, CommandKind::ApplyDelay,
                     CommandKind::CheckConnection, CommandKind::Ready, CommandKind::Rematch}) {
                fenced.kind = kind;
                EXPECT(stalled.FencedOut(fenced));
                const auto decision = stalled.Execute(fenced);
                EXPECT(!decision.accepted && decision.refusal == Refusal::Fenced);
            }
            fenced.kind = CommandKind::LeaveRoom;
            EXPECT(!stalled.FencedOut(fenced));
            fenced.kind = CommandKind::SetLobbySettings;
            fenced.generation.match++;
            EXPECT(!stalled.FencedOut(fenced));
            EXPECT(stalled.Execute(fenced).refusal == Refusal::Rejected);
        }

        // Catching up clears the stall without leaving residue.
        EXPECT(stalled.ObserveCoordination(1, 3, true, 2000));
        EXPECT(stalled.GetSnapshot().authorityWritable);
        EXPECT(stalled.GetSnapshot().authorityStalledMs == 0);
        {
            Command writable;
            writable.kind = CommandKind::SetLobbySettings;
            writable.generation = stalled.GetSnapshot().generation;
            EXPECT(!stalled.FencedOut(writable));
        }

        // A stall that persists is named at ten seconds...
        EXPECT(stalled.ObserveCoordination(1, 4, true, 3000, false));
        EXPECT(stalled.GetSnapshot().fault == Fault::None);
        EXPECT(stalled.ObserveCoordination(1, 5, true, 12000, false));
        EXPECT(stalled.GetSnapshot().fault == Fault::None);
        EXPECT(stalled.ObserveCoordination(1, 6, true, 13000, false));
        EXPECT(stalled.GetSnapshot().fault == Fault::CatchingUp && stalled.GetSnapshot().error.empty());
        EXPECT(stalled.GetSnapshot().recovery == Recovery::None);

        // ...and offers a way out at thirty, without ever claiming a disconnect.
        EXPECT(stalled.ObserveCoordination(1, 7, true, 33000, false));
        EXPECT(stalled.GetSnapshot().recovery == Recovery::ReplacementOffered);
        EXPECT(stalled.GetSnapshot().control == Health::Healthy);
        EXPECT(CommandNow(stalled, CommandKind::ReplaceRoom).accepted);
    }

    std::printf("SessionController: %d failure(s)\n", TEST_FAILURES());
    return TEST_EXIT_CODE();
}
