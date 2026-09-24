#include "../netplay/SessionController.hxx"
#include "../netplay/BoundedMailbox.hxx"
#include "../netplay/PlayerPreferences.hxx"

#include <cstdio>
#include <thread>
#include <atomic>

using namespace sf4e::netplay;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static Decision CommandNow(SessionController& controller, CommandKind kind, const char* invite = "") {
    Command command;
    command.kind = kind; command.generation = controller.GetSnapshot().generation; command.invitation = invite;
    return controller.Execute(command);
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
    CHECK(CommandNow(controller, CommandKind::HostRoom).accepted);
    CHECK(EventNow(controller, EventKind::RoomJoined).accepted);
    CHECK(CommandNow(controller, CommandKind::Ready).effect == Effect::SendReady);
    CHECK(EventNow(controller, EventKind::MatchPreparing).accepted);
    CHECK(controller.GetSnapshot().match == MatchState::Preparing);
    CHECK(EventNow(controller, EventKind::MatchEnded).accepted);
    CHECK(controller.GetSnapshot().match == MatchState::PostMatch);
    CHECK(CommandNow(controller, CommandKind::Rematch).effect == Effect::SendReady);
    // From None there is still nothing to end.
    SessionController idle;
    CHECK(CommandNow(idle, CommandKind::HostRoom).accepted);
    CHECK(EventNow(idle, EventKind::RoomJoined).accepted);
    CHECK(!EventNow(idle, EventKind::MatchEnded).accepted);
}

static void StartMatch(SessionController& controller) {
    CHECK(CommandNow(controller, CommandKind::Ready).effect == Effect::SendReady);
    CHECK(!CommandNow(controller, CommandKind::Ready).accepted);
    CHECK(EventNow(controller, EventKind::MatchPreparing).accepted);
    CHECK(!EventNow(controller, EventKind::MatchStarted).accepted);
    CHECK(EventNow(controller, EventKind::GameplayReady).accepted);
    CHECK(EventNow(controller, EventKind::MatchStarted).accepted);
}

