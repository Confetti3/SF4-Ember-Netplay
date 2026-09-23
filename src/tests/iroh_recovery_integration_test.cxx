#include "iroh_integration_fixture.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../netplay/SessionController.hxx"
#include <ggponet.h>
#include <functional>
#include <cstdlib>
#include <cstring>

using namespace sf4e;
#include "test_support.hxx"

static GGPOSession* activeSession = nullptr;
static GGPOSessionCallbacks RecoveryGameCallbacks() {
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = [](const char*) { return true; };
    callbacks.save_game_state = [](unsigned char** data, int* size, int* checksum, int) {
        *size = 1; *checksum = 0;
        *data = static_cast<unsigned char*>(std::calloc(1, 1));
        return *data != nullptr;
    };
    callbacks.load_game_state = [](unsigned char*, int) { return true; };
    callbacks.log_game_state = [](char*, unsigned char*, int) { return true; };
    callbacks.free_buffer = [](void* data) { std::free(data); };
    callbacks.advance_frame = [](int) {
        unsigned char inputs[2 * sf4e::session::GgpoInputBytes] = {};
        int disconnected = 0;
        return ggpo_synchronize_input(activeSession, inputs, sizeof(inputs), &disconnected) == GGPO_OK &&
            ggpo_advance_frame(activeSession) == GGPO_OK;
    };
    callbacks.on_event = [](GGPOEvent*) { return true; };
    return callbacks;
}

static Dimps::GameEvents::VsMode::ConfirmedCharaConditions RecoveryChara(int id) {
    Dimps::GameEvents::VsMode::ConfirmedCharaConditions result{};
    result.charaID = static_cast<std::uint8_t>(id);
    result.unc_edition = 14;
    return result;
}

// Observe actual delivered control without injecting or bypassing authority.
// The match owner can then deliberately hold a grant in SessionClient's queue.
class ObservedGrantTransport final : public session::ClientTransport {
public:
    ObservedGrantTransport(std::unique_ptr<session::ClientTransport> inner, std::uint64_t& generation,
        unsigned* deliveries=nullptr)
        : inner_(std::move(inner)), generation_(generation), deliveries_(deliveries) {}
    bool Poll(std::vector<session::Message>& messages,std::size_t maximum) override {
        const auto first=messages.size();
        if(!inner_->Poll(messages,maximum)) return false;
        for(std::size_t i=first;i<messages.size();++i) {
            const auto body=nlohmann::json::parse(messages[i].payload);
            if(body.value("type",std::string())=="game_prepare") {
                generation_=body.at("generation").get<std::uint64_t>();
                if(deliveries_) ++*deliveries_;
            }
        }
        return true;
    }
    session::ConnectionState State() const override {return inner_->State();}
    session::SendResult Send(const std::string& payload,bool reliable,std::int64_t* id) override {
        return inner_->Send(payload,reliable,id);
    }
    std::string PeerAddress() const override {return inner_->PeerAddress();}
    void Close() override {inner_->Close();}
private:
    std::unique_ptr<session::ClientTransport> inner_;
    std::uint64_t& generation_;
    unsigned* deliveries_;
};

