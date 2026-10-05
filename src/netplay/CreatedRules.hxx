#pragma once

#include "../session/RoomModel.hxx"

#include <array>
#include <cstdint>

namespace sf4e { namespace netplay {

// The table rules chosen when creating a public room. The service opens the
// room at the public default; once the creator is in it as its host, the
// runtime sets the chosen rules on each table with SetRules, whether or not
// Ember is on screen.
//
// One table at a time: the next is sent only once the committed room shows
// the last with the chosen rules, so a fenced room control parks one action,
// never several that replace each other. One refused as stale (the table's
// revision moved on without it) or lost (nothing came back within RetryMs) is
// sent again at the table's current revision.
//
// A table that shows the chosen rules is done for good. The operation ends
// when every table is done, after DeadlineMs in the room, when the room, the
// member or the host changes, and when the host edits any table's rules by
// hand (a SetRules of their own, or a table that shows other rules than it
// had or was given): whatever is left is theirs.
class CreatedRules {
public:
    // How long the room may take to admit the creator after the create.
    static constexpr std::uint64_t JoinMs = 60000;
    // How long the tables are given, from the creator's first sight of the room.
    static constexpr std::uint64_t DeadlineMs = 15000;
    // A SetRules with no answer by then is sent again.
    static constexpr std::uint64_t RetryMs = 1500;

    void Start(const room::Rules& rules, std::uint64_t nowMs) {
        *this = CreatedRules();
        active_ = true; rules_ = rules; armedAtMs_ = nowMs;
    }
    void Cancel(const char* why) { if (active_) { active_ = false; ended_ = why; } }
    bool Active() const { return active_; }
    // Why the last operation ended: "done", "deadline", "edited" and so on.
    const char* Ended() const { return ended_; }
    const room::Rules& Rules() const { return rules_; }
    // A room action the player sent: their own SetRules ends the operation.
    void ObservePlayer(const room::Action& action) {
        if (action.kind == room::ActionKind::SetRules) Cancel("edited");
    }
    // The committed room as this member sees it. True, with `action` filled
    // in, when a table's rules are to be sent now; `canSend` says the room
    // control can take an action. Sent() records it once it is queued.
    bool Next(const room::Snapshot& room, bool canSend, std::uint64_t nowMs, room::Action& action) {
        if (!active_) return false;
        if (!roomEpoch_) {
            if (!room.roomEpoch || !room::FindMember(room, room.localMember)) {
                if (nowMs - armedAtMs_ >= JoinMs) Cancel("never_joined");
                return false;
            }
            // Some other room, or someone else got in first and moderates it.
            if (!room.serverOwned || room.host != room.localMember) { Cancel("not_host"); return false; }
            roomEpoch_ = room.roomEpoch; host_ = room.localMember; deadlineMs_ = nowMs + DeadlineMs;
            for (const auto& table : room.tables) before_[table.id] = table.rules;
        }
        if (room.roomEpoch != roomEpoch_ || room.localMember != host_ || room.host != host_) { Cancel("room_changed"); return false; }
        if (nowMs >= deadlineMs_) { Cancel("deadline"); return false; }
        int next = -1;
        for (const auto& table : room.tables) {
            auto& done = confirmed_[table.id];
            if (done) {
                if (!(table.rules == rules_)) { Cancel("edited"); return false; }
                continue;
            }
            if (table.rules == rules_) { done = true; continue; }
            if (!(table.rules == before_[table.id])) { Cancel("edited"); return false; }
            if (next < 0) next = table.id;
        }
        if (next < 0) { Cancel("done"); return false; }
        if (sentTable_ >= 0) {
            const auto& sent = room.tables[sentTable_];
            if (!confirmed_[sentTable_] && sent.revision == sentRevision_ && nowMs - sentAtMs_ < RetryMs) return false;
            sentTable_ = -1;
        }
        if (!canSend) return false;
        const auto& table = room.tables[next];
        action = room::Action();
        action.kind = room::ActionKind::SetRules;
        action.roomEpoch = room.roomEpoch;
        action.revision = room.revision;
        action.table = table.id;
        action.tableRevision = table.revision;
        action.rules = rules_;
        return true;
    }
    void Sent(const room::Action& action, std::uint64_t nowMs) {
        if (!active_ || action.table >= room::TableCount) return;
        sentTable_ = action.table; sentRevision_ = action.tableRevision; sentAtMs_ = nowMs;
    }

private:
    bool active_ = false;
    const char* ended_ = "";
    room::Rules rules_;
    std::uint64_t armedAtMs_ = 0;
    // Set at the creator's first sight of the room as its host.
    std::uint64_t roomEpoch_ = 0;
    room::MemberId host_ = 0;
    std::uint64_t deadlineMs_ = 0;
    std::array<room::Rules, room::TableCount> before_{};
    std::array<bool, room::TableCount> confirmed_{};
    // The table whose SetRules is on its way, and the revision it was sent for.
    int sentTable_ = -1;
    std::uint64_t sentRevision_ = 0;
    std::uint64_t sentAtMs_ = 0;
};

} }
