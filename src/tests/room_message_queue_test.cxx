#include "../session/ClientEffectQueue.hxx"
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
// A live committed effect of commit `revision` for member `recipient`, on
// connection 2, tagged as RoomMessageTag.hxx tags it where it is queued.
// `size` pads the payload.
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

// A member's client queue with the room's applied revision; connection 2 is
// the leader's unless a message says otherwise.
struct Client {
    ClientEffectQueue queue;
    std::uint64_t applied=0;
    RoomQueueResult Admit(Message message, bool fromLeader = true) {
        return queue.Admit(std::move(message),fromLeader,applied,MaximumRoomQueueBytes);
    }
    std::uint64_t Revision(std::size_t index) const { return queue.Messages()[index].tag.commitRevision; }
    RoomProjection Kind(std::size_t index) const { return queue.Messages()[index].tag.projection; }
    std::size_t RoomMessages() const { return queue.Messages().size(); }
    bool BytesAgree() const {
        std::size_t total=0;
        for (const auto& item:queue.Messages()) total+=item.payload.size();
        for (const auto& item:queue.Frames()) total+=item.payload.size();
        return total==queue.Bytes();
    }
};

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
// The drain as an effect gate whose journal still holds every effect: frames
// go at once, and a room message once `applied` is the newest commit applied
// here. The real gate against a compacted journal is RoomEffectBurst's.
static void Deliver(Client& client, ClientView& view) {
    while (!client.queue.Empty() && client.queue.Next().tag.commitRevision<=client.applied) {
        view.Apply(client.queue.Next());
        client.queue.PopNext(client.queue.Next().payload.size());
    }
}

// 300 commits of a busy room while checkpoints apply only every 30: the client
// queue holds a few room messages instead of failing at 64, and what it hands
// the gate leaves the member with the same room, the same chat and the same
// events, in the same order, as handing it every message.
static void TestBurstBehindParkedCheckpoints() {
    Client client;
    ClientView view, reference;
    std::size_t deepest=0;
    const auto add=[&](Message message) {
        reference.Apply(message);
        CHECK(client.Admit(std::move(message))==RoomQueueResult::Queued);
        deepest=(std::max)(deepest,client.RoomMessages());
    };
    for (std::uint64_t revision=1;revision<=300;++revision) {
        // The snapshot, a lifecycle event for this member's table, then its data_update.
        add(Snapshot(revision,revision%25==0));
        if (revision%40==0) add(Event(revision));
        add(Data(revision));
        if (revision%3==0) CHECK(client.Admit(Tagged(Diagnostic(static_cast<int>(revision))))==RoomQueueResult::Queued);
        if (revision%30==0) { client.applied=revision; Deliver(client,view); }
        CHECK(client.BytesAgree());
    }
    CHECK(client.queue.Empty());
    CHECK(view==reference);
    CHECK(view.snapshot==300 && view.chat==300 && view.data==300 && view.events.size()==7);
    // 30 commits of two or three room messages each, held in at most three per
    // run of state between events; the verification frames beside them stay
    // inside their own window.
    CHECK(deepest<=12);
}