// None stops the technical leader's helper. Departure is a graceful leader
// Leave; FollowerKilled closes a seated follower's pipe without one.
enum class Fault { None, Departure, FollowerKilled, Preparing, PreparingMinority, SameTermPreparation, Started, CommittedResult };
static void RunRecovery(const wchar_t* helperPath, bool relayOnly, std::size_t count, Fault fault) {
    std::array<platform::HelperProcess,3> processes;
    std::array<platform::HelperClient,3> helpers;
    std::array<test::IrohIntegrationPeer,3> peers;
    std::array<std::unique_ptr<session::IrohMatchSession>,3> matches;
    std::array<GGPOSession*,3> ggpo = {};
    std::vector<test::IrohIntegrationPeer*> live;
    const char* phase="bootstrap";
    bool holdSecondPreparation=fault==Fault::Preparing || fault==Fault::PreparingMinority;
    std::uint64_t observedQueuedGrant=0;
    std::array<std::uint64_t,3> observedGenerations{};
    std::array<unsigned,3> grantDeliveries{};
    std::function<void()> gameStep;
    const auto pump=[&]() {
        CHECK(test::PumpIrohIntegrationPeers(live));
        for (std::size_t i=0;i<3;++i) if (matches[i]) {
            if(i==2 && holdSecondPreparation) continue;
            if (!matches[i]->Tick(ggpo[i] != nullptr)) {
                std::cerr << "Recovery match " << i << " failed: " << matches[i]->Error()
                    << " phase=" << static_cast<int>(matches[i]->GetPhase()) << '\n';
                CHECK(false);
            }
        }
        if(gameStep) gameStep();
    };
    const auto wait=[&](const std::function<bool()>& complete, unsigned timeout=30000) {
        const auto until=GetTickCount64()+timeout;
        do {
            for(std::size_t i=0;i<count;++i) if(processes[i].IsRunning()) peers[i].room->Poll();
            if(complete()) return;
            Sleep(3);
        } while(GetTickCount64()<until);
        std::cerr << "Recovery timeout in " << phase << '\n';
        for(std::size_t i=0;i<count;++i) {
            const auto& a=peers[i].room->Coordination();
            std::cerr << i << " alive=" << processes[i].IsRunning() << " term=" << a.term
                << " revision=" << a.revision << " local=" << a.leaderLocal << " writable=" << a.writable
                << " voters=" << a.voterCount << " learners=" << a.learnerCount
                << " rebound=" << a.rebound << " applied=" << peers[i].recovery.AppliedRevision()
                << " caught_up=" << peers[i].recovery.CaughtUp(a)
                << " error=" << peers[i].room->Error();
            if(peers[i].client) {
                const auto& snapshot=peers[i].client->GetRoomSnapshot();
                std::cerr << " client_members=" << snapshot.members.size()
                    << " client_revision=" << snapshot.revision
                    << " local_member=" << snapshot.localMember
                    << " client_error=" << peers[i].client->RoomError();
            }
            if(peers[i].server) {
                const auto checkpoint=peers[i].server->RecoveryCheckpoint();
                const auto transport=peers[i].room->RecoveryState();
                std::cerr << " candidate=" << peers[i].server->HasRecoveryCandidate()
                    << " proposal=" << bool(peers[i].server->PendingProposal())
                    << " queued_server=" << transport.serverQueueMessages << " queued_client=" << transport.clientQueueMessages
                    << " staged=" << transport.stagedCheckpoints << " parked_marker=" << transport.pendingCommittedMarker
                    << " receiving=" << transport.checkpointActive << '/' << transport.checkpointComplete << ' '
                    << transport.checkpointOffset << '/' << transport.checkpointLength << " rev=" << transport.checkpointRevision
                    << " in_flight=" << peers[i].room->ProposalInFlight() << " proposal_status=" << transport.proposalStatus
                    << " proposal_wire=" << transport.proposalTransfer << '/' << transport.proposalTerm << '/' << transport.proposalBaseRevision
                    << " sent=" << transport.proposalSent << '/' << transport.proposalBytes << " acked=" << transport.proposalAcknowledged
                    << " begun=" << transport.proposalBegun << " ended=" << transport.proposalEnded
                    << " elapsed_ms=" << transport.proposalElapsedMs << " proposal_timeouts=" << transport.proposalTimeouts;
                if(const auto pending=peers[i].server->PendingProposal())
                    std::cerr << " pending=" << pending->request << '/' << pending->term << '/' << pending->baseRevision
                        << " effects=" << pending->effects.size();
                if(i && matches[i]) std::cerr << " match_phase=" << static_cast<int>(matches[i]->GetPhase())
                    << " match_generation=" << matches[i]->Generation();
                if(checkpoint.contains("room") && checkpoint["room"].contains("terminal_receipts"))
                    for(const auto& receipt:checkpoint["room"]["terminal_receipts"]) {
                        std::cerr << " terminal=" << receipt.at("table") << '/' << receipt.at("generation")
                            << " acknowledged=" << receipt.at("acknowledged");
                        for(const auto& recipient:receipt.at("recipients"))
                            std::cerr << " recipient=" << recipient.at("member") << ':' << recipient.at("acknowledged");
                    }
            }
            std::cerr << '\n';
        }
        CHECK(false);
    };
    for(std::size_t i=0;i<count;++i) {
        CHECK(processes[i].Start(helperPath,GetCurrentProcessId(),relayOnly));
        CHECK(helpers[i].Start(processes[i].Bootstrap()));
        peers[i].room=std::make_shared<session::IrohRoom>(helpers[i]);
        peers[i].name="Recovery "+std::to_string(i);
    }
    wait([&](){for(std::size_t i=0;i<count;++i) if(helpers[i].State()!=platform::HelperState::Connected) return false;return true;});
    CHECK(peers[0].room->Host("recovery-integration"));
    wait([&](){return peers[0].room->GetState()==session::IrohRoom::State::Ready;});
    SessionClient::Callbacks callbacks={};
    callbacks.OnError=[](SessionClient::ErrorType,SessionClient*,const SessionClient::Callbacks&) {CHECK(false);};
    for(std::size_t i=0;i<count;++i) {
        phase="admission";
        if(i) {
            CHECK(peers[i].room->Join(peers[0].room->Invitation(),"recovery-integration"));
            wait([&](){if(!live.empty()) pump();return peers[i].room->GetState()==session::IrohRoom::State::Ready;});
        }
        if((i==2 && fault==Fault::PreparingMinority) ||
            (i>0 && fault==Fault::SameTermPreparation)) {
            CHECK(test::ConfigureIrohIntegrationServer(peers[i],"recovery-integration"));
            peers[i].client.reset(new SessionClient(callbacks,"recovery-integration",static_cast<std::uint16_t>(31000+i),peers[i].name));
            peers[i].client->RequireCustomRooms();peers[i].client->RequireMatchAuthorization();
            auto& observedGeneration=fault==Fault::SameTermPreparation?observedGenerations[i]:observedQueuedGrant;
            std::unique_ptr<session::ClientTransport> observed(new ObservedGrantTransport(peers[i].room->Client(),
                observedGeneration,fault==Fault::SameTermPreparation?&grantDeliveries[i]:nullptr));
            CHECK(peers[i].client->Connect(std::move(observed),false)==0);peers[i].configured=true;
        } else CHECK(test::ConfigureIrohIntegrationPeer(peers[i],callbacks,"recovery-integration",static_cast<std::uint16_t>(31000+i)));
        live.push_back(&peers[i]);
        wait([&](){pump();return test::AllIrohRoomMembers(live,i+1);});
    }
    const auto action=[&](std::size_t peer,room::ActionKind kind,const std::string& text="",room::MemberId target=0) {
        auto& client=*peers[peer].client;
        // Do not build a request from a stale native projection.  This is
        // especially important when a follower has just installed a
        // checkpoint while the old leader's control stream is closing.
        wait([&]() {
            pump();
            std::uint64_t roomRevision=0, appliedRevision=0;
            for(auto* livePeer:live) {
                roomRevision=std::max(roomRevision,livePeer->client->GetRoomSnapshot().revision);
                appliedRevision=std::max(appliedRevision,livePeer->recovery.AppliedRevision());
            }
            for(auto* livePeer:live) {
                // Coordination also commits native setup/transport changes
                // which do not advance the public room model revision.
                if(livePeer->client->GetRoomSnapshot().revision!=roomRevision ||
                    livePeer->recovery.AppliedRevision()!=appliedRevision ||
                    !livePeer->server->RoomSnapshot() ||
                    livePeer->server->RoomSnapshot()->revision!=roomRevision ||
                    livePeer->server->HasRecoveryCandidate() || livePeer->server->PendingProposal() ||
                    !livePeer->recovery.CaughtUp(livePeer->room->Coordination())) return false;
            }
            return client.GetRoomSnapshot().revision==roomRevision;
        });
        room::Action request;
        std::uint64_t id=0;
        bool reportedUnsent=false;
        wait([&]() {
            pump();
            const auto view=client.GetRoomSnapshot();
            request.kind=kind;request.table=0;request.roomEpoch=view.roomEpoch;request.revision=view.revision;
            request.tableRevision=view.tables[0].revision;request.text=text;request.inputDelay=4;
            request.matchGeneration=view.tables[0].matchGeneration;request.target=target;
            const auto sent=client.SendRoomAction(request,&id);
            if(sent==session::SendResult::Queued) return true;
            if(!reportedUnsent) {
                std::cerr << "Recovery action not queued phase=" << phase << " kind=" << static_cast<int>(kind)
                    << " peer=" << peer << " send=" << static_cast<int>(sent)
                    << " writable=" << peers[peer].room->Coordination().writable
                    << " room_error=" << client.RoomError() << '\n';
                reportedUnsent=true;
            }
            // Only retry an explicitly unsent request. Once queued, require
            // its exact accepted reply below; never retry a rejected action.
            CHECK(sent==session::SendResult::NotConnected || sent==session::SendResult::QueueFull);
            return false;
        });
        wait([&](){
            pump(); SessionClient::ActionReply reply;
            while(client.TakeActionReply(reply)) if(reply.actionId==id) {
                if(!reply.accepted) {
                    std::cerr << "Recovery action rejected kind=" << static_cast<int>(kind)
                        << " peer=" << peer << " reason=" << static_cast<int>(reply.reason)
                        << " room_error=" << client.RoomError() << " request_revision=" << request.revision
                        << " current_revision=" << client.GetRoomSnapshot().revision << '\n';
                }
                CHECK(reply.accepted); return true;
            }
            return false;
        });
    };
    phase="baseline commitment";
    action(1,room::ActionKind::Chat,"Before control loss");
    wait([&](){pump();const auto revision=peers[0].recovery.AppliedRevision();
        for(auto* peer:live) if(!peer->recovery.CaughtUp(peer->room->Coordination()) || peer->recovery.AppliedRevision()!=revision) return false;
        return true;});
    CHECK(peers[0].room->Coordination().leaderLocal);
    const auto oldRoom=peers[0].room->RoomId();
    const auto oldTerm=peers[0].room->Coordination().term;
    const auto oldRevision=peers[0].recovery.AppliedRevision();
    for(std::size_t i=1;i<count;++i) CHECK(peers[i].room->RoomId()==oldRoom);
    if(fault==Fault::Departure) {
        phase="explicit moderator transfer";
        const auto first=peers[0].client->GetRoomSnapshot().localMember;
        const auto oldest=peers[1].client->GetRoomSnapshot().localMember;
        const auto other=peers[2].client->GetRoomSnapshot().localMember;
        action(0,room::ActionKind::TransferHost,"",other);
        action(2,room::ActionKind::TransferHost,"",first);
        phase="normal leader and moderator departure";
        action(0,room::ActionKind::Leave);
        CHECK(peers[0].client->GetRoomSnapshot().localMember==0);
        live.erase(live.begin());
        peers[0].client.reset();peers[0].server.reset();peers[0].room->Leave(false);
        wait([&](){pump();if(peers[0].room->GetState()!=session::IrohRoom::State::Idle) return false;
            unsigned leaders=0;for(auto* peer:live) {
                const auto& authority=peer->room->Coordination();const auto& snapshot=peer->client->GetRoomSnapshot();
                if(!authority.writable || !peer->recovery.CaughtUp(authority) || snapshot.host!=oldest ||
                    snapshot.closed || snapshot.members.size()!=2 || peer->room->RoomId()!=oldRoom) return false;
                leaders+=authority.leaderLocal?1:0;
            }return leaders==1;},6000); // Well inside OpenRaft's 12 s leader lease: the handoff must not wait it out.
        CHECK(processes[0].IsRunning());
        phase="successor restores the stable voter count";
        // The handoff goes to one voter; the successor promotes the other
        // survivor so a later departure still has somewhere to hand off to.
        wait([&](){pump();for(auto* peer:live) if(peer->room->Coordination().voterCount!=2 ||
            peer->room->Coordination().learnerCount!=0) return false;return true;});
        phase="new native command after graceful transfer";
        // Recovery rebases the existing one-second chat cooldown. A fast
        // transfer must not turn this authority check into a rate-limit test.
        const auto chatReadyAt=GetTickCount64()+1010;
        wait([&](){pump();return GetTickCount64()>=chatReadyAt;});
        action(1,room::ActionKind::Chat,"After normal transfer");
        for(std::size_t i=0;i<count;++i) CHECK(helpers[i].Send("{\"type\":\"shutdown\"}"));
        phase="graceful fixture shutdown";
        wait([&](){for(std::size_t i=0;i<count;++i) if(processes[i].IsRunning()) return false;return true;});
        std::cout << "Explicit moderator transfer and normal leader departure preserved usable room authority; relay-only=" << relayOnly << '\n';
        return;
    }
    if(fault==Fault::FollowerKilled) {
        // A seated follower's game is killed (Alt+F4): no Leave is ever sent.
        phase="seat the follower";
        action(2,room::ActionKind::Queue);
        const auto ghost=peers[2].client->GetRoomSnapshot().localMember;
        CHECK(ghost!=0);
        wait([&](){pump();const auto& table=peers[0].client->GetRoomSnapshot().tables[0];return table.p1==ghost || table.p2==ghost;});
        // A killed game drops its helper pipe; the helper then closes its
        // endpoint without a room Leave. Close the pipe the same way.
        live.pop_back();helpers[2].Stop();
        wait([&](){return !processes[2].IsRunning();});
        phase="leader commits the killed follower's departure";
        wait([&](){pump();for(auto* peer:live) {
            const auto& authority=peer->room->Coordination();const auto& snapshot=peer->client->GetRoomSnapshot();
            if(!authority.writable || !peer->recovery.CaughtUp(authority) || snapshot.members.size()!=2 ||
                snapshot.tables[0].p1==ghost || snapshot.tables[0].p2==ghost || authority.voterCount!=2) return false;
        }return true;},45000); // 15 s departure grace, then the commit and voter removal.
        phase="freed seat is usable";
        action(1,room::ActionKind::Queue);
        action(1,room::ActionKind::Chat,"After killed follower");
        for(std::size_t i=0;i<2;++i) CHECK(helpers[i].Send("{\"type\":\"shutdown\"}"));
        phase="follower-killed fixture shutdown";
        wait([&](){for(std::size_t i=0;i<2;++i) if(processes[i].IsRunning()) return false;return true;});
        std::cout << "Killed follower was removed from the room, its seat and the voter set; relay-only=" << relayOnly << '\n';
        return;
    }

    if(fault!=Fault::None) {
        CHECK(count == 3);
        phase="active started match setup";
        // Keep the original technical leader in the room as an unseated
        // member.  The two other helpers own the selected fighter seats;
        // this makes the running match independent of the helper that will
        // be stopped below.
        CHECK(peers[0].client->GetRoomSnapshot().members.size() == 3);
        const auto findLocal = [&](std::size_t peer) {
            const auto& view=peers[peer].client->GetRoomSnapshot();
            return std::find_if(view.members.begin(),view.members.end(),[&](const room::Member& member) {
                return member.id == view.localMember;
            });
        };
        CHECK(findLocal(0) != peers[0].client->GetRoomSnapshot().members.end());
        CHECK(findLocal(0)->status == room::MemberStatus::Idle);
        action(1,room::ActionKind::Queue);
        action(2,room::ActionKind::Queue);
        wait([&]() {
            pump();
            const auto& table=peers[1].client->GetRoomSnapshot().tables[0];
            return table.p1 && table.p2 &&
                peers[1].client->GetRoomSnapshot().members.size() == 3 &&
                peers[2].client->GetRoomSnapshot().members.size() == 3;
        });
        CHECK(peers[1].client->PreBattle_SetChara(RecoveryChara(2)) == session::SendResult::Queued);
        CHECK(peers[2].client->PreBattle_SetChara(RecoveryChara(7)) == session::SendResult::Queued);
        wait([&]() {
            pump();
            return std::all_of(live.begin(),live.end(),[](test::IrohIntegrationPeer* peer) {
                const auto& view=peer->client->GetRoomSnapshot();
                const auto& table=view.tables[0];
                const auto find=[&](room::MemberId id) {
                    const auto row=std::find_if(view.members.begin(),view.members.end(),[&](const room::Member& member) { return member.id == id; });
                    return row == view.members.end() ? -1 : row->fighter;
                };
                return table.p1 && table.p2 && find(table.p1) == 2 && find(table.p2) == 7;
            });
        });
        matches[1].reset(new session::IrohMatchSession(*peers[1].client,peers[1].room));
        matches[2].reset(new session::IrohMatchSession(*peers[2].client,peers[2].room));
        const auto ready=[&](std::size_t peer,unsigned delay) {
            if(fault==Fault::SameTermPreparation && peer==2) {
                // The window below steps the owner until it holds a candidate
                // and then proposes it in one tick. Start from an idle owner,
                // or that candidate may be an earlier commit still applying
                // (checkpoints decode off the game thread) and not this Ready.
                auto& owner=peers[0];
                wait([&]() {
                    pump();
                    return !owner.server->HasRecoveryCandidate() && !owner.server->PendingProposal() &&
                        owner.recovery.CaughtUp(owner.room->Coordination());
                });
            }
            const auto view=peers[peer].client->GetRoomSnapshot();
            room::Action request; request.kind=room::ActionKind::Ready; request.table=0;
            request.roomEpoch=view.roomEpoch; request.revision=view.revision;
            request.tableRevision=view.tables[0].revision; request.inputDelay=static_cast<std::uint8_t>(delay);
            std::uint64_t id=0;
            CHECK(peers[peer].client->SendRoomAction(request,&id)==session::SendResult::Queued);
            if(fault==Fault::SameTermPreparation && peer==2) {
                phase="same-term preparation proposal";
                auto& owner=peers[0];
                // Consume the Ready intent without importing its eventual
                // commit. The production bridge still constructs and submits
                // the exact proposal; the helper supplies real quorum proof.
                wait([&]() {
                    CHECK(owner.server->Step()==0);
                    return owner.server->HasRecoveryCandidate();
                });
                CHECK(owner.recovery.Tick(*owner.server,*owner.room));
                const auto pending=owner.server->PendingProposal();
                CHECK(pending && std::count_if(pending->effects.begin(),pending->effects.end(),
                    [](const session::EffectEnvelope& effect) {return effect.type=="game_prepare";})==2);
                owner.server->SetAuthority(pending->term,pending->baseRevision,false);
                CHECK(owner.server->PendingProposal()==pending);
                phase="helper commitment while native proposal is paused";
                wait([&]() {
                    session::IrohRoom::CommittedCheckpoint committed;
                    return owner.room->Coordination().revision==pending->baseRevision+1 &&
                        owner.room->TakeCommittedCheckpoint(committed) &&
                        committed.identity.transfer==pending->request;
                });
                CHECK(owner.recovery.AppliedRevision()==pending->baseRevision);
                CHECK(grantDeliveries[1]==0 && grantDeliveries[2]==0);
                CHECK(owner.server->PendingProposal()==pending);
                // Keep the native gate paused on the first healthy pump. The
                // bridge must restore that same-term authority before commit,
                // retaining private payloads until it can publish them once.
                phase="same-term preparation commit resumption";
            }
            wait([&]() {
                pump(); SessionClient::ActionReply reply;
                while(peers[peer].client->TakeActionReply(reply)) if(reply.actionId==id) {
                    CHECK(reply.accepted); return true;
                }
                return false;
            });
        };
        const unsigned fighterOneDelay=3, fighterTwoDelay=8;
        ready(1,fighterOneDelay); ready(2,fighterTwoDelay);
        using MatchPhase=session::IrohMatchSession::Phase;
        const auto terminalAcks=[&](std::uint64_t generation,unsigned score) {
            std::array<bool,3> observed{},queued{};
            wait([&]() {
                pump();
                for(std::size_t i=1;i<=2;++i)
                    test::AcknowledgeIrohFixtureTerminal(*peers[i].client,0,generation,
                        ggpo[i]==nullptr && matches[i]->GetPhase()==MatchPhase::Idle,observed[i],queued[i]);
                if(!queued[1] || !queued[2]) return false;
                for(auto* peer:live) {
                    if(!test::IrohFixtureTerminalsCommitted(*peer->server,{{0,generation}})) return false;
                    const auto& table=peer->client->GetRoomSnapshot().tables[0];
                    if(table.score[0]!=0 || table.score[1]!=score) return false;
                }
                return true;
            });
        };
        const auto shutdownMatchPeers=[&]() {
            matches[1].reset();matches[2].reset();
            CHECK(helpers[1].Send("{\"type\":\"shutdown\"}"));CHECK(helpers[2].Send("{\"type\":\"shutdown\"}"));
            wait([&]() {return !processes[1].IsRunning() && !processes[2].IsRunning();});
        };
        if(fault==Fault::SameTermPreparation) {
            wait([&]() {pump();return matches[1]->GetPhase()==MatchPhase::Started &&
                matches[2]->GetPhase()==MatchPhase::Started;});
            const auto generation=matches[1]->Generation();
            CHECK(generation==1 && matches[2]->Generation()==generation);
            for(std::size_t i=1;i<=2;++i) {
                CHECK(observedGenerations[i]==generation && grantDeliveries[i]==1);
                CHECK(peers[i].room->RoomId()==oldRoom && peers[i].room->Coordination().term==oldTerm);
                const auto game=peers[i].room->Game(peers[i==1?2:1].room->LocalIdentity());
                CHECK(game.state==session::IrohRoom::GameState::Ready && game.generation==generation && !game.route.empty());
                if(relayOnly) CHECK(game.route.rfind("relay:",0)==0);
                std::cout << "Same-term preparation generation=" << generation << " peer=" << i
                    << " grant_deliveries=" << grantDeliveries[i] << " selected route=" << game.route << '\n';
            }
            action(1,room::ActionKind::AbortMatch);
            phase="same-term authorization cleanup";terminalAcks(generation,0);
            CHECK(grantDeliveries[1]==1 && grantDeliveries[2]==1);
            shutdownMatchPeers();
            CHECK(helpers[0].Send("{\"type\":\"shutdown\"}"));
            wait([&]() {return !processes[0].IsRunning();});
            std::cout << "Same-term native pause retained the private proposal through real helper commitment and emitted each grant once; relay-only=" << relayOnly << '\n';
            return;
        }
        if(fault==Fault::Preparing || fault==Fault::PreparingMinority) {
            phase="real preparation before leader loss";
            wait([&]() {pump();return matches[1]->GetPhase()==MatchPhase::Prepared &&
                matches[2]->GetPhase()==MatchPhase::Idle;});
            const auto generation=matches[1]->Generation();
            CHECK(generation && matches[2]->Generation()==0);
            CHECK(peers[2].room->Game(peers[1].room->LocalIdentity()).generation==0);
            const auto listener=peers[1].room->Game(peers[2].room->LocalIdentity());
            // Waiting acknowledges the prepared listener. A virtual bridge
            // port is only published by game_ready after authorization.
            CHECK(listener.generation==generation && listener.virtualPort==0 && peers[1].client->_ggpoPort &&
                listener.state==session::IrohRoom::GameState::Waiting);
            if(fault==Fault::PreparingMinority) {
                wait([&]() {pump();return observedQueuedGrant==generation &&
                    peers[2].room->Coordination().writable && peers[2].recovery.CaughtUp(peers[2].room->Coordination());});
                CHECK(matches[2]->Generation()==0 && matches[2]->GetPhase()==MatchPhase::Idle);
                CHECK(matches[2]->BeginReplacement());
                CHECK(matches[2]->Tick(false));
                CHECK(matches[2]->Generation()==0 && matches[2]->GetPhase()==MatchPhase::Idle && matches[2]->CanReplace(false));
                CHECK(peers[2].room->Game(peers[1].room->LocalIdentity()).generation==0);
                std::cout << "Explicit replacement ignored an actually queued generation " << generation << " grant with writable control.\n";
                // This is the real prepared-listener case which the empty
                // two-voter fixture cannot exercise. Lose the old quorum
                // while the local native application has not created GGPO.
                CHECK(matches[1]->CanBeginReplacement(false));
                CHECK(!matches[1]->CanBeginReplacement(true));
                CHECK(!matches[1]->CanReplace(false));
                const auto remote=peers[2].room->LocalIdentity();
                processes[0].Stop(0);processes[2].Stop(0);matches[2].reset();
                live={&peers[1]};holdSecondPreparation=false;
                phase="prepared minority frozen without old quorum";
                wait([&]() {pump();return !peers[1].room->Coordination().writable;});
                netplay::SessionController replacement;
                const auto command=[&](netplay::CommandKind kind) {
                    return replacement.Execute({kind,replacement.GetSnapshot().generation,{}});
                };
                const auto event=[&](netplay::EventKind kind) {
                    netplay::Event value;value.kind=kind;value.generation=replacement.GetSnapshot().generation;
                    return replacement.Apply(value);
                };
                CHECK(command(netplay::CommandKind::HostRoom).accepted);
                CHECK(event(netplay::EventKind::RoomJoined).accepted);
                CHECK(command(netplay::CommandKind::Ready).accepted);
                CHECK(event(netplay::EventKind::MatchPreparing).accepted);
                const auto recoveryStart=GetTickCount64();
                CHECK(replacement.ObserveCoordination(oldTerm,oldRevision,false,recoveryStart));
                const auto until=recoveryStart+16000;
                do {
                    pump();CHECK(!peers[1].room->Coordination().writable);
                    CHECK(peers[1].room->RoomId()==oldRoom);
                    CHECK(matches[1]->CanBeginReplacement(false));
                    CHECK(!matches[1]->CanReplace(false));
                    replacement.AdvanceRecovery(GetTickCount64());Sleep(10);
                } while(GetTickCount64()<until);
                CHECK(replacement.GetSnapshot().recovery==netplay::Recovery::ReplacementOffered);
                CHECK(peers[1].client->GetRoomSnapshot().tables[0].phase==room::TablePhase::Playing);
                CHECK(command(netplay::CommandKind::ReplaceRoom).effect==netplay::Effect::ReplaceRoom);
                CHECK(matches[1]->BeginReplacement());
                // An explicit request starts retirement; it does not itself
                // prove that the old helper can no longer forward packets.
                CHECK(!matches[1]->CanReplace(false));
                phase="prepared minority local mapping retirement";
                wait([&]() {pump();return matches[1]->CanReplace(false);});
                const auto retired=peers[1].room->Game(remote);
                CHECK(retired.generation==generation && retired.state==session::IrohRoom::GameState::Closed);
                CHECK(!matches[1]->CanReplace(true));
                CHECK(peers[1].client->GetRoomSnapshot().tables[0].phase==room::TablePhase::Playing);
                matches[1].reset();live.clear();peers[1].client.reset();peers[1].server.reset();
                peers[1].room->Leave(true);
                wait([&]() {return peers[1].room->GetState()==session::IrohRoom::State::Idle;});
                CHECK(event(netplay::EventKind::RoomClosed).accepted);
                CHECK(command(netplay::CommandKind::HostRoom).accepted);
                CHECK(peers[1].room->Host("recovery-integration"));
                wait([&]() {return peers[1].room->GetState()==session::IrohRoom::State::Ready;});
                CHECK(peers[1].room->RoomId()!=oldRoom);
                peers[1].recovery=session::RoomRecoveryRuntime{};
                CHECK(test::ConfigureIrohIntegrationPeer(peers[1],callbacks,"recovery-integration",31001));
                live.push_back(&peers[1]);
                wait([&]() {pump();return test::AllIrohRoomMembers(live,1);});
                action(1,room::ActionKind::Chat,"Prepared minority replacement is usable");
                CHECK(helpers[1].Send("{\"type\":\"shutdown\"}"));
                wait([&]() {return !processes[1].IsRunning();});
                std::cout << "Prepared minority retired its exact listener before creating usable fresh authority despite stale Playing; relay-only=" << relayOnly << '\n';
                return;
            }
            const auto* firstMatch=matches[1].get();const auto* secondMatch=matches[2].get();
            processes[0].Stop(0);live.erase(live.begin());holdSecondPreparation=false;
            phase="preparation cancellation after leader loss";
            wait([&]() {
                pump();unsigned leaders=0;
                for(auto* peer:live) {
                    const auto& a=peer->room->Coordination();
                    if(!a.writable || !a.rebound || a.term<=oldTerm || !peer->recovery.CaughtUp(a)) return false;
                    leaders+=a.leaderLocal?1:0;
                }
                return leaders==1 && matches[1]->GetPhase()==MatchPhase::Idle && matches[2]->GetPhase()==MatchPhase::Idle;
            });
            CHECK(matches[1].get()==firstMatch && matches[2].get()==secondMatch);
            const auto retired=peers[1].room->Game(peers[2].room->LocalIdentity());
            CHECK(retired.generation==generation && retired.state==session::IrohRoom::GameState::Closed);
            phase="preparation cancellation receipt acknowledgment";terminalAcks(generation,0);
            ready(1,fighterOneDelay);ready(2,fighterTwoDelay);
            phase="fresh generation after preparation mappings retired";
            wait([&]() {pump();return matches[1]->GetPhase()==MatchPhase::Started && matches[2]->GetPhase()==MatchPhase::Started;});
            const auto fresh=matches[1]->Generation();
            CHECK(fresh>generation && matches[2]->Generation()==fresh);
            for(std::size_t i=1;i<=2;++i) {
                const auto game=peers[i].room->Game(peers[i==1?2:1].room->LocalIdentity());
                CHECK(game.state==session::IrohRoom::GameState::Ready && game.generation==fresh && !game.route.empty());
                if(relayOnly) CHECK(game.route.rfind("relay:",0)==0);
                std::cout << "Fresh preparation recovery generation=" << fresh << " peer=" << i << " selected route=" << game.route << '\n';
            }
            // This branch proves authorization/cleanup; GGPO creation and
            // native callback traffic are exercised by the Started branch.
            action(1,room::ActionKind::AbortMatch);
            phase="fresh authorization cleanup";terminalAcks(fresh,0);
            shutdownMatchPeers();
            std::cout << "Preparing migration cancelled the old grant, drained its listener, and started a fresh generation; relay-only=" << relayOnly << '\n';
            return;
        }
        wait([&]() { pump(); return matches[1]->GetPhase()==MatchPhase::Started && matches[2]->GetPhase()==MatchPhase::Started; });
        const auto* matchOne=matches[1].get(); const auto* matchTwo=matches[2].get();
        auto ggpoCallbacks=RecoveryGameCallbacks();
        const auto rosterOne=matches[1]->Roster(); const auto rosterTwo=matches[2]->Roster();
        CHECK(rosterOne.size()==2 && rosterTwo==rosterOne);
        std::array<std::uint16_t,2> localPorts={peers[1].client->_ggpoPort,peers[2].client->_ggpoPort};
        std::array<std::string,2> routes={peers[1].room->Game(peers[2].room->LocalIdentity()).route,
            peers[2].room->Game(peers[1].room->LocalIdentity()).route};
        CHECK(!routes[0].empty() && !routes[1].empty());
        if(relayOnly) CHECK(routes[0].rfind("relay:",0)==0 && routes[1].rfind("relay:",0)==0);
        std::array<std::uint64_t,2> generations={matches[1]->Generation(),matches[2]->Generation()};
        CHECK(generations[0] != 0 && generations[0]==generations[1]);
        std::cout << "Started recovery generation=" << generations[0]
            << " selected gameplay routes=" << routes[0] << "," << routes[1] << '\n';
        const auto beforeTable=peers[1].client->GetRoomSnapshot().tables[0];
        CHECK(beforeTable.inputDelay[0]==fighterOneDelay && beforeTable.inputDelay[1]==fighterTwoDelay);
        std::array<nlohmann::json,2> authorityBefore;
        for(std::size_t i=0;i<2;++i) {
            const auto checkpoint=peers[i+1].server->RecoveryCheckpoint();
            CHECK(checkpoint.contains("match_authorities") && checkpoint["match_authorities"].is_array());
            CHECK(checkpoint["match_authorities"].size() > 0 && !checkpoint["match_authorities"][0].is_null());
            authorityBefore[i]=checkpoint["match_authorities"][0];
            CHECK(!authorityBefore[i].value("cap_digest",std::string()).empty());
        }
        std::array<GGPOPlayerHandle,3> localHandles={};
        for(std::size_t i=1;i<=2;++i) {
            matches[i]->ReleasePortToGgpo();
            CHECK(ggpo_start_session(&ggpo[i],&ggpoCallbacks,"recovery-started",2,
                static_cast<int>(session::GgpoInputBytes),peers[i].client->_ggpoPort)==GGPO_OK);
            const auto slot=matches[i]->LocalSlot();
            const auto& roster=matches[i]->Roster();
            for(std::size_t player=0;player<roster.size();++player) {
                GGPOPlayer entry={}; entry.size=sizeof(entry); entry.player_num=static_cast<int>(player)+1;
                entry.type=player==slot ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
                if(player!=slot) {
                    CHECK(matches[i]->RemotePort(roster[player]) != 0);
                    strcpy_s(entry.u.remote.ip_address,"127.0.0.1");
                    entry.u.remote.port=matches[i]->RemotePort(roster[player]);
                }
                GGPOPlayerHandle handle;
                CHECK(ggpo_add_player(ggpo[i],&entry,&handle)==GGPO_OK);
                if(player==slot) {
                    localHandles[i]=handle;
                    CHECK(ggpo_set_frame_delay(ggpo[i],handle,beforeTable.inputDelay[slot])==GGPO_OK);
                }
            }
        }
        std::array<int,3> frames={0,0,0};
        std::array<bool,3> inputAdded={};
        int frameGoal=0;
        gameStep=[&]() {
            for(std::size_t i=1;i<=2;++i) {
                if(!ggpo[i]) continue;
                activeSession=ggpo[i];
                CHECK(ggpo_idle(ggpo[i],0)==GGPO_OK);
                if(frames[i] >= frameGoal) continue;
                if(!inputAdded[i]) {
                    std::array<unsigned char,session::GgpoInputBytes> input={};
                    for(std::size_t byte=0;byte<input.size();++byte)
                        input[byte]=static_cast<unsigned char>((frames[i]*73+byte*31+i)&255);
                    const auto result=ggpo_add_local_input(ggpo[i],localHandles[i],input.data(),static_cast<int>(input.size()));
                    if(result==GGPO_OK) inputAdded[i]=true;
                    else if(result!=GGPO_ERRORCODE_NOT_SYNCHRONIZED && result!=GGPO_ERRORCODE_PREDICTION_THRESHOLD) {
                        std::cerr << "Recovery GGPO input failed peer=" << i << " code=" << result << '\n';
                        CHECK(false);
                    }
                }
                if(!inputAdded[i]) continue;
                unsigned char synchronized[2*session::GgpoInputBytes]={}; int disconnected=0;
                const auto result=ggpo_synchronize_input(ggpo[i],synchronized,sizeof(synchronized),&disconnected);
                if(result==GGPO_ERRORCODE_NOT_SYNCHRONIZED || result==GGPO_ERRORCODE_PREDICTION_THRESHOLD) continue;
                CHECK(result==GGPO_OK && disconnected==0 && ggpo_advance_frame(ggpo[i])==GGPO_OK);
                ++frames[i]; inputAdded[i]=false;
            }
        };
        const auto runFrames=[&](int target) {
            frameGoal=target;
            const auto deadline=GetTickCount64()+120000;
            while(frames[1] < target || frames[2] < target) {
                pump(); Sleep(14);
                CHECK(GetTickCount64() < deadline);
            }
        };
        phase="started GGPO before leader loss";
        runFrames(60);
        std::array<session::IrohRoom::GameSnapshot,2> beforeGames={
            peers[1].room->Game(peers[2].room->LocalIdentity()),
            peers[2].room->Game(peers[1].room->LocalIdentity())};
        CHECK(beforeGames[0].state==session::IrohRoom::GameState::Ready && beforeGames[1].state==session::IrohRoom::GameState::Ready);
        if(fault==Fault::CommittedResult) {
            phase="native results before leader loss";gameStep={};
            const std::array<std::uint64_t,2> heldApplied={peers[1].recovery.AppliedRevision(),peers[2].recovery.AppliedRevision()};
            for(std::size_t i=1;i<=2;++i) {
                matches[i]->End();activeSession=ggpo[i];CHECK(ggpo_close_session(ggpo[i])==GGPO_OK);ggpo[i]=nullptr;
                CHECK(peers[i].client->Lobby_ReportResults(0)==session::SendResult::Queued);
            }
            // Helpers keep replicating and assembling checkpoints, but neither
            // fighter's native runtime/client/match consumes the terminal effects.
            std::vector<test::IrohIntegrationPeer*> leaderOnly{&peers[0]};
            wait([&]() {
                CHECK(test::PumpIrohIntegrationPeers(leaderOnly));
                const auto checkpoint=peers[0].server->RecoveryCheckpoint();
                if(peers[0].server->HasRecoveryCandidate() || peers[0].server->PendingProposal()) return false;
                for(const auto& receipt:checkpoint.at("room").at("terminal_receipts"))
                    if(receipt.at("table")==0 && receipt.at("generation")==generations[0]) {
                        CHECK(!receipt.at("acknowledged").get<bool>());
                        return peers[0].server->RoomSnapshot()->tables[0].score[1]==1;
                    }
                return false;
            });
            for(std::size_t i=1;i<=2;++i) {
                CHECK(peers[i].recovery.AppliedRevision()==heldApplied[i-1]);
                CHECK(peers[i].client->GetRoomSnapshot().tables[0].score[1]==0);
                room::Event event;while(peers[i].client->TakeRoomEvent(event))
                    CHECK(event.kind!=room::Event::Kind::MatchEnded || event.matchGeneration!=generations[0]);
            }
            processes[0].Stop(0);live.erase(live.begin());
            phase="committed result replay from successor";
            wait([&]() {
                pump();unsigned leaders=0;
                for(auto* peer:live) {
                    const auto& a=peer->room->Coordination();
                    if(!a.writable || !a.rebound || a.term<=oldTerm || !peer->recovery.CaughtUp(a)) return false;
                    leaders+=a.leaderLocal?1:0;
                }
                return leaders==1 && matches[1]->GetPhase()==MatchPhase::Idle && matches[2]->GetPhase()==MatchPhase::Idle;
            });
            CHECK(matches[1].get()==matchOne && matches[2].get()==matchTwo);
            for(std::size_t i=1;i<=2;++i) {
                const auto game=peers[i].room->Game(peers[i==1?2:1].room->LocalIdentity());
                CHECK(game.generation==generations[i-1] && game.state==session::IrohRoom::GameState::Closed);
            }
            phase="committed result recipient acknowledgment after migration";terminalAcks(generations[0],1);
            shutdownMatchPeers();
            std::cout << "Post-result leader loss replayed retained committed outcomes and retired original mappings before ACK; relay-only=" << relayOnly << '\n';
            return;
        }
        processes[0].Stop(0); live.erase(live.begin());
        phase="started GGPO majority recovery";
        const auto framesAtLoss=frames;
        frameGoal=180;
        wait([&]() {
            pump(); unsigned leaders=0;
            for(auto* peer:live) {
                const auto& authority=peer->room->Coordination();
                if(!authority.writable || !authority.rebound || authority.term<=oldTerm || !peer->recovery.CaughtUp(authority)) return false;
                leaders+=authority.leaderLocal?1:0;
            }
            return leaders==1 && matches[1]->GetPhase()==MatchPhase::Started && matches[2]->GetPhase()==MatchPhase::Started;
        });
        CHECK(frames[1]>framesAtLoss[1] && frames[2]>framesAtLoss[2]);
        const auto framesAtElection=frames;
        runFrames((std::max)(framesAtElection[1],framesAtElection[2])+120);
        CHECK(frames[1]>=framesAtElection[1]+120 && frames[2]>=framesAtElection[2]+120);
        phase="chat commit during started recovery";
        action(1,room::ActionKind::Chat,"Live GGPO survived majority recovery");
        wait([&]() {
            pump();
            for(auto* peer:live) {
                const auto& chat=peer->client->GetRoomSnapshot().chat;
                if(std::none_of(chat.begin(),chat.end(),[](const room::ChatMessage& message) { return message.text=="Live GGPO survived majority recovery"; })) return false;
            }
            return true;
        });
        CHECK(matches[1].get()==matchOne && matches[2].get()==matchTwo);
        CHECK(ggpo[1] && ggpo[2]);
        CHECK(matches[1]->Generation()==generations[0] && matches[2]->Generation()==generations[1]);
        CHECK(matches[1]->Roster()==rosterOne && matches[2]->Roster()==rosterTwo);
        for(std::size_t i=1;i<=2;++i) {
            const auto& table=peers[i].client->GetRoomSnapshot().tables[0];
            CHECK(table.inputDelay[0]==fighterOneDelay && table.inputDelay[1]==fighterTwoDelay);
            const auto game=peers[i].room->Game(i==1 ? peers[2].room->LocalIdentity() : peers[1].room->LocalIdentity());
            CHECK(game.state==session::IrohRoom::GameState::Ready && game.generation==generations[i-1]);
            CHECK(game.virtualPort==beforeGames[i-1].virtualPort);
            CHECK(!game.route.empty() && game.route!="unavailable");
            CHECK(game.routeChanges>=beforeGames[i-1].routeChanges);
            if(game.route!=beforeGames[i-1].route) CHECK(game.routeChanges>beforeGames[i-1].routeChanges);
            if(relayOnly) CHECK(game.route.rfind("relay:",0)==0);
            // Path migration updates telemetry without replacing the live
            // match generation, local sockets, roster, capability or delay.
            std::cout << "Recovered gameplay route changes=" << game.routeChanges
                << " current=" << game.route << '\n';
            CHECK(game.sentPackets>beforeGames[i-1].sentPackets && game.receivedPackets>beforeGames[i-1].receivedPackets);
            CHECK(peers[i].client->_ggpoPort==localPorts[i-1]);
            const auto afterRecovery=peers[i].server->RecoveryCheckpoint();
            CHECK(afterRecovery.contains("match_authorities") && afterRecovery["match_authorities"].is_array());
            CHECK(afterRecovery["match_authorities"].size() > 0 && !afterRecovery["match_authorities"][0].is_null());
            const auto afterCheckpoint=afterRecovery["match_authorities"][0];
            CHECK(afterCheckpoint.value("cap_digest",std::string())==authorityBefore[i-1].value("cap_digest",std::string()));
            CHECK(afterCheckpoint.value("participants",nlohmann::json::array())==authorityBefore[i-1].value("participants",nlohmann::json::array()));
        }
        gameStep={};
        for(std::size_t i=1;i<=2;++i) {
            matches[i]->End(); activeSession=ggpo[i]; CHECK(ggpo_close_session(ggpo[i])==GGPO_OK); ggpo[i]=nullptr;
            CHECK(peers[i].client->Lobby_ReportResults(0)==session::SendResult::Queued);
        }
        phase="matching confirmed results after started recovery";
        wait([&]() { pump(); return matches[1]->GetPhase()==MatchPhase::Idle && matches[2]->GetPhase()==MatchPhase::Idle; });
        std::array<bool,3> observedTerminals{}, queuedTerminalAcks{};
        phase="terminal acknowledgments after started recovery";
        wait([&]() {
            pump();
            for(std::size_t i=1;i<=2;++i)
                test::AcknowledgeIrohFixtureTerminal(*peers[i].client,0,generations[i-1],
                    ggpo[i]==nullptr && matches[i]->GetPhase()==MatchPhase::Idle,
                    observedTerminals[i],queuedTerminalAcks[i]);
            if(!queuedTerminalAcks[1] || !queuedTerminalAcks[2]) return false;
            for(auto* peer:live) {
                if(!test::IrohFixtureTerminalsCommitted(*peer->server,{{0,generations[0]}})) return false;
                const auto& table=peer->client->GetRoomSnapshot().tables[0];
                if(table.score[0]!=0 || table.score[1]!=1) return false;
            }
            return true;
        });
        matches[1].reset(); matches[2].reset();
        CHECK(helpers[1].Send("{\"type\":\"shutdown\"}")); CHECK(helpers[2].Send("{\"type\":\"shutdown\"}"));
        phase="started GGPO fixture shutdown";
        wait([&]() { for(std::size_t i=1;i<3;++i) if(processes[i].IsRunning()) return false; return true; });
        std::cout << "Started GGPO session survived technical-leader loss: 60 pre-loss + 120 post-election frames, stable grants/roster/ports and a new chat commit; relay-only=" << relayOnly << '\n';
        return;
    }
    std::uint64_t pendingActionId=0;
    if(count==3) {
        phase="commit before native effect delivery";
        // Use an action from the other fighter so this is not chat-rate limited.
        const auto view=peers[2].client->GetRoomSnapshot();
        room::Action request;request.kind=room::ActionKind::Chat;request.text="Committed before takeover";
        request.roomEpoch=view.roomEpoch;request.revision=view.revision;request.tableRevision=view.tables[0].revision;
        CHECK(peers[2].client->SendRoomAction(request,&pendingActionId)==session::SendResult::Queued);
        wait([&](){CHECK(peers[0].server->Step()==0);return peers[0].server->HasRecoveryCandidate();});
        CHECK(peers[0].server->ProposeCheckpoint(oldRevision+1,oldTerm,oldRevision,nullptr));
        const auto proposal=peers[0].server->PendingProposal();CHECK(proposal);
        CHECK(peers[0].room->ProposeCheckpointBytes(proposal->request,proposal->term,proposal->baseRevision,proposal->encoded));
        // Poll helper transfers and follower application only. The original
        // native owner never applies its committed candidate or flushes effects.
        wait([&](){for(std::size_t i=1;i<count;++i)
            CHECK(peers[i].recovery.Tick(*peers[i].server,*peers[i].room));
            return peers[1].recovery.AppliedRevision()==oldRevision+1 && peers[2].recovery.AppliedRevision()==oldRevision+1;});
        CHECK(peers[0].recovery.AppliedRevision()==oldRevision && peers[0].server->PendingProposal());
    }
    // Only this fixture's owned child is terminated. No game process is opened.
    processes[0].Stop(0);
    live.erase(live.begin());
    phase="control loss";
    wait([&](){pump();for(auto* peer:live) {
        const auto& authority=peer->room->Coordination();
        if(!authority.writable || (count==3 && authority.term>oldTerm)) return true;
    }return false;});
    if(count==3) {
        phase="majority authority recovery";
        wait([&](){pump();unsigned leaders=0;for(auto* peer:live) {
            const auto& a=peer->room->Coordination();
            if(!a.writable || !a.rebound || a.term<=oldTerm || !peer->recovery.CaughtUp(a)) return false;
            leaders+=a.leaderLocal?1:0;
        }return leaders==1;});
        unsigned acknowledgments=0;
        SessionClient::ActionReply reply;
        while(peers[2].client->TakeActionReply(reply)) if(reply.actionId==pendingActionId) {CHECK(reply.accepted);++acknowledgments;}
        CHECK(acknowledgments==1);
        phase="new native commitment after takeover";
        action(1,room::ActionKind::Chat,"After control recovery");
        action(1,room::ActionKind::Queue);
        action(2,room::ActionKind::Queue);
        action(1,room::ActionKind::Ready);
        action(1,room::ActionKind::Unready);
        wait([&](){pump();const auto revision=peers[1].recovery.AppliedRevision();
            auto expected=nlohmann::json(peers[1].client->GetRoomSnapshot());expected["local_member"]=0;
            for(auto* peer:live) {
            if(peer->room->RoomId()!=oldRoom || peer->recovery.AppliedRevision()<=oldRevision) return false;
            if(peer->recovery.AppliedRevision()!=revision || !peer->recovery.CaughtUp(peer->room->Coordination())) return false;
            auto snapshot=nlohmann::json(peer->client->GetRoomSnapshot());snapshot["local_member"]=0;if(snapshot!=expected) return false;
            const auto& table=peer->client->GetRoomSnapshot().tables[0];
            if(!table.p1 || !table.p2 || table.ready[0] || table.ready[1]) return false;
        }return true;});
        for(auto* peer:live) {
            const auto& chat=peer->client->GetRoomSnapshot().chat;
            CHECK(std::count_if(chat.begin(),chat.end(),[](const room::ChatMessage& message){return message.text=="Committed before takeover";})==1);
        }
        std::cout << "Majority recovered the original room and committed new C++ actions; relay-only=" << relayOnly << '\n';
    } else {
        phase="two-voter minority remains frozen";
        const auto until=GetTickCount64()+16000;
        do {pump();CHECK(!peers[1].room->Coordination().writable);CHECK(peers[1].room->RoomId()==oldRoom);Sleep(10);} while(GetTickCount64()<until);
        CHECK(peers[1].recovery.AppliedRevision()==oldRevision);
        room::Action blocked;blocked.kind=room::ActionKind::Chat;blocked.text="Blocked during recovery";
        CHECK(peers[1].client->SendRoomAction(blocked)==session::SendResult::NotConnected);
        CHECK(!peers[1].room->Host("recovery-integration"));
        phase="explicit replacement after local retirement";
        live.clear();peers[1].client.reset();peers[1].server.reset();peers[1].room->Leave(true);
        wait([&](){return peers[1].room->GetState()==session::IrohRoom::State::Idle;});
        CHECK(peers[1].room->Host("recovery-integration"));
        wait([&](){return peers[1].room->GetState()==session::IrohRoom::State::Ready;});
        CHECK(peers[1].room->RoomId()!=oldRoom && peers[1].room->Invitation().rfind("sf4e3:",0)==0);
        peers[1].recovery=session::RoomRecoveryRuntime{};
        CHECK(test::ConfigureIrohIntegrationPeer(peers[1],callbacks,"recovery-integration",31001));
        live.push_back(&peers[1]);
        wait([&](){pump();return peers[1].client->GetRoomSnapshot().members.size()==1;});
        action(1,room::ActionKind::Chat,"Replacement is usable");
        std::cout << "Two-voter minority stayed frozen; explicit replacement created fresh authority; relay-only=" << relayOnly << '\n';
    }
    for(std::size_t i=1;i<count;++i) CHECK(helpers[i].Send("{\"type\":\"shutdown\"}"));
    phase="fixture shutdown";
    wait([&](){for(std::size_t i=1;i<count;++i) if(processes[i].IsRunning()) return false;return true;});
}

