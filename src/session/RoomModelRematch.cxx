#include "RoomModel.hxx"

namespace sf4e { namespace room {
void RoomAuthority::RefreshRematchTimers() {
    for (const auto& table : snapshot_.tables) {
        if (table.rematch.state != RematchOffer::Offered || table.rematch.timed ||
            table.phase != TablePhase::Waiting || (!table.ready[0] && !table.ready[1]) ||
            HasOutstandingTerminalReceipt(table.id)) continue;
        // Selection gets its full budget after teardown. Activity, chat and
        // withdrawing Ready never replenish it. Recovery preserves its age.
        snapshot_.tables[table.id].rematch.timed = true;
        rematchSince_[table.id] = recoveryPaused_ ? 0 : nowMs_;
    }
}
bool RoomAuthority::RematchDue(const Table& table, std::uint64_t nowMs) const {
    return !snapshot_.closed && table.rematch.state == RematchOffer::Offered && table.rematch.timed &&
        table.phase == TablePhase::Waiting && TimerDue(rematchSince_[table.id], RematchTimeoutMs, nowMs);
}
void RoomAuthority::EndRematch(Table& table, RematchOffer::State reason) {
    ClearReadiness(table);
    table.rematch.state = reason;
    ClearPermit(table);
    table.spectatorHold = false;
    startHeldSince_[table.id] = 0;
    table.phase = table.p1 && table.p2 ? TablePhase::Waiting : TablePhase::Idle;
    Touch(table);
    NormalizeTableMembers(table);
}
Result RoomAuthority::ApplyCancelRematch(MemberId member, const Action& action, Table& table) {
    if (!IsParticipant(table, member)) return Reject(RejectReason::NotSeated);
    if (!action.matchGeneration || action.matchGeneration != table.matchGeneration)
        return Reject(RejectReason::WrongGeneration);
    if (table.rematch.state != RematchOffer::Offered ||
        (table.phase != TablePhase::Waiting && !ReadyCancellable(table, table.p1 == member ? 0 : 1)))
        return Reject(RejectReason::WrongPhase);
    EndRematch(table, RematchOffer::Cancelled);
    return Accept();
}
void RoomAuthority::ExpireRematches(std::vector<Event>& events) {
    for (auto& table : snapshot_.tables) {
        if (!RematchDue(table, nowMs_)) continue;
        EndRematch(table, RematchOffer::Expired);
        events.push_back(Event{Event::Kind::SnapshotChanged, table.id, table.matchGeneration, 0, MatchResult::Abort});
    }
}
} }
