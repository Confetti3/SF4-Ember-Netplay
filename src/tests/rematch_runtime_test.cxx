#include "runtime_room_fixture.hxx"
#include "../ui/ApplicationShell.hxx"
#include "imgui_test_support.hxx"
#include <iostream>
using namespace sf4e;
namespace facade = sf4e::NetplayFacade;
namespace internal = sf4e::NetplayFacade::internal;
namespace protocol = sf4e::SessionProtocol;

struct RematchRoom {
    room::RoomAuthority authority{"Rematch runtime",8,41};
    room::MemberId first=0,second=0;
    test::RuntimeRoom r;
    room::Action Action(room::MemberId member,room::ActionKind kind) const {
        const auto& view=authority.SnapshotView();room::Action action;
        action.kind=kind;action.roomEpoch=view.roomEpoch;action.revision=view.revision;
        action.table=0;action.tableRevision=view.tables[0].revision;
        action.matchGeneration=view.tables[0].matchGeneration;
        action.actionId=member*1000+view.revision+1;return action;
    }
    RematchRoom() {
        auto joined=authority.Join("One",{"host","1"},true);CHECK(joined.accepted);first=joined.snapshot.members.back().id;
        joined=authority.Join("Two",{"host","2"},false);CHECK(joined.accepted);second=joined.snapshot.members.back().id;
        auto rules=Action(first,room::ActionKind::SetRules);rules.rules.format=static_cast<room::SetFormat>(2);
        CHECK(authority.Apply(first,rules).accepted);
        for(auto who:{first,second})CHECK(authority.Apply(who,Action(who,room::ActionKind::Queue)).accepted);
        for(auto who:{first,second})CHECK(authority.Apply(who,Action(who,room::ActionKind::Ready)).accepted);
        CHECK(authority.BeginMatch(0,first,second).accepted);
        CHECK(authority.EndMatch(0,authority.SnapshotView().tables[0].matchGeneration,room::MatchResult::P1Win).accepted);
        for(auto who:{first,second})CHECK(authority.Apply(who,Action(who,room::ActionKind::AcknowledgeTerminal)).accepted);
        r.Snapshot(authority.SnapshotFor(first));
        CHECK(r.Client().GetRoomSnapshot().tables[0].rematch.state==room::RematchOffer::Offered);
        internal::Publish();
    }
    facade::RuntimeCommand Cancel() const {
        facade::RuntimeCommand command;
        command.command={netplay::CommandKind::RoomAction,r.runtime->controller.GetSnapshot().generation,{}};
        command.roomAction=Action(first,room::ActionKind::CancelRematch);return command;
    }
    void Submit(facade::RuntimeCommand command) {
        CHECK(facade::SubmitRuntimeCommand(std::move(command)));internal::DrainCommands(true);internal::Publish();
    }
    room::Action Sent() const {return r.transport->sent.back().at("action").get<room::Action>();}
    void Reply(std::uint64_t id,bool accepted) {
        protocol::RoomResultMessage reply;reply.actionId=id;reply.result.accepted=accepted;
        reply.result.reason=accepted?room::RejectReason::None:room::RejectReason::NotSeated;
        reply.result.snapshot=authority.SnapshotFor(first);
        r.Deliver(nlohmann::json(reply));internal::DrainActionReplies();internal::ResolvePendingIntents(true);internal::Publish();
    }
    void CheckPending(bool expected) {
        const auto snapshot=facade::GetRuntimeSnapshot();CHECK(snapshot.rematchCancelRequested==expected);
        // The menu consumes the production publication, including the request
        // sequence. The test never assigns the outcome it expects to see.
        HeadlessImGui imgui;
        ui::ApplicationShell shell;ui::ShellView view;
        view.session=snapshot.session;view.room=snapshot.room;
        view.rematchCancelRequested=snapshot.rematchCancelRequested;view.rematchCommandSequence=snapshot.rematchCommandSequence;
        std::vector<ui::MenuEntry> rows;
        ui::SetMenuEntriesProbe([&](const auto& entries){rows=entries;});
        bool open=true;ImGui::NewFrame();shell.Draw(view,&open,[](ui::ShellAction){return false;},{});ImGui::Render();
        bool found=false;for(const auto& row:rows)if(row.id=="rematch-room"){found=true;CHECK(row.enabled!=expected);}
        CHECK(found);ui::SetMenuEntriesProbe({});
    }
};
static void RefusedAndCommitted() {
    RematchRoom game;game.Submit(game.Cancel());game.CheckPending(true);
    const auto old=game.Sent().actionId;game.Reply(old,false);game.CheckPending(false);
    CHECK(!facade::GetRuntimeSnapshot().helperError.empty());
    game.Submit(game.Cancel());game.CheckPending(true);
    const auto current=game.Sent().actionId;CHECK(current!=old);
    game.Reply(old,false);game.CheckPending(true); // A late reply cannot cancel the retry.
    auto action=game.Sent();action.actionId=game.Action(game.first,room::ActionKind::CancelRematch).actionId;
    CHECK(game.authority.Apply(game.first,action).accepted);
    game.Reply(current,true);CHECK(!facade::GetRuntimeSnapshot().rematchCancelRequested);
}
static void DeferredAndExpired(bool fence) {
    RematchRoom game;
    if(fence){
        const auto now=GetTickCount64();
        CHECK(game.r.runtime->controller.ObserveCoordination(1,2,true,now-300,false));
        CHECK(game.r.runtime->controller.ObserveCoordination(1,2,true,now,false));
        CHECK(game.r.runtime->controller.GetSnapshot().control==netplay::Health::Healthy);
        CHECK(!game.r.runtime->controller.GetSnapshot().authorityWritable);
    }
    else game.r.transport->writable=false;
    game.Submit(game.Cancel());CHECK(facade::GetRuntimeSnapshot().rematchCancelRequested);
    CHECK(game.r.runtime->rematchCancelIntent.Parked());
    Sleep(3100);
    internal::ResolvePendingIntents(true);
    if(fence)CHECK(game.r.runtime->controller.ObserveCoordination(1,2,true,GetTickCount64()));
    internal::Publish();game.CheckPending(false);
    CHECK(facade::GetRuntimeSnapshot().helperError==loc::T("runtime.room_catchup_timeout"));
}
static void StalePressAndRoomChange() {
    RematchRoom game;game.Submit(game.Cancel());game.CheckPending(true);
    auto stale=game.Cancel();++stale.command.generation.room;
    const auto before=game.r.transport->sent.size();game.Submit(stale);
    CHECK(game.r.transport->sent.size()==before);game.CheckPending(true);
    CHECK(game.r.runtime->controller.Execute({netplay::CommandKind::LeaveRoom,game.r.runtime->controller.GetSnapshot().generation,{}}).accepted);
    internal::Apply(netplay::EventKind::RoomClosed);
    CHECK(game.r.runtime->controller.Execute({netplay::CommandKind::HostRoom,game.r.runtime->controller.GetSnapshot().generation,{}}).accepted);
    internal::Apply(netplay::EventKind::RoomJoined);
    internal::ResolvePendingIntents(true);internal::Publish();CHECK(!facade::GetRuntimeSnapshot().rematchCancelRequested);
}
static void SendFailureAndAdmission() {
    {
        RematchRoom game;game.r.transport->sendResult=session::SendResult::Failed;
        game.Submit(game.Cancel());game.CheckPending(false);
        CHECK(facade::GetRuntimeSnapshot().helperError==loc::T("runtime.room_action_failed"));
        game.r.transport->sendResult=session::SendResult::Queued;
        game.Submit(game.Cancel());game.CheckPending(true);
    }
    {
        RematchRoom game;unsigned queued=0;
        while(queued<10000&&facade::SubmitRuntimeCommand(game.Cancel()))++queued;
        CHECK(queued>0&&queued<10000);
        CHECK(!facade::SubmitRuntimeCommand(game.Cancel()));
        internal::Publish();game.CheckPending(false); // Admission alone publishes no pending operation.
    }
}
int main() {
    SendFailureAndAdmission();RefusedAndCommitted();DeferredAndExpired(false);DeferredAndExpired(true);StalePressAndRoomChange();
    std::cout<<"Rematch runtime cancellation tests passed\n";
}
