#pragma once

#include "../session/RoomModel.hxx"
#include "TableSetup.hxx"

#include <array>
#include <cstdint>

namespace sf4e { namespace netplay {

// The tables chosen when creating a room. Once the creator is in it as its
// host, the runtime sets each table's rules with SetRules and its name with
// NameTable, whether or not Ember is on screen.
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
// hand (a SetRules or NameTable of their own, or a table that shows other
// rules than it had or was given): whatever is left is theirs.
//
// An Arcade room's type and rules (the first table's) go in one SetRoomType
// instead, while the creator is alone in it; a room that shows the type is done.
class CreatedRules {
public:
    // How long the room may take to admit the creator after the create.
    static constexpr std::uint64_t JoinMs = 60000;
    // How long the tables are given, from the creator's first sight of the room.
    static constexpr std::uint64_t DeadlineMs = 15000;
    // A SetRules with no answer by then is sent again.
    static constexpr std::uint64_t RetryMs = 1500;

    void Start(const TableSetup& setup, std::uint64_t nowMs, room::RoomType type = room::ClassicRoom) {
        *this = CreatedRules();
        active_ = true; setup_ = setup; type_ = type; armedAtMs_ = nowMs;
    }
    void Cancel(const char* why) { if (active_) { active_ = false; ended_ = why; } }
    bool Active() const { return active_; }
    // Why the last operation ended: "done", "deadline", "edited" and so on.
    const char* Ended() const { return ended_; }
    const TableSetup& Setup() const { return setup_; }
    // A room action the player sent: their own SetRules or NameTable ends the operation.
    void ObservePlayer(const room::Action& action) {
        if (action.kind == room::ActionKind::SetRules || action.kind == room::ActionKind::NameTable) Cancel("edited");
    }
    // The committed room as this member sees it. True, with `action` filled
    // in, when a table's rules or name are to be sent now; `canSend` says the room
    // control can take an action. Sent() records it once it is queued.
    bool Next(const room::Snapshot& room, bool canSend, std::uint64_t nowMs, room::Action& action) {
        if (!active_) return false;
        if (!roomEpoch_) {
            if (!room.roomEpoch || !room::FindMember(room, room.localMember)) {
                if (nowMs - armedAtMs_ >= JoinMs) Cancel("never_joined");
                return false;
            }
            // Some other room, or someone else got in first and moderates it.
            if (room.host != room.localMember) { Cancel("not_host"); return false; }
            roomEpoch_ = room.roomEpoch; host_ = room.localMember; deadlineMs_ = nowMs + DeadlineMs;
            for (const auto& table : room.tables) before_[table.id] = table.rules;
        }
        if (room.roomEpoch != roomEpoch_ || room.localMember != host_ || room.host != host_) { Cancel("room_changed"); return false; }
        if (nowMs >= deadlineMs_) { Cancel("deadline"); return false; }
        if (type_) {
            if (room.arcade) { Cancel(room.arcade == type_ ? "done" : "edited"); return false; }
            if (!canSend || (sentAtMs_ && nowMs - sentAtMs_ < RetryMs)) return false;
            action = room::Action();
            action.kind = room::ActionKind::SetRoomType;
            action.roomEpoch = room.roomEpoch;
            action.revision = room.revision;
            action.rules = setup_.rules[0];
            action.roomType = type_;
            return true;
        }
        int next = -1;
        // Only the tables a room plays at take rules; the others stay as they are.
        for (std::size_t i = 0; i < room::TablesInPlay(room); ++i) {
            const auto& table = room.tables[i];
            auto& done = confirmed_[table.id];
            const bool set = table.rules == setup_.rules[table.id] && table.name == setup_.names[table.id];
            if (done) {
                if (!set) { Cancel("edited"); return false; }
                continue;
            }
            if (set) { done = true; continue; }
            // Rules are set first, then the name; rules that moved anywhere else are the host's.
            if (!(table.rules == before_[table.id]) && !(table.rules == setup_.rules[table.id])) { Cancel("edited"); return false; }
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
        const bool rules = !(table.rules == setup_.rules[table.id]);
        action.kind = rules ? room::ActionKind::SetRules : room::ActionKind::NameTable;
        action.roomEpoch = room.roomEpoch;
        action.revision = room.revision;
        action.table = table.id;
        action.tableRevision = table.revision;
        if (rules) action.rules = setup_.rules[table.id];
        else action.text = setup_.names[table.id];
        return true;
    }
    void Sent(const room::Action& action, std::uint64_t nowMs) {
        if (active_ && action.kind == room::ActionKind::SetRoomType) sentAtMs_ = nowMs;
        if (!active_ || (action.kind != room::ActionKind::SetRules && action.kind != room::ActionKind::NameTable) || action.table >= room::TableCount) return;
        sentTable_ = action.table; sentRevision_ = action.tableRevision; sentAtMs_ = nowMs;
    }

private:
    bool active_ = false;
    const char* ended_ = "";
    TableSetup setup_;
    room::RoomType type_ = room::ClassicRoom;
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
