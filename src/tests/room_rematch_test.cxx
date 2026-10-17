#include "room_authority_support.hxx"

struct Set {
    RoomAuthority room{"Rematch", 8, 4};
    MemberId a=Join(room,0,true), b=Join(room,1), spectator=Join(room,2);
    Set(int goal=2) {
        auto rules=TableAction(room,a,0,ActionKind::SetRules);rules.rules.format=static_cast<SetFormat>(goal);
        CHECK(room.Apply(a,rules).accepted);
        for(auto id:{a,b})CHECK(room.Apply(id,TableAction(room,id,0,ActionKind::Queue)).accepted);
        CHECK(room.SetMemberFighter(a,0));CHECK(room.SetMemberFighter(b,1));
        room.AdvanceTime(1000);
    }
    const Table& table() const {return room.SnapshotView().tables[0];}
    Action action(MemberId id,ActionKind kind,bool quick=false) const {
        auto r=TableAction(room,id,0,kind);if(quick)r.matchGeneration=table().matchGeneration;return r;
    }
    void ready(MemberId id,bool quick=true){CHECK(room.Apply(id,action(id,ActionKind::Ready,quick)).accepted);}
    void start(){ready(a,false);ready(b,false);CHECK(room.BeginMatch(0,a,b).accepted);}
    void finish(MatchResult result=MatchResult::P1Win,bool ack=true){
        CHECK(room.EndMatch(0,table().matchGeneration,result).accepted);
        if(ack)for(auto id:{a,b})CHECK(room.Apply(id,action(id,ActionKind::AcknowledgeTerminal,true)).accepted);
    }
};
static void ContinueSet(){
    Set s;s.start();s.finish();
    CHECK(s.table().rematch.state==RematchOffer::Offered);CHECK(s.table().score[0]==1);
    CHECK(!s.table().rematch.timed);s.room.AdvanceTime(100000);CHECK(!s.table().rematch.timed);
    s.ready(s.a);CHECK(s.room.SnapshotFor(s.b).tables[0].rematch.remainingMs==30000);
    bool withdrew=true;CHECK(s.room.SetMemberFighter(s.b,4,&withdrew));CHECK(!withdrew);CHECK(s.table().ready[0]);
    s.ready(s.b);CHECK(s.table().phase==TablePhase::Ready);
    CHECK(!s.room.HasDueTimerTransition(140000));
    const auto old=s.table().matchGeneration;
    CHECK(s.room.BeginMatch(0,s.a,s.b).accepted);CHECK(s.table().matchGeneration>old);
    CHECK(s.table().rematch.state==RematchOffer::None);CHECK(s.table().score[0]==1);
    s.finish();CHECK(s.table().rematch.state==RematchOffer::None);CHECK(s.table().lastSet.score[0]==2);
}
static void Deadline(){
    Set s;s.start();s.finish();s.ready(s.a);
    s.room.AdvanceTime(11000);
    auto chat=s.action(s.b,ActionKind::Chat);chat.text="Still choosing";CHECK(s.room.Apply(s.b,chat).accepted);
    CHECK(s.room.SetMemberFighter(s.b,7));
    CHECK(s.room.Apply(s.a,s.action(s.a,ActionKind::Unready)).accepted);
    CHECK(s.table().rematch.timed);CHECK(s.room.SnapshotFor(s.b).tables[0].rematch.remainingMs==20000);
    s.ready(s.a);CHECK(s.room.SnapshotFor(s.b).tables[0].rematch.remainingMs==20000);
    CHECK(!s.room.HasDueTimerTransition(30999));CHECK(s.room.HasDueTimerTransition(31000));
    auto late=s.action(s.b,ActionKind::Ready,true);
    auto events=s.room.AdvanceTime(31000);CHECK(events.size()==1);
    CHECK(s.table().rematch.state==RematchOffer::Expired);CHECK(!s.table().ready[0]&&!s.table().ready[1]);
    CHECK(s.table().p1==s.a&&s.table().p2==s.b&&s.table().score[0]==1);
    CHECK(!s.room.Apply(s.b,late).accepted);
    CHECK(!s.room.Apply(s.b,s.action(s.b,ActionKind::Ready,true)).accepted);
    CHECK(s.room.AdvanceTime(100000).empty());
    s.ready(s.a,false);s.ready(s.b,false);CHECK(s.room.BeginMatch(0,s.a,s.b).accepted);
}
static void CancellationAndFences(){
    Set s;s.start();s.finish();s.ready(s.a);
    CHECK(!s.room.Apply(s.spectator,s.action(s.spectator,ActionKind::CancelRematch,true)).accepted);
    auto stale=s.action(s.b,ActionKind::CancelRematch,true);++stale.matchGeneration;
    CHECK(!s.room.Apply(s.b,stale).accepted);
    const auto cancel=s.action(s.b,ActionKind::CancelRematch,true);
    CHECK(s.room.Apply(s.b,cancel).accepted);CHECK(s.room.Apply(s.b,cancel).accepted);
    CHECK(s.table().rematch.state==RematchOffer::Cancelled);CHECK(s.table().score[0]==1);
    CHECK(!s.table().ready[0]&&!s.table().ready[1]);
    CHECK(!FindMember(s.room.SnapshotView(),s.a)->delayLocked);
    CHECK(!s.room.Apply(s.a,s.action(s.a,ActionKind::Ready,true)).accepted);
    // The ordinary Ready contract still withdraws consent when the matchup changes.
    s.ready(s.a,false);bool withdrew=false;CHECK(s.room.SetMemberFighter(s.b,8,&withdrew));CHECK(withdrew);CHECK(!s.table().ready[0]);
}
static void RecoveryAndWire(){
    Set s;s.start();s.finish();s.ready(s.a);s.room.AdvanceTime(11000);
    const auto wire=nlohmann::json(s.room.SnapshotFor(s.a));
    CHECK(wire.get<Snapshot>().tables[0].rematch.remainingMs==20000);
    const auto live=s.room.Checkpoint();s.room.PauseForRecovery();const auto paused=s.room.Checkpoint();
    for(const auto& state:{live,paused})for(auto clock:{500ull,1000000ull}){
        RoomAuthority restored("Recovered");CHECK(restored.RestoreCheckpoint(nlohmann::json::parse(state.dump())));
        restored.PauseForRecovery();restored.ResumeRecovery(clock);
        CHECK(restored.SnapshotFor(s.a).tables[0].rematch.remainingMs==20000);
        auto now=(std::max)(clock,10000ull);CHECK(!restored.HasDueTimerTransition(now+19999));
        restored.AdvanceTime(now+20000);CHECK(restored.SnapshotView().tables[0].rematch.state==RematchOffer::Expired);
    }
    auto corrupt=paused;corrupt.erase("rematch_age");RoomAuthority target("Reject");CHECK(!target.RestoreCheckpoint(corrupt));
    auto legacy=wire;legacy["tables"][0].erase("rematch");legacy["tables"][0].erase("rematch_ms");
    CHECK(legacy.get<Snapshot>().tables[0].rematch.state==RematchOffer::None);
    s.room.AdvancePausedTimers(5000);CHECK(s.room.SnapshotFor(s.a).tables[0].rematch.remainingMs==15000);
}
static void ExclusionsAndCleanup(){
    for(int goal:{0,1,2,3,10})for(auto outcome:{MatchResult::Draw,MatchResult::Cancel,MatchResult::Abort}){
        Set s(goal);s.start();s.finish(outcome);
        CHECK((s.table().rematch.state==RematchOffer::Offered)==(goal>=2&&outcome==MatchResult::Draw));
    }
    for(bool leave:{false,true}){
        Set s;s.start();s.finish();s.ready(s.a);
        if(leave)CHECK(s.room.Leave(s.b).accepted);
        else CHECK(s.room.Apply(s.b,s.action(s.b,ActionKind::Unqueue)).accepted);
        CHECK(s.table().rematch.state==RematchOffer::None);CHECK(!s.room.HasDueTimerTransition(100000));
    }
    Set s;s.start();s.finish(MatchResult::P1Win,false);
    CHECK(s.room.Apply(s.a,s.action(s.a,ActionKind::AcknowledgeTerminal,true)).accepted);s.ready(s.a);
    CHECK(!s.table().rematch.timed);s.room.AdvanceTime(40000);CHECK(s.table().rematch.state==RematchOffer::Offered);
    CHECK(s.room.Apply(s.b,s.action(s.b,ActionKind::AcknowledgeTerminal,true)).accepted);
    CHECK(s.room.SnapshotFor(s.b).tables[0].rematch.remainingMs==30000);
    CHECK(s.room.Apply(s.a,s.action(s.a,ActionKind::Close)).accepted);
    CHECK(nlohmann::json(s.room.SnapshotFor(s.a)).get<Snapshot>().closed);
}
int main(){ContinueSet();Deadline();CancellationAndFences();RecoveryAndWire();ExclusionsAndCleanup();
    std::printf("Room rematch: %d failure(s)\n",failures);return failures?1:0;}
