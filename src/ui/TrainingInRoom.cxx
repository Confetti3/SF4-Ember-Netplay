#include "TrainingInRoom.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
TrainingRoomStatus DescribeTrainingRoom(const room::Snapshot& room, unsigned unread) {
    TrainingRoomStatus status;
    if (!room.roomEpoch || !room::FindMember(room, room.localMember)) return status;
    status.room = room.name;
    const auto place = room::PlaceOf(room, room.localMember);
    if (place.kind != room::Place::Kind::None) status.table = loc::Tf("training.room.table", place.table + 1);
    if (place.kind == room::Place::Kind::Seat) {
        const auto* opponent = room::SeatOpponent(room, static_cast<std::uint8_t>(place.table), room.localMember);
        if (opponent) status.opponent = opponent->name;
        else status.place = loc::T("training.room.waiting");
    } else if (place.kind == room::Place::Kind::Queue) {
        const auto& queue = room.tables[place.table].queue;
        const auto at = std::find(queue.begin(), queue.end(), room.localMember) - queue.begin();
        status.place = loc::Tf("training.room.queued", static_cast<int>(at) + 1, static_cast<int>(queue.size()));
    } else status.place = loc::T("training.room.not_queued");
    if (unread) status.unread = loc::Tf("training.room.unread", unread);
    return status;
}
std::string TrainingRoomLine(const TrainingRoomStatus& status, const std::string& room, const std::string& opponent) {
    std::string line;
    const auto add = [&](const std::string& part) { if (!part.empty()) line += (line.empty() ? "" : "  \xC2\xB7  ") + part; };
    add(room); add(status.table);
    add(status.opponent.empty() ? status.place : loc::Tf("training.room.opponent", opponent));
    add(status.unread);
    return line;
}
ChallengerCall DescribeChallenger(const room::Snapshot& room, const input::CallIdentity& call) {
    ChallengerCall described;
    if (!call.Live() || room.roomEpoch != call.roomEpoch) return described;
    described.called = true;
    if (const auto* opponent = room::FindMember(room, call.opponent)) {
        described.opponent = opponent->name; described.fighter = opponent->fighter;
    }
    return described;
}
} }
