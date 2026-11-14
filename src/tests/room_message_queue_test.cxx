#include "../session/RoomMessageQueue.hxx"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace sf4e::session;
#include "test_support.hxx"
static Message Diagnostic(int frame) {
    return {2, frame, nlohmann::json{{"type",frame % 2 ? "battle_hash" : "battle_snapshot"},{"frameIdx",frame}}.dump(),{}};
}
static Message Action(const char* kind) {
    return {2, 1000, nlohmann::json{{"type","room_action"},{"kind",kind}}.dump(),{}};
}
static Message Tagged(Message message) {
    const auto decoded=nlohmann::json::parse(message.payload);
    message.tag.classified=true;
    message.tag.type=decoded.value("type",std::string());
    message.tag.verification=IsVerificationType(message.tag.type);
    return message;
}
// A live committed effect of commit `revision` for member `recipient`, from
// the leader's connection, tagged as RoomMessageTag.hxx tags it where it is
// queued. `size` pads the payload.
static Message Effect(const char* type, std::uint64_t revision, RoomProjection projection,
    std::uint64_t recipient = 7, Connection connection = 2, std::size_t size = 0) {
    Message message{connection, static_cast<std::int64_t>(revision),
        nlohmann::json{{"type",type},{"revision",revision},{"pad",std::string(size,'x')}}.dump(), {}};
    message.tag.classified=true; message.tag.type=type; message.tag.committed=true;
    message.tag.commitTerm=1; message.tag.commitRevision=revision; message.tag.commitRecipient=recipient;
    message.tag.commitSequence=revision*4+static_cast<std::uint64_t>(projection);
    message.tag.projection=projection;
    return message;
}
static Message Snapshot(std::uint64_t revision, bool chat = false) {
    return Effect("room_snapshot",revision,chat ? RoomProjection::SnapshotWithChat : RoomProjection::Snapshot);
}
static Message Data(std::uint64_t revision) { return Effect("data_update",revision,RoomProjection::TableData); }
static Message Event(std::uint64_t revision) { return Effect("room_event",revision,RoomProjection::None); }

// What a member's client keeps from the room state it is given, as
// SessionClient does: the newest snapshot (an older revision is ignored), the
// chat of the last snapshot that carried it, the last data_update, and every
// event in order.
struct ClientView {
    std::uint64_t snapshot=0, chat=0, data=0;
    std::vector<std::uint64_t> events;
    void Apply(const Message& message) {
        const auto revision=message.tag.commitRevision;
        switch (message.tag.projection) {
        case RoomProjection::SnapshotWithChat: if (revision>=snapshot) { snapshot=revision; chat=revision; } break;
        case RoomProjection::Snapshot: if (revision>=snapshot) snapshot=revision; break;
        case RoomProjection::TableData: data=revision; break;
        case RoomProjection::None: if (message.tag.committed) events.push_back(revision); break;
        }
    }
    bool operator==(const ClientView& other) const {
        return snapshot==other.snapshot && chat==other.chat && data==other.data && events==other.events;
    }
};
// The queue head as the effect gate lets it through once `applied` is the
// newest commit applied here: everything up to the first effect of a later one.
static void Deliver(std::deque<Message>& queue, std::size_t& bytes, std::uint64_t applied, ClientView& client) {
    while (!queue.empty() && queue.front().tag.commitRevision<=applied) {
        client.Apply(queue.front());
        bytes-=queue.front().payload.size();
        queue.pop_front();
    }
}
static std::size_t Bytes(const std::deque<Message>& queue) {
    std::size_t total=0; for (const auto& item:queue) total+=item.payload.size(); return total;
}

// 300 commits of a busy room while checkpoints apply only every 60: the client
// queue holds a few messages instead of failing at 64, and the member ends
// with the same room, the same chat and the same events, in the same order, as
// one given every message.
static void TestBurstBehindParkedCheckpoints() {
    std::deque<Message> queue; std::size_t bytes=0, deepest=0;
    ClientView client, reference;
    std::uint64_t applied=0;
    const auto add=[&](Message message) {
        reference.Apply(message);
        CHECK(QueueRoomEffect(queue,bytes,std::move(message),applied)==RoomQueueResult::Queued);
        deepest=(std::max)(deepest,static_cast<std::size_t>(std::count_if(queue.begin(),queue.end(),
            [](const Message& queued) { return !IsRoomVerificationMessage(queued); })));
    };
    for (std::uint64_t revision=1;revision<=300;++revision) {
        // The snapshot, a lifecycle event for this member's table, then its data_update.
        add(Snapshot(revision,revision%25==0));
        if (revision%40==0) add(Event(revision));
        add(Data(revision));
        if (revision%3==0) CHECK(QueueRoomEffect(queue,bytes,Tagged(Diagnostic(static_cast<int>(revision))),applied)==RoomQueueResult::Queued);
        if (revision%60==0) { applied=revision; Deliver(queue,bytes,applied,client); }
        CHECK(bytes==Bytes(queue));
    }
    CHECK(queue.empty());
    CHECK(client==reference);
    CHECK(client.snapshot==300 && client.chat==300 && client.data==300 && client.events.size()==7);
    // 60 commits of two or three room messages each, held in at most 16; the
    // verification frames beside them stay inside their own window.
    CHECK(deepest<=16);
}

