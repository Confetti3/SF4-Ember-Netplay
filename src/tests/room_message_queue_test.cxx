#include "../session/RoomMessageQueue.hxx"
#include <cstdio>
#include <cstdlib>
using namespace sf4e::session;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)
static Message Diagnostic(int frame) {
    return {2, frame, nlohmann::json{{"type",frame % 2 ? "battle_hash" : "battle_snapshot"},{"frameIdx",frame}}.dump(),{}};
}
static Message Action(const char* kind) {
    return {2, 1000, nlohmann::json{{"type","room_action"},{"kind",kind}}.dump(),{}};
}
int main() {
    std::deque<Message> queue; std::size_t bytes = 0;
    // Reproduce a paused checkpoint commit while the fight continues sending
    // verification frames. These must not kill the room or strand its result.
    for (int frame = 0; frame < 300; ++frame)
        CHECK(QueueRoomMessage(queue,bytes,Diagnostic(frame),true)==RoomQueueResult::Queued);
    for (const char* kind : {"result","finished","leave"})
        CHECK(QueueRoomMessage(queue,bytes,Action(kind),true)==RoomQueueResult::Queued);
    CHECK(queue.size() <= MaximumRoomQueueMessages);
    CHECK(queue[queue.size()-3].payload==Action("result").payload);
    CHECK(queue.back().payload==Action("leave").payload);
    std::size_t actual = 0; for(const auto& item:queue) actual += item.payload.size(); CHECK(actual==bytes);
    // A diagnostic burst cannot evict ordered room actions. If all slots are
    // required control messages, drop only incoming best-effort diagnostics.
    queue.clear(); bytes=0;
    for(int i=0;i<64;++i) CHECK(QueueRoomMessage(queue,bytes,Action("ready"),true)==RoomQueueResult::Queued);
    CHECK(QueueRoomMessage(queue,bytes,Diagnostic(301),true)==RoomQueueResult::Queued);
    CHECK(queue.size()==64 && queue.back().payload==Action("ready").payload);
    CHECK(QueueRoomMessage(queue,bytes,Action("leave"),true)==RoomQueueResult::MessagesFull);
    // Committed client effects are never discarded or reordered.
    queue.clear();bytes=0;
    for(int i=0;i<64;++i) CHECK(QueueRoomMessage(queue,bytes,Diagnostic(i),false)==RoomQueueResult::Queued);
    CHECK(QueueRoomMessage(queue,bytes,Diagnostic(64),false)==RoomQueueResult::MessagesFull);
    std::puts("Room diagnostic backpressure and result/leave queue tests passed");
}
