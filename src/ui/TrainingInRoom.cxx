#include "TrainingInRoom.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
TrainingRoomStatus DescribeTrainingRoom(const room::Snapshot& room, unsigned unread) {
    TrainingRoomStatus status;
    if (!room.roomEpoch || !room::FindMember(room, room.localMember)) return status;
    status.room = room.name;
    const auto place = room::PlaceOf(room, room.localMember);
    if (place.kind != room::Place::Kind::None) status.parts.push_back(loc::Tf("training.room.table", place.table + 1));
    if (place.kind == room::Place::Kind::Seat) {
        const auto* opponent = room::SeatOpponent(room, static_cast<std::uint8_t>(place.table), room.localMember);
        status.parts.push_back(opponent ? loc::Tf("training.room.opponent", opponent->name) : std::string(loc::T("training.room.waiting")));
    } else if (place.kind == room::Place::Kind::Queue) {
        const auto& queue = room.tables[place.table].queue;
        const auto at = std::find(queue.begin(), queue.end(), room.localMember) - queue.begin();
        status.parts.push_back(loc::Tf("training.room.queued", static_cast<int>(at) + 1, static_cast<int>(queue.size())));
    } else status.parts.push_back(loc::T("training.room.not_queued"));
    if (unread) status.parts.push_back(loc::Tf("training.room.unread", unread));
    return status;
}
ChallengerCall DescribeChallenger(const room::Snapshot& room) {
    ChallengerCall call;
    const auto place = room::PlaceOf(room, room.localMember);
    if (place.kind != room::Place::Kind::Seat) return call;
    if (const auto* opponent = room::SeatOpponent(room, static_cast<std::uint8_t>(place.table), room.localMember)) {
        call.opponent = opponent->name; call.fighter = opponent->fighter;
    }
    return call;
}
} }
