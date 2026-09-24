#pragma once

#include <cstdint>
#include <string>

namespace sf4e { namespace netplay {

// Orchestration state only. SessionServer remains the authority for lobby
// membership, settings, readiness, and match conditions.
enum class RoomState { Idle, Opening, Joined, Closing, Lost };
enum class MatchState { None, Preparing, Playing, PostMatch, Failed };
enum class Health { Offline, Connecting, Healthy, Lost };
enum class Recovery { None, Recovering, ReplacementOffered };
enum class Page { Home, Host, Join, Lobby, Match, PostMatch, Settings, Diagnostics };
enum class CommandKind { HostRoom, JoinInvite, LeaveRoom, Ready, Rematch, StartOffline, RequestUpdate, SavePreferences, SetLobbySettings, RoomAction, ReplaceRoom, CheckConnection, ApplyDelay };
enum class EventKind { RoomJoined, RoomFailed, RoomClosed, ReadyAcknowledged,
    MatchPreparing, GameplayReady, MatchStarted, MatchEnded, ControlLost, GameplayLost, HelperLost, MatchRecovered };
enum class Effect { None, HostRoom, JoinInvite, CloseSession, SendReady, StartOffline,
    CheckUpdate, AbortMatch, FinishDegradedMatch, SavePreferences, SetLobbySettings, SendRoomAction, ReplaceRoom, CheckConnection, ApplyDelay };

struct Generation {
    uint64_t room = 0;
    uint64_t match = 0;
    bool operator==(const Generation& other) const { return room == other.room && match == other.match; }
};

struct Command {
    CommandKind kind;
    // UI commands are tagged with the snapshot they were drawn from. This
    // prevents a queued Ready/Leave from acting on a newly created room.
    Generation generation;
    std::string invitation;
    bool benchmark = false;
};

struct Event {
    EventKind kind;
    Generation generation;
    std::string error;
};

struct Snapshot {
    Generation generation;
    Page page = Page::Home;
    RoomState room = RoomState::Idle;
    MatchState match = MatchState::None;
    Health control = Health::Offline;
    Health gameplay = Health::Offline;
    bool readyPending = false;
    bool isHost = false;
    bool verificationAvailable = false;
    bool coordinated = false;
    bool authorityWritable = false;
    std::uint64_t authorityTerm = 0, authorityRevision = 0;
    Recovery recovery = Recovery::None;
    std::uint64_t recoveryStartedMs = 0;
    // A connected, same-term control stream whose checkpoint has not applied
    // locally fences every room mutation. Ordinary lag is brief; a stall that
    // never clears must become visible and recoverable instead of silently
    // refusing Ready, Queue and Watch for the rest of the session.
    std::uint64_t authorityStalledMs = 0;
    // While creating or joining, when the forming control stream was first
    // seen unwritable. It is not a lost room, but a long wait is shown.
    std::uint64_t openingStalledMs = 0;
    bool openingStalled = false;
    std::string error;
};

// Why a command was not accepted. A fenced command is valid but arrived while
// the room authority is catching up, so the caller may hold it and retry.
enum class Refusal { Rejected, Fenced };

struct Decision {
    bool accepted = false;
    Refusal refusal = Refusal::Rejected;
    Effect effect = Effect::None;
    Generation generation;
    // Only returned to the backend for JoinInvite. Never part of Snapshot.
    std::string invitation;
};

// Own on the game thread. Workers enqueue events; the renderer consumes a
// copied Snapshot and enqueues commands. No network or game callbacks here.
class SessionController {
public:
    Snapshot GetSnapshot() const { return state_; }
    Decision Execute(const Command& command);
    Decision Apply(const Event& event);
    // A room mutation for the current generation that Execute refuses only
    // because the authority checkpoint fence is closed. The caller parks or
    // reports it; it is never dropped silently (ledger H-006).
    bool FencedOut(const Command& command) const;
    // Called only from locally applied helper coordination state. A remote
    // message claiming a newer term is not evidence of room authority.
    bool ObserveCoordination(std::uint64_t term, std::uint64_t revision,
        bool writable, std::uint64_t nowMs, bool locallyApplied = true);
    void AdvanceRecovery(std::uint64_t nowMs);
    void AdvanceCatchUp(std::uint64_t nowMs);
    // A create or join whose control stream stays unwritable this long sets
    // Snapshot::openingStalled, so the screen can say so.
    static constexpr std::uint64_t OpeningStallMs = 30000;
    // Whether an unwritable control stream means a lost control plane. A room
    // still being created or joined has none yet, so there is nothing to recover.
    bool ControlPlaneEstablished() const { return state_.room != RoomState::Opening; }
    void ShowPage(Page page) { state_.page = page; }

private:
    Snapshot state_;
    Decision Accept(Effect effect = Effect::None) const;
    void ResetRoom();
};

} } // namespace sf4e::netplay