// Which queued state a newer message replaces, and which it never does.
static void TestReplacedState() {
    {
        Client client;
        for (std::uint64_t revision=1;revision<=5;++revision) {
            CHECK(client.Admit(Snapshot(revision))==RoomQueueResult::Queued);
            CHECK(client.Admit(Data(revision))==RoomQueueResult::Queued);
        }
        CHECK(client.queue.Size()==2 && client.Revision(0)==5 && client.Kind(0)==RoomProjection::Snapshot);
        CHECK(client.Revision(1)==5 && client.Kind(1)==RoomProjection::TableData && client.BytesAgree());
        // The oldest commit waited on stays where it was.
        CHECK(client.queue.ParkedSince(0)==1);
    }
    {
        // A snapshot with the chat stays while the newer ones say
        // chat_unchanged, and goes once a newer one carries the chat itself.
        Client client;
        for (auto message:{Snapshot(1),Snapshot(2,true),Snapshot(3),Snapshot(4)})
            CHECK(client.Admit(std::move(message))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==2 && client.Kind(0)==RoomProjection::SnapshotWithChat && client.Revision(1)==4);
        CHECK(client.Admit(Snapshot(5,true))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==1 && client.Revision(0)==5);
    }
    {
        // An event ends the run: no state moves past it, before or after.
        Client client;
        for (auto message:{Snapshot(1),Snapshot(2),Event(2),Snapshot(3),Snapshot(4)})
            CHECK(client.Admit(std::move(message))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==3 && client.Revision(0)==2 && client.Kind(1)==RoomProjection::None && client.Revision(2)==4);
    }
    {
        // Verification frames wait in their own lane: they keep no order with
        // commits, and neither end the run nor wait behind it.
        Client client;
        for (auto message:{Snapshot(1),Snapshot(2),Tagged(Diagnostic(1)),Snapshot(3)})
            CHECK(client.Admit(std::move(message))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==2 && client.RoomMessages()==1 && client.Revision(0)==3);
        CHECK(client.queue.Frames().size()==1 && client.queue.Next().tag.verification && client.BytesAgree());
    }
    {
        // Only the same sender's state for the same member, only from the
        // leader's stream, and only by a newer commit: an older one never
        // replaces what came after it.
        Client client;
        for (auto message:{Snapshot(1),Snapshot(2),Effect("room_snapshot",3,RoomProjection::Snapshot,8)})
            CHECK(client.Admit(std::move(message))==RoomQueueResult::Queued);
        CHECK(client.Admit(Effect("room_snapshot",4,RoomProjection::Snapshot,7,3),false)==RoomQueueResult::Queued);
        CHECK(client.Admit(Snapshot(1))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==4 && client.Revision(0)==2);
        // Another peer's token says nothing to the queue.
        CHECK(client.Revision(2)==0 && client.queue.Messages()[2].tag.commitSequence==0);
        CHECK(client.Admit(Data(9))==RoomQueueResult::Queued && client.queue.Size()==5 && client.BytesAgree());
    }
}

// The bounds hold against a stream that never applies: a queue that has waited
// on its oldest commit MaximumParkedCommits commits fails, though the state it
// holds replaced itself all along; another peer's token cannot make it fail;
// events and other room messages still count against 64, and bytes against 4 MB.
static void TestBoundsStillHold() {
    {
        Client client;
        for (std::uint64_t revision=1;revision<=1+MaximumParkedCommits;++revision) {
            CHECK(client.Admit(Snapshot(revision))==RoomQueueResult::Queued);
            CHECK(client.Admit(Data(revision))==RoomQueueResult::Queued);
        }
        CHECK(client.queue.Size()==2 && client.queue.ParkedSince(0)==1);
        CHECK(client.Admit(Effect("data_update",9999,RoomProjection::TableData,7,3),false)==RoomQueueResult::Queued);
        CHECK(client.Admit(Snapshot(2+MaximumParkedCommits))==RoomQueueResult::CommitsBehind);
        // Once the oldest has applied, the oldest waited on is the oldest queued.
        client.applied=1;
        CHECK(client.Admit(Snapshot(2+MaximumParkedCommits))==RoomQueueResult::Queued);
        CHECK(client.queue.ParkedSince(1)==1+MaximumParkedCommits);
    }
    {
        Client client;
        for (std::uint64_t revision=1;revision<=MaximumRoomQueueMessages;++revision)
            CHECK(client.Admit(Event(revision))==RoomQueueResult::Queued);
        CHECK(client.Admit(Event(65))==RoomQueueResult::MessagesFull);
    }
    {
        // State that replaces itself never reaches the byte bound; events do.
        Client client;
        for (std::uint64_t revision=1;revision<=100;++revision)
            CHECK(client.Admit(Effect("room_snapshot",revision,RoomProjection::SnapshotWithChat,7,2,256*1024))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==1 && client.BytesAgree() && client.queue.Bytes()<MaximumRoomQueueBytes);
    }
    {
        Client client;
        RoomQueueResult result=RoomQueueResult::Queued;
        std::uint64_t revision=0;
        while (result==RoomQueueResult::Queued && revision<MaximumRoomQueueMessages)
            result=client.Admit(Effect("room_event",++revision,RoomProjection::None,7,2,256*1024));
        CHECK(result==RoomQueueResult::BytesFull && client.queue.Bytes()<=MaximumRoomQueueBytes && client.BytesAgree());
    }
    {
        // The queue keeps its own byte count, and its budget is what the room's
        // leaves it beside the server queue: a room message past it is refused,
        // a frame past it is let go.
        ClientEffectQueue queue;
        CHECK(queue.Admit(Effect("room_event",1,RoomProjection::None,7,2,1024),true,0,512)==RoomQueueResult::BytesFull);
        CHECK(queue.Admit(Tagged(Diagnostic(1)),true,0,0)==RoomQueueResult::Queued && queue.Empty() && queue.Bytes()==0);
        CHECK(queue.Admit(Event(2),true,0,MaximumRoomQueueBytes)==RoomQueueResult::Queued && queue.Bytes()==Event(2).payload.size());
    }
}