// Which queued state a newer message replaces, and which it never does.
static void TestSupersededProjections() {
    // The first of each kind in a run stays: the next checkpoint may deliver
    // it, and it marks how long the queue has waited.
    std::deque<Message> queue; std::size_t bytes=0;
    for (std::uint64_t revision=1;revision<=5;++revision) {
        CHECK(QueueRoomEffect(queue,bytes,Snapshot(revision),0)==RoomQueueResult::Queued);
        CHECK(QueueRoomEffect(queue,bytes,Data(revision),0)==RoomQueueResult::Queued);
    }
    CHECK(queue.size()==4 && queue[0].tag.commitRevision==1 && queue[1].tag.commitRevision==1);
    CHECK(queue[2].tag.projection==RoomProjection::Snapshot && queue[2].tag.commitRevision==5);
    CHECK(queue[3].tag.projection==RoomProjection::TableData && queue[3].tag.commitRevision==5);
    CHECK(bytes==Bytes(queue));
    // A snapshot with the chat stays while the newer ones say chat_unchanged,
    // and goes once a newer one carries the chat itself.
    queue.clear(); bytes=0;
    for (auto message:{Snapshot(1),Snapshot(2,true),Snapshot(3),Snapshot(4)})
        CHECK(QueueRoomEffect(queue,bytes,std::move(message),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==3 && queue[1].tag.projection==RoomProjection::SnapshotWithChat && queue[2].tag.commitRevision==4);
    CHECK(QueueRoomEffect(queue,bytes,Snapshot(5,true),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==2 && queue[0].tag.commitRevision==1 && queue[1].tag.commitRevision==5);
    // An event ends the run: no state moves past it, before or after.
    queue.clear(); bytes=0;
    for (auto message:{Snapshot(1),Snapshot(2),Event(2),Snapshot(3),Snapshot(4)})
        CHECK(QueueRoomEffect(queue,bytes,std::move(message),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==5);
    CHECK(QueueRoomEffect(queue,bytes,Snapshot(5),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==5 && queue[1].tag.commitRevision==2 && queue[2].tag.projection==RoomProjection::None &&
        queue[3].tag.commitRevision==3 && queue[4].tag.commitRevision==5);
    // Verification frames do not end it: they keep no order with commits.
    queue.clear(); bytes=0;
    for (auto message:{Snapshot(1),Snapshot(2),Tagged(Diagnostic(1)),Snapshot(3)})
        CHECK(QueueRoomEffect(queue,bytes,std::move(message),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==3 && queue[1].tag.verification && queue[2].tag.commitRevision==3);
    // Only the same sender's state for the same member, and only by a newer
    // commit: an older one (a replay) never replaces what came after it.
    queue.clear(); bytes=0;
    for (auto message:{Snapshot(1),Snapshot(2),Effect("room_snapshot",3,RoomProjection::Snapshot,8),
        Effect("room_snapshot",4,RoomProjection::Snapshot,7,3),Snapshot(1)})
        CHECK(QueueRoomEffect(queue,bytes,std::move(message),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==5);
    CHECK(QueueRoomEffect(queue,bytes,Data(9),0)==RoomQueueResult::Queued && queue.size()==6);
    CHECK(bytes==Bytes(queue));
}

// The bounds still hold against a stream that never applies: a queue that
// waits on its oldest effect MaximumParkedCommits later fails, events and
// other room messages still count against 64, and bytes against 4 MB.
static void TestBoundsStillHold() {
    std::deque<Message> queue; std::size_t bytes=0;
    for (std::uint64_t revision=1;revision<=1+MaximumParkedCommits;++revision) {
        CHECK(QueueRoomEffect(queue,bytes,Snapshot(revision),0)==RoomQueueResult::Queued);
        CHECK(QueueRoomEffect(queue,bytes,Data(revision),0)==RoomQueueResult::Queued);
    }
    CHECK(queue.size()==4);
    CHECK(QueueRoomEffect(queue,bytes,Snapshot(2+MaximumParkedCommits),0)==RoomQueueResult::CommitsBehind);
    // Once the oldest has applied, the same commit is in reach.
    CHECK(QueueRoomEffect(queue,bytes,Snapshot(2+MaximumParkedCommits),1)==RoomQueueResult::Queued);
    // Events are never dropped, so they still fill the queue.
    queue.clear(); bytes=0;
    for (std::uint64_t revision=1;revision<=MaximumRoomQueueMessages;++revision)
        CHECK(QueueRoomEffect(queue,bytes,Event(revision),0)==RoomQueueResult::Queued);
    CHECK(QueueRoomEffect(queue,bytes,Event(65),0)==RoomQueueResult::MessagesFull);
    // State that replaces itself never reaches the byte bound; events do.
    queue.clear(); bytes=0;
    for (std::uint64_t revision=1;revision<=100;++revision)
        CHECK(QueueRoomEffect(queue,bytes,Effect("room_snapshot",revision,RoomProjection::SnapshotWithChat,7,2,256*1024),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==2 && bytes==Bytes(queue) && bytes<MaximumRoomQueueBytes);
    queue.clear(); bytes=0;
    RoomQueueResult result=RoomQueueResult::Queued;
    std::uint64_t revision=0;
    while (result==RoomQueueResult::Queued && revision<MaximumRoomQueueMessages)
        result=QueueRoomEffect(queue,bytes,Effect("room_event",++revision,RoomProjection::None,7,2,256*1024),0);
    CHECK(result==RoomQueueResult::BytesFull && bytes<=MaximumRoomQueueBytes && bytes==Bytes(queue));
}

int main() {
    std::deque<Message> queue; std::size_t bytes = 0;
    // Reproduce a paused checkpoint commit while the fight continues sending
    // verification frames. These must not kill the room or strand its result.
    for (int frame = 0; frame < 300; ++frame)
        CHECK(QueueRoomMessage(queue,bytes,Diagnostic(frame))==RoomQueueResult::Queued);
    for (const char* kind : {"result","finished","leave"})
        CHECK(QueueRoomMessage(queue,bytes,Action(kind))==RoomQueueResult::Queued);
    CHECK(queue.size() <= MaximumRoomQueueMessages);
    CHECK(queue[queue.size()-3].payload==Action("result").payload);
    CHECK(queue.back().payload==Action("leave").payload);
    std::size_t actual = 0; for(const auto& item:queue) actual += item.payload.size(); CHECK(actual==bytes);
    // A diagnostic burst cannot evict ordered room actions. If all slots are
    // required control messages, drop only incoming best-effort diagnostics.
    queue.clear(); bytes=0;
    for(int i=0;i<64;++i) CHECK(QueueRoomMessage(queue,bytes,Action("ready"))==RoomQueueResult::Queued);
    CHECK(QueueRoomMessage(queue,bytes,Diagnostic(301))==RoomQueueResult::Queued);
    CHECK(queue.size()==64 && queue.back().payload==Action("ready").payload);
    CHECK(QueueRoomMessage(queue,bytes,Action("leave"))==RoomQueueResult::MessagesFull);
    // The client queue keeps the same verification window behind a parked
    // checkpoint, and its committed effects are never discarded for it.
    queue.clear();bytes=0;
    for(int i=0;i<300;++i) CHECK(QueueRoomEffect(queue,bytes,Diagnostic(i),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==MaximumQueuedVerification && bytes==Bytes(queue));
    for(std::uint64_t i=1;i<=MaximumRoomQueueMessages;++i) CHECK(QueueRoomEffect(queue,bytes,Event(i),0)==RoomQueueResult::Queued);
    CHECK(queue.size()==MaximumRoomQueueMessages && !IsRoomVerificationMessage(queue.front()));
    CHECK(QueueRoomEffect(queue,bytes,Event(65),0)==RoomQueueResult::MessagesFull);
    for(std::uint64_t i=0;i<MaximumRoomQueueMessages;++i) CHECK(queue[i].tag.commitRevision==i+1);
    // A message classified where it was queued is read from its tag, and the
    // queue keeps exactly the same window as when every entry is parsed.
    {
        // A tag that disagrees with the payload shows the tag is what is read.
        auto disguised=Action("ready"); disguised.tag.classified=true; disguised.tag.verification=true;
        CHECK(IsRoomVerificationMessage(disguised) && !IsRoomVerificationMessage(Action("ready")));
        std::deque<Message> plain, classified; std::size_t plainBytes=0, classifiedBytes=0;
        for (int frame=0;frame<300;++frame) {
            const auto message=frame%7==0 ? Action("ready") : Diagnostic(frame);
            const auto expected=QueueRoomMessage(plain,plainBytes,message);
            CHECK(QueueRoomMessage(classified,classifiedBytes,Tagged(message))==expected);
        }
        CHECK(plain.size()==classified.size() && plainBytes==classifiedBytes);
        for (std::size_t i=0;i<plain.size();++i) CHECK(plain[i].payload==classified[i].payload && classified[i].tag.classified);
    }
    TestBurstBehindParkedCheckpoints();
    TestSupersededProjections();
    TestBoundsStillHold();
    std::puts("Room diagnostic backpressure and result/leave queue tests passed");
}