int wmain(int argc,wchar_t** argv) {
    std::cout << std::unitbuf;
    CHECK(argc>=2 && argc<=4);
    bool relayOnly=false;
    std::wstring scenario=L"all";
    for(int i=2;i<argc;++i) {
        const std::wstring option=argv[i];
        if(option==L"--relay-only") { CHECK(!relayOnly);relayOnly=true; }
        else if(option.rfind(L"--scenario=",0)==0) { CHECK(scenario==L"all");scenario=option.substr(11); }
        else CHECK(false);
    }
    struct Case { const wchar_t* name; std::size_t count; Fault fault; };
    const Case cases[]={
        {L"majority",3,Fault::None},{L"minority",2,Fault::None},{L"departure",3,Fault::Departure},
        {L"started",3,Fault::Started},{L"preparing",3,Fault::Preparing},
        {L"preparing-minority",3,Fault::PreparingMinority},{L"same-term-preparing",3,Fault::SameTermPreparation},
        {L"committed-result",3,Fault::CommittedResult},{L"follower-killed",3,Fault::FollowerKilled}};
    CHECK(scenario==L"all" || std::any_of(std::begin(cases),std::end(cases),[&](const Case& c){return scenario==c.name;}));
    for(const auto& c:cases) if(scenario==L"all" || scenario==c.name) RunRecovery(argv[1],relayOnly,c.count,c.fault);
    std::cout << "Helper recovery integration passed. No native SF4 gameplay tested.\n";
}