int main() {
    TestMatchEndedFromPreparing();
    // Ordinary checkpoint delivery can trail the healthy coordination watch.
    // It pauses mutation, but must not announce a lost connection or erase Ready.
    SessionController syncing;
    CHECK(CommandNow(syncing, CommandKind::HostRoom).accepted);
    CHECK(syncing.ObserveCoordination(1, 1, true, 100));
    CHECK(EventNow(syncing, EventKind::RoomJoined).accepted);
    CHECK(CommandNow(syncing, CommandKind::Ready).accepted);
    // The few ticks between a commit and its checkpoint stay writable, so
    // room controls do not flicker with every commit.
    CHECK(syncing.ObserveCoordination(1, 2, true, 200, false));
    CHECK(syncing.GetSnapshot().authorityWritable);
    CHECK(syncing.ObserveCoordination(1, 2, true, 449, false));
    CHECK(syncing.GetSnapshot().authorityWritable);
    CHECK(syncing.ObserveCoordination(1, 2, true, 450, false));
    CHECK(syncing.GetSnapshot().control == Health::Healthy);
    CHECK(syncing.GetSnapshot().recovery == Recovery::None);
    CHECK(syncing.GetSnapshot().readyPending);
    CHECK(!syncing.GetSnapshot().authorityWritable);
    CHECK(!CommandNow(syncing, CommandKind::RoomAction).accepted);
    CHECK(syncing.ObserveCoordination(1, 2, true, 460, true));
    CHECK(syncing.GetSnapshot().authorityWritable && syncing.GetSnapshot().authorityStalledMs == 0);
    CHECK(syncing.GetSnapshot().readyPending);

    // A completed local abort must return to the room even if the remote
    // authority vanished. Otherwise the UI remains Playing forever.
    SessionController dropped;
    CHECK(CommandNow(dropped, CommandKind::HostRoom).accepted);
    CHECK(dropped.ObserveCoordination(1, 1, true, 100));
    CHECK(EventNow(dropped, EventKind::RoomJoined).accepted);
    StartMatch(dropped);
    CHECK(dropped.ObserveCoordination(1, 1, false, 1000));
    CHECK(EventNow(dropped, EventKind::MatchRecovered).accepted);
    CHECK(dropped.GetSnapshot().match == MatchState::PostMatch);
    CHECK(dropped.GetSnapshot().control == Health::Lost);
    CHECK(!CommandNow(dropped, CommandKind::Ready).accepted);
    dropped.AdvanceRecovery(16000);
    CHECK(CommandNow(dropped, CommandKind::ReplaceRoom).accepted);

    // Coordination loss pauses room commands without closing healthy gameplay.
    // The same applied authority state must not reset the 15-second offer.
    SessionController migration;
    CHECK(CommandNow(migration, CommandKind::HostRoom).accepted);
    CHECK(migration.ObserveCoordination(1, 1, true, 100));
    CHECK(EventNow(migration, EventKind::RoomJoined).accepted);
    StartMatch(migration);
    const auto activeGeneration = migration.GetSnapshot().generation;
    CHECK(migration.ObserveCoordination(1, 1, false, 1000));
    CHECK(migration.GetSnapshot().match == MatchState::Playing);
    CHECK(migration.GetSnapshot().generation == activeGeneration);
    CHECK(!CommandNow(migration, CommandKind::Ready).accepted);
    CHECK(!CommandNow(migration, CommandKind::RoomAction).accepted);
    CHECK(!CommandNow(migration, CommandKind::SetLobbySettings).accepted);
    CHECK(!CommandNow(migration, CommandKind::ReplaceRoom).accepted);
    CHECK(migration.ObserveCoordination(1, 1, false, 15999));
    CHECK(migration.GetSnapshot().recovery == Recovery::Recovering);
    migration.AdvanceRecovery(16000);
    CHECK(migration.GetSnapshot().recovery == Recovery::ReplacementOffered);
    CHECK(!CommandNow(migration, CommandKind::ReplaceRoom).accepted);
    CHECK(EventNow(migration, EventKind::MatchEnded).effect == Effect::None);
    CHECK(migration.GetSnapshot().match == MatchState::PostMatch);
    CHECK(migration.ObserveCoordination(2, 2, true, 17000));
    CHECK(migration.GetSnapshot().recovery == Recovery::None);
    CHECK(migration.GetSnapshot().generation == activeGeneration);
    CHECK(!migration.ObserveCoordination(1, 99, true, 18000));
    CHECK(!CommandNow(migration, CommandKind::ReplaceRoom).accepted);
    CHECK(CommandNow(migration, CommandKind::CheckConnection).accepted);
    CHECK(migration.ObserveCoordination(2, 2, false, 19000));
    migration.AdvanceRecovery(34000);
    CHECK(CommandNow(migration, CommandKind::ReplaceRoom).effect == Effect::ReplaceRoom);
    CHECK(!CommandNow(migration, CommandKind::HostRoom).accepted);
    CHECK(EventNow(migration, EventKind::RoomClosed).accepted);
    CHECK(CommandNow(migration, CommandKind::HostRoom).accepted);

    // Room controls stay available while one table plays. Recovering that
    // table preserves the room epoch and rejects callbacks from earlier games.
    SessionController rooms;
    CHECK(!CommandNow(rooms, CommandKind::RoomAction).accepted);
    CHECK(CommandNow(rooms, CommandKind::HostRoom).accepted);
    CHECK(EventNow(rooms, EventKind::RoomJoined).accepted);
    StartMatch(rooms);
    const auto roomGeneration = rooms.GetSnapshot().generation;
    CHECK(CommandNow(rooms, CommandKind::RoomAction).effect == Effect::SendRoomAction);
    CHECK(EventNow(rooms, EventKind::MatchRecovered).accepted);
    CHECK(rooms.GetSnapshot().room == RoomState::Joined && rooms.GetSnapshot().generation == roomGeneration);
    CHECK(!rooms.GetSnapshot().verificationAvailable && rooms.GetSnapshot().match == MatchState::PostMatch);
    StartMatch(rooms);
    Event obsolete{EventKind::MatchRecovered, roomGeneration, {}};
    CHECK(!rooms.Apply(obsolete).accepted);
    CHECK(EventNow(rooms, EventKind::ControlLost).accepted);
    CHECK(!CommandNow(rooms, CommandKind::RoomAction).accepted);
    PlayerPreferences preferences;
    CHECK(preferences.Valid());
    preferences.displayName = ""; CHECK(!preferences.Valid());
    preferences.displayName = std::string(32, 'x'); CHECK(!preferences.Valid());
    preferences.displayName = "Name\n"; CHECK(!preferences.Valid());
    preferences.displayName = "Player";
    preferences.inputDelay = 0; CHECK(preferences.Valid());
    preferences.inputDelay = -1; CHECK(!preferences.Valid());
    preferences.inputDelay = 11; CHECK(!preferences.Valid());
    preferences.inputDelay = 10;
    preferences.lobby.roundCount = 99; preferences.lobby.roundTime = 9999; CHECK(preferences.Valid());
    preferences.lobby.roundTime = 0; CHECK(!preferences.Valid());
    preferences.lobby.roundTime = 99; preferences.lobby.roundCount = 2; CHECK(!preferences.Valid());
    SessionController settingsController;
    CHECK(CommandNow(settingsController, CommandKind::SavePreferences).effect == Effect::SavePreferences);
    CHECK(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    CHECK(CommandNow(settingsController, CommandKind::HostRoom).accepted);
    CHECK(!CommandNow(settingsController, CommandKind::SavePreferences).accepted);
    CHECK(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    CHECK(EventNow(settingsController, EventKind::RoomJoined).accepted);
    CHECK(CommandNow(settingsController, CommandKind::SetLobbySettings).effect == Effect::SetLobbySettings);
    CHECK(CommandNow(settingsController, CommandKind::Ready).accepted);
    CHECK(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    CHECK(EventNow(settingsController, EventKind::MatchPreparing).accepted);
    CHECK(!CommandNow(settingsController, CommandKind::SetLobbySettings).accepted);
    CHECK(!CommandNow(settingsController, CommandKind::SavePreferences).accepted);
    SessionController controller;
    CHECK(CommandNow(controller, CommandKind::StartOffline).effect == Effect::StartOffline);
    CHECK(!CommandNow(controller, CommandKind::JoinInvite).accepted);
    CHECK(CommandNow(controller, CommandKind::HostRoom).effect == Effect::HostRoom);
    const Generation firstRoom = controller.GetSnapshot().generation;
    CHECK(!CommandNow(controller, CommandKind::HostRoom).accepted);
    CHECK(CommandNow(controller, CommandKind::LeaveRoom).effect == Effect::CloseSession);
    CHECK(!EventNow(controller, EventKind::RoomJoined).accepted);
    CHECK(!CommandNow(controller, CommandKind::StartOffline).accepted);
    CHECK(EventNow(controller, EventKind::RoomClosed).accepted);
    CHECK(CommandNow(controller, CommandKind::JoinInvite, "sf4e1:private-capability").effect == Effect::JoinInvite);
    Event stale; stale.kind = EventKind::RoomJoined; stale.generation = firstRoom;
    CHECK(!controller.Apply(stale).accepted);
    CHECK(EventNow(controller, EventKind::RoomJoined).accepted);
    StartMatch(controller);
    for (int match = 0; match < 50; ++match) {
        const Generation oldMatch = controller.GetSnapshot().generation;
        CHECK(EventNow(controller, EventKind::MatchEnded).accepted);
        CHECK(CommandNow(controller, CommandKind::Rematch).effect == Effect::SendReady);
        CHECK(EventNow(controller, EventKind::MatchPreparing).accepted);
        stale.kind = EventKind::GameplayLost; stale.generation = oldMatch;
        CHECK(!controller.Apply(stale).accepted);
        CHECK(EventNow(controller, EventKind::GameplayReady).accepted);
        CHECK(EventNow(controller, EventKind::MatchStarted).accepted);
    }
    CHECK(controller.GetSnapshot().match == MatchState::Playing);
    Event lost;
    lost.kind = EventKind::ControlLost;
    lost.generation.room = controller.GetSnapshot().generation.room;
    lost.generation.match = 0; // Worker was started before the first match.
    const Decision degraded = controller.Apply(lost);
    CHECK(degraded.accepted && degraded.effect == Effect::None);
    CHECK(controller.GetSnapshot().match == MatchState::Playing);
    CHECK(!controller.GetSnapshot().verificationAvailable);
    CHECK(!CommandNow(controller, CommandKind::Rematch).accepted);
    CHECK(EventNow(controller, EventKind::MatchEnded).effect == Effect::FinishDegradedMatch);
    CHECK(EventNow(controller, EventKind::RoomClosed).accepted);
    CHECK(CommandNow(controller, CommandKind::HostRoom).accepted);
    CHECK(EventNow(controller, EventKind::RoomJoined).accepted);
    StartMatch(controller);
    CHECK(EventNow(controller, EventKind::GameplayLost).effect == Effect::AbortMatch);
    CHECK(!EventNow(controller, EventKind::GameplayReady).accepted);
    CHECK(EventNow(controller, EventKind::RoomClosed).accepted);
    CHECK(EventNow(controller, EventKind::HelperLost).accepted);
    CHECK(CommandNow(controller, CommandKind::StartOffline).accepted);

    BoundedMailbox<int> queue(2, 8);
    CHECK(queue.TryPush(1, 4)); CHECK(queue.TryPush(2, 4));
    CHECK(!queue.TryPush(3, 1));
    int value = 0; CHECK(queue.TryPop(value) && value == 1);
    CHECK(!queue.TryPush(3, 5));
    CHECK(queue.TryPush(3, 4));
    CHECK(queue.TryPop(value) && value == 2); CHECK(queue.TryPop(value) && value == 3);
    CHECK(!queue.TryPush(0, 0));
    CHECK(queue.Rejections() == 3);
    queue.Close(); CHECK(!queue.TryPush(4, 1)); CHECK(!queue.TryPop(value));

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
        if (concurrent.TryPop(value)) { CHECK(value == expected); ++expected; }
        else { std::this_thread::yield(); }
    }
    producer.join(); CHECK(producerDone); concurrent.Close();
    // A connected same-term stream whose checkpoint never applies locally used to
    // fence every room mutation for good: recovery None, no error, and Replace
    // room refused, so the player sat in the lobby unable to queue, ready or
    // watch. Brief lag must stay silent; a persistent stall must become visible
    // and recoverable.
    {
        SessionController stalled;
        CHECK(CommandNow(stalled, CommandKind::HostRoom).accepted);
        CHECK(stalled.ObserveCoordination(1, 1, true, 1000));
        CHECK(EventNow(stalled, EventKind::RoomJoined).accepted);
        CHECK(stalled.GetSnapshot().authorityWritable);

        // Ordinary lag: fenced, but no alarm and nothing to recover from.
        CHECK(stalled.ObserveCoordination(1, 2, true, 1500, false));
        CHECK(stalled.GetSnapshot().authorityWritable);
        CHECK(stalled.ObserveCoordination(1, 2, true, 1750, false));
        CHECK(!stalled.GetSnapshot().authorityWritable);
        CHECK(stalled.GetSnapshot().control == Health::Healthy);
        CHECK(stalled.GetSnapshot().recovery == Recovery::None);
        CHECK(stalled.GetSnapshot().error.empty());
        CHECK(!CommandNow(stalled, CommandKind::RoomAction).accepted);
        // H-006: the runtime can tell a fenced refusal from any other, so it
        // parks or reports it instead of dropping it silently.
        {
            Command fenced;
            fenced.generation = stalled.GetSnapshot().generation;
            for (CommandKind kind : {CommandKind::RoomAction, CommandKind::SetLobbySettings, CommandKind::ApplyDelay,
                     CommandKind::CheckConnection, CommandKind::Ready, CommandKind::Rematch}) {
                fenced.kind = kind;
                CHECK(stalled.FencedOut(fenced));
                const auto decision = stalled.Execute(fenced);
                CHECK(!decision.accepted && decision.refusal == Refusal::Fenced);
            }
            fenced.kind = CommandKind::LeaveRoom;
            CHECK(!stalled.FencedOut(fenced));
            fenced.kind = CommandKind::SetLobbySettings;
            fenced.generation.match++;
            CHECK(!stalled.FencedOut(fenced));
            CHECK(stalled.Execute(fenced).refusal == Refusal::Rejected);
        }

        // Catching up clears the stall without leaving residue.
        CHECK(stalled.ObserveCoordination(1, 3, true, 2000));
        CHECK(stalled.GetSnapshot().authorityWritable);
        CHECK(stalled.GetSnapshot().authorityStalledMs == 0);
        {
            Command writable;
            writable.kind = CommandKind::SetLobbySettings;
            writable.generation = stalled.GetSnapshot().generation;
            CHECK(!stalled.FencedOut(writable));
        }

        // A stall that persists is named at ten seconds...
        CHECK(stalled.ObserveCoordination(1, 4, true, 3000, false));
        CHECK(stalled.GetSnapshot().error.empty());
        CHECK(stalled.ObserveCoordination(1, 5, true, 12000, false));
        CHECK(stalled.GetSnapshot().error.empty());
        CHECK(stalled.ObserveCoordination(1, 6, true, 13000, false));
        CHECK(!stalled.GetSnapshot().error.empty());
        CHECK(stalled.GetSnapshot().recovery == Recovery::None);

        // ...and offers a way out at thirty, without ever claiming a disconnect.
        CHECK(stalled.ObserveCoordination(1, 7, true, 33000, false));
        CHECK(stalled.GetSnapshot().recovery == Recovery::ReplacementOffered);
        CHECK(stalled.GetSnapshot().control == Health::Healthy);
        CHECK(CommandNow(stalled, CommandKind::ReplaceRoom).accepted);
    }

    std::printf("SessionController: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