// Replays: verification frames leave them room, a live copy holds them back,
// and each goes where its live copy would be in commit order.
static void TestReplays() {
    {
        Client client;
        CHECK(client.Admit(Snapshot(30))==RoomQueueResult::Queued);
        for (int frame=0;frame<100;++frame) CHECK(client.Admit(Tagged(Diagnostic(frame)))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==1+MaximumQueuedVerification && client.queue.TakesReplay());
        for (std::uint64_t revision=1;revision<MaximumRoomQueueMessages/2;++revision)
            client.queue.InsertReplay(Event(revision),revision,revision*4);
        CHECK(client.RoomMessages()==MaximumRoomQueueMessages/2 && !client.queue.TakesReplay());
        // Still room for the live stream: the frames take none of its slots.
        CHECK(client.Admit(Event(31))==RoomQueueResult::Queued && client.RoomMessages()==MaximumRoomQueueMessages/2+1);
        CHECK(client.queue.Frames().size()==MaximumQueuedVerification && client.BytesAgree());
    }
    {
        // The live D_20 is queued and S_21 replaced S_20: S_20's replay goes
        // before D_20, and a receipt's notification after all of commit 20.
        Client client;
        for (auto message:{Snapshot(20),Data(20),Snapshot(21)}) CHECK(client.Admit(std::move(message))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==2 && client.Kind(0)==RoomProjection::TableData);
        CHECK(client.queue.HoldsCopy(1,20*4+static_cast<std::uint64_t>(RoomProjection::TableData)));
        CHECK(!client.queue.HoldsCopy(1,20*4+static_cast<std::uint64_t>(RoomProjection::Snapshot)));
        client.queue.InsertReplay(Snapshot(20),20,20*4+static_cast<std::uint64_t>(RoomProjection::Snapshot));
        Message notice{1,0,"{\"type\":\"room_event\"}",{}};
        notice.tag.classified=true; notice.tag.type="room_event"; notice.tag.terminalReplay=true;
        client.queue.InsertReplay(std::move(notice),20,ClientEffectQueue::AfterCommit);
        CHECK(client.queue.Size()==4 && client.Kind(0)==RoomProjection::Snapshot && client.Revision(0)==20);
        CHECK(client.Kind(1)==RoomProjection::TableData && client.queue.Messages()[2].tag.terminalReplay && client.Revision(3)==21);
        CHECK(client.BytesAgree());
    }
    {
        // Another peer's copy of a token holds no replay back.
        Client client;
        CHECK(client.Admit(Event(2),false)==RoomQueueResult::Queued);
        CHECK(!client.queue.HoldsCopy(1,2*4));
    }
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
    // The client queue keeps the same verification window while the game does
    // not poll, beside its room messages, which have all their slots.
    {
        Client client;
        for(int i=0;i<300;++i) CHECK(client.Admit(Diagnostic(i))==RoomQueueResult::Queued);
        CHECK(client.queue.Size()==MaximumQueuedVerification && client.BytesAgree());
        for(std::uint64_t i=1;i<=MaximumRoomQueueMessages;++i) CHECK(client.Admit(Event(i))==RoomQueueResult::Queued);
        CHECK(client.RoomMessages()==MaximumRoomQueueMessages && client.queue.Frames().size()==MaximumQueuedVerification);
        CHECK(client.Admit(Event(65))==RoomQueueResult::MessagesFull);
        for(std::uint64_t i=0;i<MaximumRoomQueueMessages;++i) CHECK(client.Revision(i)==i+1);
        // The newest frames, oldest first, go before every room message.
        CHECK(nlohmann::json::parse(client.queue.Next().payload).at("frameIdx")==300-static_cast<int>(MaximumQueuedVerification));
    }
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
    TestReplacedState();
    TestBoundsStillHold();
    TestReplays();
    std::puts("Room diagnostic backpressure and result/leave queue tests passed");
}
