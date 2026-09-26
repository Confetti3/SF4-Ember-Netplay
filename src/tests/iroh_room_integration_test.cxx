#include "../session/IrohRoom.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionServer.hxx"
#include "iroh_integration_fixture.hxx"
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>

#include "test_support.hxx"
using namespace sf4e;

struct Observer { int ready = 0, synced = 0, errors = 0; SessionClient::ErrorType lastError=SessionClient::ErrorType::SCE_UNKNOWN; };
static SessionClient::Callbacks Callbacks(Observer& observer) {
	SessionClient::Callbacks c = {}; c.data = &observer;
	c.OnReady = [](SessionClient*, const SessionClient::Callbacks& c) { ++static_cast<Observer*>(c.data)->ready; };
	c.OnBattleSynced = [](SessionClient*, const SessionClient::Callbacks& c) { ++static_cast<Observer*>(c.data)->synced; };
	c.OnError = [](SessionClient::ErrorType error, SessionClient*, const SessionClient::Callbacks& c) {
		auto& observer=*static_cast<Observer*>(c.data); ++observer.errors; observer.lastError=error;
	};
	return c;
}

int wmain(int argc, wchar_t** argv) {
	std::cout << std::unitbuf;
	CHECK(argc >= 2 && argc <= 4);
	bool relayOnly=false, quick=false, queueAcks=false, probeCheck=false, benchmark=false, rejoin=false;
	for(int i=2;i<argc;++i) {
		const std::wstring option=argv[i];
		if(option==L"--relay-only") relayOnly=true;
		else if(option==L"--quick") quick=true;
		else if(option==L"--queue-acks") { queueAcks=true; quick=true; }
		else if(option==L"--probe") { probeCheck=true; quick=true; }
		else if(option==L"--benchmark") { probeCheck=true; benchmark=true; quick=true; }
		else if(option==L"--rejoin") { rejoin=true; quick=true; }
		else CHECK(false);
	}
	platform::HelperProcess hostProcess, guestProcess;
	CHECK(hostProcess.Start(argv[1], GetCurrentProcessId(), relayOnly));
	CHECK(guestProcess.Start(argv[1], GetCurrentProcessId(), relayOnly));
	platform::HelperClient hostHelper, guestHelper;
	CHECK(hostHelper.Start(hostProcess.Bootstrap()));
	CHECK(guestHelper.Start(guestProcess.Bootstrap()));
    auto host = std::make_shared<session::IrohRoom>(hostHelper);
    auto guest = std::make_shared<session::IrohRoom>(guestHelper);
    unsigned waitNumber = 0;
	auto waitFor = [&](std::chrono::seconds allowance, const std::function<bool()>& progress) {
        const auto attempt = ++waitNumber;
		const auto deadline = std::chrono::steady_clock::now() + allowance;
		do {
			if (progress()) return;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		} while (std::chrono::steady_clock::now() < deadline);
        std::cerr << "Wait " << attempt << " timed out: host_invitation=" << host->Invitation().substr(0,12) << "/" << host->DiscordInvitation().substr(0,12)
            << " guest_invitation=" << guest->Invitation().substr(0,12) << "/" << guest->DiscordInvitation().substr(0,12)
            << " host=" << static_cast<int>(host->GetState())
            << " error=" << host->Error() << " term=" << host->Coordination().term
            << " revision=" << host->Coordination().revision << " writable=" << host->Coordination().writable
            << " leader_local=" << host->Coordination().leaderLocal << " rebound=" << host->Coordination().rebound
            << " helper=" << static_cast<int>(hostHelper.State()) << '/' << hostHelper.LastError()
            << " guest=" << static_cast<int>(guest->GetState()) << " error=" << guest->Error()
            << " term=" << guest->Coordination().term << " revision=" << guest->Coordination().revision
            << " writable=" << guest->Coordination().writable << " leader_local=" << guest->Coordination().leaderLocal
            << " rebound=" << guest->Coordination().rebound << " helper=" << static_cast<int>(guestHelper.State())
            << '/' << guestHelper.LastError() << '\n';
		CHECK(false);
	};
	// Relay establishment and the 10-12 second Raft election window may occur
	// serially. Keep exercising the documented background-recovery path after
	// the UI's 15-second replacement offer instead of declaring a healthy,
	// degraded helper dead at 20 seconds.
	auto wait = [&](const std::function<bool()>& progress) { waitFor(std::chrono::seconds(45), progress); };
	wait([&]() { return hostHelper.State() == platform::HelperState::Connected && guestHelper.State() == platform::HelperState::Connected; });
	// Rejection must be observable in the same attempt and permit cancellation
	// and retry. An error tagged with the preceding room epoch would hang here.
	CHECK(guest->Join("not-an-invitation", "cpp-room-test"));
	wait([&]() { guest->Poll(); return guest->GetState() == session::IrohRoom::State::Failed; });
	CHECK(guest->Error() == "invalid_or_incompatible_invitation");
	guest->Leave();
	wait([&]() { guest->Poll(); return guest->GetState() == session::IrohRoom::State::Idle; });
	CHECK(host->Host("cpp-room-test"));
	auto obsoleteClient = host->Client();
	host->Leave();
	wait([&]() { host->Poll(); return host->GetState() == session::IrohRoom::State::Idle; });
	// No GameNetworkingSockets_Init: all lobby traffic below goes through
	// the native helper IPC/Iroh adapter or the host's in-process connection.
	for (int roomCycle = 0; roomCycle < (quick?1:3); ++roomCycle) {
		Observer hostObserver, guestObserver;
		test::IrohIntegrationPeer hostPeer, guestPeer;
		hostPeer.room = host; hostPeer.name = "Host";
		guestPeer.room = guest; guestPeer.name = "Guest";
		CHECK(host->Host("cpp-room-test"));
		obsoleteClient.reset(); // A prior epoch's destructor cannot leave this room.
		wait([&]() { host->Poll(); return host->GetState() == session::IrohRoom::State::Ready; });
		CHECK(!host->Invitation().empty());
        wait([&]() { host->Poll(); return host->DiscordInvitation().size()==127; });
		CHECK(test::ConfigureIrohIntegrationPeer(hostPeer, Callbacks(hostObserver), "cpp-room-test", 30000));
		auto hostClient = [&]() -> SessionClient& { return *hostPeer.client; };
		std::vector<test::IrohIntegrationPeer*> hostOnly{&hostPeer};
        const auto admissionStarted=GetTickCount64(); bool admissionDiagnostic=false;
		wait([&]() { CHECK(test::PumpIrohIntegrationPeers(hostOnly));
            if(!admissionDiagnostic && GetTickCount64()-admissionStarted>15000) {
                admissionDiagnostic=true;
                std::cerr << "Host admission: server members=" << hostPeer.server->RoomSnapshot()->members.size()
                    << " client members=" << hostClient().GetRoomSnapshot().members.size()
                    << " client local=" << hostClient().GetRoomSnapshot().localMember
                    << " native revision=" << hostPeer.recovery.AppliedRevision()
                    << " client error=" << hostClient().RoomError() << '\n';
                const auto recoveryState=hostPeer.server->RecoveryCheckpoint();
                for(const auto& effect:recoveryState.at("effect_journal"))
                    std::cerr << "Committed effect: " << effect.at("type") << " sequence=" << effect.at("sequence")
                        << " recipient=" << effect.at("recipient") << " revision=" << effect.at("revision") << '\n';
            }
            return hostPeer.server->ConnectedClientCount() == 1 && hostClient().GetRoomSnapshot().members.size()==1;
        });
		if(queueAcks) {
			// Hold native server consumption exactly as an outstanding committed
			// candidate does. Identical terminal retries cross the real local
			// adapter; no private queue state or test-only transport API is used.
			const auto original=hostClient().GetRoomSnapshot();
			SessionProtocol::RoomActionMessage acknowledgment;
			acknowledgment.action.kind=room::ActionKind::AcknowledgeTerminal;
			acknowledgment.action.roomEpoch=original.roomEpoch;
			acknowledgment.action.tableRevision=original.tables[0].revision;
			acknowledgment.action.actionId=1;
			acknowledgment.action.matchGeneration=1;
			nlohmann::json repeated=acknowledgment;
			for(unsigned retry=0;retry<192;++retry) {
				const auto sent=hostClient().Send(repeated,nullptr);
				if(sent!=session::SendResult::Queued)
					std::cerr << "Terminal retry " << retry << " send=" << static_cast<int>(sent)
						<< " room_error=" << host->Error() << '\n';
				CHECK(sent==session::SendResult::Queued);
			}
			CHECK(host->GetState()==session::IrohRoom::State::Ready);
			CHECK(hostPeer.server->RoomSnapshot()->revision==original.revision);
			acknowledgment.action.actionId=2;
			acknowledgment.action.matchGeneration=2;
			nlohmann::json distinct=acknowledgment;
			CHECK(hostClient().Send(distinct,nullptr)==session::SendResult::Queued);
			SessionProtocol::RoomActionMessage chat;
			chat.action.kind=room::ActionKind::Chat;
			chat.action.roomEpoch=original.roomEpoch;
			chat.action.revision=original.revision;
			chat.action.actionId=3;
			chat.action.text="Distinct action after terminal retries";
			nlohmann::json chatPayload=chat;
			CHECK(hostClient().Send(chatPayload,nullptr)==session::SendResult::Queued);
			std::vector<SessionClient::ActionReply> replies;
			wait([&]() {
				CHECK(test::PumpIrohIntegrationPeers(hostOnly));
				SessionClient::ActionReply reply;
				while(hostClient().TakeActionReply(reply)) replies.push_back(reply);
				return replies.size()>=3 && hostClient().GetRoomSnapshot().chat.size()==1 &&
					hostPeer.recovery.CaughtUp(host->Coordination()) &&
					!hostPeer.server->HasRecoveryCandidate() && !hostPeer.server->PendingProposal();
			});
			CHECK(replies.size()==3);
			CHECK(replies[0].actionId==1 && !replies[0].accepted);
			CHECK(replies[1].actionId==2 && !replies[1].accepted);
			CHECK(replies[2].actionId==3 && replies[2].accepted);
			CHECK(hostClient().GetRoomSnapshot().chat.front().text==chat.action.text);
			CHECK(hostClient().GetRoomSnapshot().tables[0].score[0]==original.tables[0].score[0] &&
				hostClient().GetRoomSnapshot().tables[0].score[1]==original.tables[0].score[1]);
			hostClient().Disconnect();
			hostPeer.server->Close();
			wait([&]() {host->Poll();return host->GetState()==session::IrohRoom::State::Idle;});
			CHECK(hostHelper.Send("{\"type\":\"shutdown\"}"));
			CHECK(guestHelper.Send("{\"type\":\"shutdown\"}"));
			wait([&]() {return !hostProcess.IsRunning() && !guestProcess.IsRunning();});
			std::cout << "192 identical terminal retries retained one queued action; distinct generation and Chat survived in order. No SF4 gameplay tested.\n";
			return 0;
		}
		if(roomCycle==0) {
			// Helper admission validates its build independently of the native
			// SessionClient hello. A mismatching native build must receive the
			// authenticated rejection, then permit a fresh room-control join.
			CHECK(guest->Join(host->Invitation(),"cpp-room-test"));
			wait([&]() {guest->Poll();CHECK(test::PumpIrohIntegrationPeers(hostOnly));
				return guest->GetState()==session::IrohRoom::State::Ready;});
			CHECK(test::ConfigureIrohIntegrationServer(guestPeer,"cpp-room-test"));
			Observer rejectedObserver;
			std::string rejectedName="Rejected";
			SessionClient rejected(Callbacks(rejectedObserver),"wrong-native-build",30002,rejectedName);
			rejected.RequireCustomRooms(); rejected.RequireMatchAuthorization();
			CHECK(rejected.Connect(guest->Client(),false)==0);
			std::vector<test::IrohServerPeer*> admissionPeers{&hostPeer,&guestPeer};
			wait([&]() {CHECK(test::PumpIrohRecoveryPeers(admissionPeers)); CHECK(hostClient().Step()==0);
				const auto step=rejected.Step();
				CHECK(step==0 || rejectedObserver.errors==1);
				return rejectedObserver.errors==1;});
			CHECK(hostPeer.server->RoomSnapshot()->members.size()==1);
			CHECK(rejectedObserver.lastError==SessionClient::ErrorType::SCE_JOIN_REJECTED_HASH_INVALID);
			guestPeer.server.reset();
			wait([&]() {guest->Poll(); CHECK(test::PumpIrohIntegrationPeers(hostOnly));
				return guest->GetState()==session::IrohRoom::State::Idle;});
			guestPeer.recovery=session::RoomRecoveryRuntime{};
			std::cout << "Rejected mismatching native build without hanging admission" << std::endl;
		}
		// Shared clipboards may carry a Windows line ending after the invitation.
		const auto pastedInvitation = roomCycle == 0 ? host->Invitation() + "\r\n" :
			(roomCycle == 1 ? host->DiscordInvitation() : host->Invitation());
		CHECK(guest->Join(pastedInvitation, "cpp-room-test"));
		wait([&]() { guest->Poll(); const bool pumped=test::PumpIrohIntegrationPeers(hostOnly);
			if(!pumped) std::cerr << "Helper diagnostic: host_state=" << static_cast<int>(hostHelper.State())
				<< " host_error=" << hostHelper.LastError() << " host_running=" << hostProcess.IsRunning()
				<< " guest_state=" << static_cast<int>(guestHelper.State())
				<< " guest_error=" << guestHelper.LastError() << " guest_running=" << guestProcess.IsRunning() << '\n';
			CHECK(pumped);
			return guest->GetState() == session::IrohRoom::State::Ready; });
		CHECK(test::ConfigureIrohIntegrationPeer(guestPeer, Callbacks(guestObserver), "cpp-room-test", 30001));
		auto guestClient = [&]() -> SessionClient& { return *guestPeer.client; };
		std::vector<test::IrohIntegrationPeer*> peers{&hostPeer, &guestPeer};
		auto pump = [&]() { CHECK(test::PumpIrohIntegrationPeers(peers)); };
		const auto nativeAdmissionStarted=GetTickCount64(); bool nativeAdmissionDiagnostic=false;
		waitFor(std::chrono::seconds(40), [&]() { pump(); const bool complete=hostClient()._lobbyData.members.size() == 2 && guestClient()._lobbyData.members.size() == 2 &&
			hostClient().GetRoomSnapshot().members.size()==2 && guestClient().GetRoomSnapshot().members.size()==2;
			if(!complete && !nativeAdmissionDiagnostic && GetTickCount64()-nativeAdmissionStarted>15000) {
				nativeAdmissionDiagnostic=true;
				std::cerr << "Native admission: host lobby=" << hostClient()._lobbyData.members.size()
					<< " room=" << hostClient().GetRoomSnapshot().members.size()
					<< " server=" << hostPeer.server->RoomSnapshot()->members.size()
					<< " applied=" << hostPeer.recovery.AppliedRevision() << " recovery=" << hostPeer.recovery.Error()
					<< " candidate=" << hostPeer.server->HasRecoveryCandidate()
					<< " pending=" << static_cast<bool>(hostPeer.server->PendingProposal())
					<< " in_flight=" << host->ProposalInFlight()
					<< " guest lobby=" << guestClient()._lobbyData.members.size()
					<< " room=" << guestClient().GetRoomSnapshot().members.size()
					<< " server=" << guestPeer.server->RoomSnapshot()->members.size()
					<< " applied=" << guestPeer.recovery.AppliedRevision() << " recovery=" << guestPeer.recovery.Error()
					<< " candidate=" << guestPeer.server->HasRecoveryCandidate()
					<< " pending=" << static_cast<bool>(guestPeer.server->PendingProposal())
					<< " in_flight=" << guest->ProposalInFlight()
					<< " host_client_error=" << hostClient().RoomError() << " guest_client_error=" << guestClient().RoomError() << '\n';
			}
			return complete; });
		wait([&]() { pump(); return guest->DiscordInvitation()==host->DiscordInvitation(); });
        CHECK(guest->Invitation()==host->Invitation());
		CHECK(hostClient().GetRoomSnapshot().members.size() == 2 && guestClient().GetRoomSnapshot().members.size() == 2);
		std::cout << "Room " << roomCycle+1 << ": both native members admitted with matching current invitations" << std::endl;
		const auto makeAction = [](SessionClient& client, room::ActionKind kind) {
			const auto& view = client.GetRoomSnapshot();
			room::Action action; action.kind = kind; action.table = 0;
			action.roomEpoch = view.roomEpoch; action.revision = view.revision;
			action.tableRevision = view.tables[0].revision; return action;
		};
		auto waitAction = [&](SessionClient& client, room::Action action, bool expected = true) {
			wait([&]() {pump(); const auto revision=hostPeer.server->RoomSnapshot()->revision;
				return hostClient().GetRoomSnapshot().revision==revision && guestClient().GetRoomSnapshot().revision==revision;});
			const auto& view=client.GetRoomSnapshot();
			action.roomEpoch=view.roomEpoch; action.revision=view.revision; action.tableRevision=view.tables[action.table].revision;
            std::uint64_t actionId=0;
			CHECK(client.SendRoomAction(action,&actionId) == session::SendResult::Queued);
			bool completed = false;
			wait([&]() {
				pump(); SessionClient::ActionReply reply;
                while(client.TakeActionReply(reply)) {
                    if(reply.actionId!=actionId) continue;
				    completed = reply.accepted; return true;
                }
                return false;
			});
			if(completed!=expected) std::cerr << "Unexpected action response: " << client.RoomError() << '\n';
			CHECK(completed == expected);
		};
		if(rejoin) {
			// A guest who leaves must be able to join the same room again through
			// the very ticket it was invited with, from the same helper process.
			// Committed: the runtime's normal Leave. Uncommitted: the client and
			// server are torn down first, as when the room is not writable, so the
			// helper must announce the departure itself. Unconfirmed: the host's
			// native room is not stepped while the guest leaves, so nothing can
			// confirm the departure and the runtime's bound abandons it.
			enum class Departure { Committed, Uncommitted, Unconfirmed };
			const Departure departures[]={Departure::Committed, Departure::Uncommitted, Departure::Unconfirmed, Departure::Committed};
			const auto ticket=host->DiscordInvitation();
			CHECK(ticket.size()==127);
			std::cout << "Rejoin identities: host=" << host->LocalIdentity().substr(0,13) << " guest=" << guest->LocalIdentity().substr(0,13) << std::endl;
			int attempt=0;
			for(const auto departure:departures) {
				++attempt;
				// Leave as a promoted voter every time: a learner's departure
				// needs no quorum proof, which is what the unconfirmed case
				// withholds.
				wait([&]() { pump(); return host->Coordination().voterCount==2; });
				if(departure==Departure::Committed) {
					waitAction(guestClient(), makeAction(guestClient(), room::ActionKind::Leave));
					wait([&]() { pump(); return guestClient().GetRoomSnapshot().localMember == 0; });
				}
				guestClient().Disconnect();
				guestPeer.client.reset();
				guestPeer.server.reset();
				guestPeer.configured=false;
				guestPeer.recovery=session::RoomRecoveryRuntime{};
				guest->Leave();
				const auto leaveStarted=GetTickCount64();
				const bool hostStepped=departure!=Departure::Unconfirmed;
				waitFor(std::chrono::seconds(20), [&]() { guest->Poll();
					if(hostStepped) CHECK(test::PumpIrohIntegrationPeers(hostOnly)); else hostPeer.recovery.Tick(*hostPeer.server, *host); // Import continues; only the native server step is withheld.
					return guest->GetState()==session::IrohRoom::State::Idle; });
				const auto departureMs=GetTickCount64()-leaveStarted;
				std::cout << "Rejoin " << attempt << ": guest idle after " << departureMs
					<< " ms (departure=" << static_cast<int>(departure) << ") guest_error='" << guest->Error() << "'"
					<< " host_connected=" << hostPeer.server->ConnectedClientCount()
					<< " host_members=" << hostClient().GetRoomSnapshot().members.size()
					<< " host_state=" << static_cast<int>(host->GetState()) << " host_error=" << host->Error()
					<< " host_writable=" << host->Coordination().writable << " voters=" << host->Coordination().voterCount
					<< " learners=" << host->Coordination().learnerCount << " ticket_length=" << host->DiscordInvitation().size() << std::endl;
				if(departure==Departure::Unconfirmed) {
					// The room's bound starts inside Leave(), a poll or two before this clock.
					CHECK(departureMs+250>=session::IrohRoom::LeaveTimeoutMs);
					CHECK(guest->Error()=="The room did not confirm your departure. You have left locally.");
					// The host resumes: the departure notice and the abandoned control
					// retire the seat before the guest is admitted again.
					wait([&]() { CHECK(test::PumpIrohIntegrationPeers(hostOnly)); return hostClient().GetRoomSnapshot().members.size()==1; });
				} else {
					CHECK(guest->Error().empty());
					CHECK(departureMs<session::IrohRoom::LeaveTimeoutMs/2);
				}
				CHECK(host->GetState()==session::IrohRoom::State::Ready && host->Coordination().writable);
				CHECK(guest->Join(ticket, "cpp-room-test"));
				const auto rejoinStarted=GetTickCount64();
				wait([&]() { guest->Poll(); CHECK(test::PumpIrohIntegrationPeers(hostOnly));
					if(guest->GetState()==session::IrohRoom::State::Failed) {
						std::cerr << "Rejoin " << attempt << " failed after " << GetTickCount64()-rejoinStarted << " ms: " << guest->Error()
							<< " host_connected=" << hostPeer.server->ConnectedClientCount()
							<< " host_members=" << hostClient().GetRoomSnapshot().members.size() << '\n';
						CHECK(false);
					}
					return guest->GetState()==session::IrohRoom::State::Ready; });
				std::cout << "Rejoin " << attempt << ": helper ready after " << GetTickCount64()-rejoinStarted << " ms" << std::endl;
				CHECK(test::ConfigureIrohIntegrationPeer(guestPeer, Callbacks(guestObserver), "cpp-room-test", 30001));
				bool rejoinDiagnostic=false;
				waitFor(std::chrono::seconds(40), [&]() { pump();
					const bool complete=hostClient().GetRoomSnapshot().members.size()==2 && guestClient().GetRoomSnapshot().members.size()==2;
					if(!complete && !rejoinDiagnostic && GetTickCount64()-rejoinStarted>15000) {
						rejoinDiagnostic=true;
						std::cerr << "Rejoin " << attempt << " native admission: host room=" << hostClient().GetRoomSnapshot().members.size()
							<< " server=" << hostPeer.server->RoomSnapshot()->members.size()
							<< " guest room=" << guestClient().GetRoomSnapshot().members.size()
							<< " host_client_error=" << hostClient().RoomError() << " guest_client_error=" << guestClient().RoomError()
							<< " guest_room_error=" << guest->Error() << " guest_state=" << static_cast<int>(guest->GetState())
							<< " host_recovery='" << hostPeer.recovery.Error() << "' guest_recovery='" << guestPeer.recovery.Error() << "'"
							<< " host_applied=" << hostPeer.recovery.AppliedRevision() << " guest_applied=" << guestPeer.recovery.AppliedRevision()
							<< " host_helper_revision=" << host->Coordination().revision << " guest_helper_revision=" << guest->Coordination().revision << '\n';
						for(const auto& [connection, identity] : host->ControlIdentities())
							std::cerr << "  host view: connection=" << connection << " identity=" << identity.substr(0,8) << " incarnation=" << host->PeerIncarnation(connection) << '\n';
						for(const auto& [connection, identity] : guest->ControlIdentities())
							std::cerr << "  guest view: connection=" << connection << " identity=" << identity.substr(0,8) << " incarnation=" << guest->PeerIncarnation(connection) << '\n';
						for(const auto& member : hostPeer.server->RoomSnapshot()->members)
							std::cerr << "  host room member=" << member.id << " incarnation=" << (hostPeer.server->roomIncarnations.count(member.id) ? hostPeer.server->roomIncarnations.at(member.id) : 0) << '\n';
					}
					return complete; });
				std::cout << "Rejoin " << attempt << ": native admission after " << GetTickCount64()-rejoinStarted << " ms" << std::endl;
				wait([&]() { pump(); return guest->DiscordInvitation()==host->DiscordInvitation(); });
				waitAction(guestClient(), makeAction(guestClient(), room::ActionKind::Queue));
				waitAction(guestClient(), makeAction(guestClient(), room::ActionKind::Unqueue));
			}
			std::cout << "Rejoined the same room through the same Discord ticket after four departures" << std::endl;
		}
		// Custom-room actions are the only admission/readiness path once the
		// client requires room authority. Legacy LobbyReady callbacks are not a
		// substitute for a committed room action.
		waitAction(hostClient(), makeAction(hostClient(), room::ActionKind::Queue));
		waitAction(guestClient(), makeAction(guestClient(), room::ActionKind::Queue));
        // An action reply can precede the other client's updated projection.
        // Wait for the complete pair on each source, and for native ownership
        // when this test will exercise the native player callbacks below.
        wait([&]() { pump(); return std::all_of(peers.begin(),peers.end(),[&](const auto* peer) {
            const auto& table=peer->client->GetRoomSnapshot().tables[0];
            return table.p1 && table.p2 && (probeCheck || peer->client->IsLocalPlayer());
        }); });
        if(probeCheck) {
            CHECK(hostClient().GetRoomSnapshot().tables[0].p1==hostClient().GetRoomSnapshot().localMember);
            CHECK(guestClient().GetRoomSnapshot().tables[0].p2==guestClient().GetRoomSnapshot().localMember);
            for(const auto& source:peers) {
                const auto target=source==&hostPeer?guest:host;
                const auto revision=source->client->GetRoomSnapshot().tables[0].revision;
                CHECK(source->room->RequestProbe(target->LocalIdentity(),1,revision,benchmark));
                wait([&]() {
                    pump(); const auto& probe=source->room->Probe();
                    if(probe.status!="checking")std::cout << "Two-peer probe: status=" << probe.status
                        << " sent=" << probe.sent << " replies=" << probe.samples
                        << " rejection=" << probe.failureReason << '\n';
                    CHECK(probe.status!="unavailable"&&probe.status!="timed_out"&&probe.status!="invalidated");
                    return probe.status=="ready"||probe.status=="complete";
                });
                const auto& probe=source->room->Probe();
                CHECK(probe.sent==(benchmark?600U:100U));
                CHECK(probe.samples>=(benchmark?480U:80U));
                CHECK(probe.recommended>=0);
            }
            // An invalid revision must remain rejected, with a readable reason
            // transported from the real helper and no damage to room control.
            CHECK(host->RequestProbe(guest->LocalIdentity(),2,
                hostClient().GetRoomSnapshot().tables[0].revision+1000));
            wait([&]() { pump(); return host->Probe().status=="unavailable"; });
            CHECK(host->Probe().failureReason==8 && host->Probe().sent==0 && host->Probe().recommended==-1);
            CHECK(host->GetState()==session::IrohRoom::State::Ready);
            std::cout << "Stale probe rejected with reason 8; room control remains ready\n";
        }
        auto rules=makeAction(hostClient(),room::ActionKind::SetRules);
        rules.rules=hostClient().GetRoomSnapshot().tables[0].rules;
        rules.rules.roundCount=5; rules.rules.roundTime=60; rules.rules.editionSelect=false;
        waitAction(hostClient(),rules);
		wait([&]() {
			pump(); return
				guestClient().GetRoomSnapshot().tables[0].rules.roundCount == 5 &&
				!guestClient().GetRoomSnapshot().tables[0].rules.editionSelect;
		});
		// Repeatedly Ready/Unready only one fighter. The second fighter never
		// readies, so this fixture cannot start a native generation.
		for (int cycle = 0; cycle < (quick?0:30); ++cycle) {
			hostClient().SetSelectedDelay(static_cast<unsigned>(cycle % 11));
            auto ready=makeAction(hostClient(),room::ActionKind::Ready);
            ready.inputDelay=static_cast<std::uint8_t>(cycle%11);
			waitAction(hostClient(),ready);
			CHECK(hostClient().GetRoomSnapshot().tables[0].ready[0] &&
				hostClient().GetRoomSnapshot().tables[0].phase == room::TablePhase::Waiting);
			waitAction(hostClient(), makeAction(hostClient(), room::ActionKind::Unready));
			CHECK(!hostClient().GetRoomSnapshot().tables[0].ready[0] &&
				hostClient().GetRoomSnapshot().tables[0].phase == room::TablePhase::Waiting);
		}
		// Retire the guest through the committed room Leave response first. The
		// transport close then performs the helper membership departure, avoiding
		// an uncommitted two-voter quorum loss.
		waitAction(guestClient(), makeAction(guestClient(), room::ActionKind::Leave));
		wait([&]() { pump(); return guestClient().GetRoomSnapshot().localMember == 0; });
		guestClient().Disconnect();
		wait([&]() { guest->Poll(); CHECK(test::PumpIrohIntegrationPeers(hostOnly));
			return guest->GetState() == session::IrohRoom::State::Idle && hostPeer.server->ConnectedClientCount() == 1; });
		CHECK(guest->Invitation().empty() && guest->DiscordInvitation().empty());
		hostClient().Disconnect();
		hostPeer.server->Close();
		wait([&]() { host->Poll(); return host->GetState() == session::IrohRoom::State::Idle; });
		CHECK(hostObserver.errors == 0 && guestObserver.errors == 0);
	}
	CHECK(hostHelper.Send("{\"type\":\"shutdown\"}"));
	CHECK(guestHelper.Send("{\"type\":\"shutdown\"}"));
	wait([&]() { return !hostProcess.IsRunning() && !guestProcess.IsRunning(); });
	std::cout << "C++ SessionClient/SessionServer over two Iroh helpers passed: " << (quick?1:3)
		<< " rooms, " << (quick?0:90) << " committed Ready/Unready cycles, neutral Leave, relay-only="
		<< relayOnly << ". No SF4 gameplay tested.\n";
}
