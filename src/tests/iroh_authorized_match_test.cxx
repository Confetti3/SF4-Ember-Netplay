#include "../session/IrohMatchSession.hxx"
#include "../session/sf4e__SessionServer.hxx"
#include "iroh_integration_fixture.hxx"
#include <ggponet.h>
#include <cstdlib>
#include <functional>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)
using namespace sf4e;
static GGPOSession* activeSession = nullptr;
static GGPOSessionCallbacks GameCallbacks() {
	GGPOSessionCallbacks callbacks = {};
	callbacks.begin_game = [](const char*) { return true; };
	callbacks.save_game_state = [](unsigned char** data, int* size, int* checksum, int) {
		*size = 1; *checksum = 0; *data = static_cast<unsigned char*>(std::calloc(1, 1)); return *data != nullptr;
	};
	callbacks.load_game_state = [](unsigned char*, int) { return true; };
	callbacks.log_game_state = [](char*, unsigned char*, int) { return true; };
	callbacks.free_buffer = [](void* data) { std::free(data); };
	callbacks.advance_frame = [](int) {
		unsigned char inputs[2 * session::GgpoInputBytes] = {}; int disconnected = 0;
		return ggpo_synchronize_input(activeSession, inputs, sizeof(inputs), &disconnected) == GGPO_OK && ggpo_advance_frame(activeSession) == GGPO_OK;
	};
	callbacks.on_event = [](GGPOEvent*) { return true; };
	return callbacks;
}

static Dimps::GameEvents::VsMode::ConfirmedCharaConditions Chara(int id) {
	Dimps::GameEvents::VsMode::ConfirmedCharaConditions result{};
	result.charaID = static_cast<std::uint8_t>(id);
	// Keep the test on the base edition and use the same valid values the
	// native lobby bridge accepts for a custom-room pre-battle selection.
	result.unc_edition = 14;
	return result;
}

// Fault injection stays in the fixture. Raft continues in the helper, while
// the native consumer loses complete control/checkpoint deliveries. Validate
// and acknowledge bulk transfers so the loss does not fill the IPC mailbox.
struct DiscardedControlStream {
	coordination::CheckpointReceiver receiver;
	nlohmann::json pendingAck = nullptr;
	std::uint64_t revision = 0, checkpoints = 0, terminalMessages = 0;
	std::set<std::string> terminalTokens;
	static std::string TokenKey(const session::EffectEnvelope& effect) {
		return std::to_string(effect.term) + ":" + std::to_string(effect.sequence) + ":" + effect.payloadDigest;
	}
	bool Pump(platform::HelperClient& helper) {
		if (!pendingAck.is_null()) {
			if (!helper.Send(pendingAck.dump())) return true;
			pendingAck = nullptr;
		}
		platform::HelperMessage frame;
		for (int budget = 0; budget < 128 && helper.TryReceive(frame); ++budget) {
			const auto event = nlohmann::json::parse(frame.payload);
			const auto type = event.value("type", std::string());
			if (type == "checkpoint_begin") {
				CHECK(!receiver.Active());
				CHECK(receiver.Begin(event, GetTickCount64()));
			} else if (type == "checkpoint_chunk" || type == "checkpoint_end") {
				// A transfer begun before the fault may finish here. Its suffix
				// is discarded, never imported; subsequent complete heads use
				// the normal identity, length and digest validator above.
				std::size_t offset = 0;
				if (receiver.Active()) {
					CHECK(type == "checkpoint_chunk" ? receiver.Chunk(event) : receiver.End(event));
					offset = receiver.Offset();
				} else if (type == "checkpoint_chunk") {
					std::string bytes;
					CHECK(coordination::Decode(event.at("data"), bytes, coordination::CheckpointChunk));
					offset = event.at("offset").get<std::size_t>() + bytes.size();
					CHECK(offset <= coordination::MaximumCheckpoint);
				} else {
					offset = event.at("length");
					CHECK(offset <= coordination::MaximumCheckpoint);
				}
				pendingAck = {{"type", "checkpoint_ack"}, {"epoch", event.at("epoch")},
					{"room", event.at("room")}, {"transfer", event.at("transfer")}, {"offset", offset}};
				if (!helper.Send(pendingAck.dump())) return true;
				pendingAck = nullptr;
			} else if (type == "checkpoint_committed") {
				if (receiver.Active()) {
					CHECK(receiver.Complete() && receiver.Identity().Matches(event));
					CHECK(event.at("digest") == receiver.Identity().digest);
					revision = receiver.Identity().revision;
					++checkpoints;
					receiver.Reset();
				}
			} else if (type == "message") {
				auto payload = nlohmann::json::parse(event.at("payload").get<std::string>());
				const auto payloadType = payload.value("type", std::string());
				const bool terminal = payloadType == "game_end" || (payloadType == "room_event" &&
					payload.at("event").at("kind") == static_cast<int>(room::Event::Kind::MatchEnded));
				if (terminal) {
					CHECK(payload.contains("_commit"));
					const auto token = payload.at("_commit").get<session::EffectEnvelope>();
					payload.erase("_commit");
					CHECK(token.payloadDigest == session::PayloadDigest(payload));
					CHECK(terminalTokens.size() < 128);
					terminalTokens.insert(TokenKey(token));
					++terminalMessages;
				}
			} else if (type == "game_closed" || type == "game_failed" || type == "error") {
				std::cerr << "Unexpected lifecycle event while only native control is held: " << event.dump() << '\n';
				return false;
			}
		}
		return true;
	}
};

int wmain(int argc, wchar_t** argv) {
	std::cout << std::unitbuf;
	CHECK(argc >= 2 && argc <= 4);
	bool relayOnly = false, terminalRecovery = false, benchmark = false;
	for (int i = 2; i < argc; ++i) {
		const std::wstring option = argv[i];
		if (option == L"--relay-only") relayOnly = true;
		else if (option == L"--benchmark") benchmark = true;
		else if (option == L"--terminal-recovery") terminalRecovery = true;
		else CHECK(false);
	}
	constexpr std::size_t Count = 4;
	std::array<platform::HelperProcess, Count> processes;
	std::array<platform::HelperClient, Count> helpers;
	std::array<std::shared_ptr<session::IrohRoom>, Count> rooms;
	std::array<test::IrohServerPeer, Count> recoveryPeers;
	std::array<std::unique_ptr<SessionClient>, Count> clients;
	std::array<std::unique_ptr<session::IrohMatchSession>, Count> matches;
	std::array<GGPOSession*, Count> ggpo = {};
	std::array<bool, Count> held{};
	std::array<DiscardedControlStream, Count> discarded;
	bool holdMatchTicks = false;
	ULONGLONG matchClockOffset = 0;
	std::array<std::string, Count> names = {"Host", "Guest", "Spectator A", "Spectator B"};
	for (std::size_t i = 0; i < Count; ++i) {
		CHECK(processes[i].Start(argv[1], GetCurrentProcessId(), relayOnly));
		CHECK(helpers[i].Start(processes[i].Bootstrap()));
		rooms[i] = std::make_shared<session::IrohRoom>(helpers[i]);
		SessionClient::Callbacks callbacks = {};
		callbacks.OnError = [](SessionClient::ErrorType, SessionClient*, const SessionClient::Callbacks&) { CHECK(false); };
		clients[i].reset(new SessionClient(callbacks, "authorized-match-test", 0, names[i]));
		clients[i]->SetProfileMain(i == 0 ? 2 : i == 1 ? 7 : 12 + static_cast<int>(i));
	}
	unsigned waitNumber=0;
	std::function<void()> serviceAdmissions;
	auto wait = [&](const std::function<bool()>& progress) {
		const auto attempt=++waitNumber;
		const auto deadline = GetTickCount64() + (benchmark ? 50000 : 25000);
		do {
			for (std::size_t i = 0; i < Count; ++i) {
				if (held[i]) CHECK(discarded[i].Pump(helpers[i]));
				else rooms[i]->Poll();
			}
			if (serviceAdmissions) serviceAdmissions();
			if (progress()) return;
			Sleep(2);
		} while (GetTickCount64() < deadline);
		std::cerr << "Authorized wait " << attempt << " timed out\n";
		for(std::size_t i=0;i<Count;++i) {
			const auto& authority=rooms[i]->Coordination();const auto& probe=rooms[i]->Probe();
			std::cerr << i << " room=" << static_cast<int>(rooms[i]->GetState()) << " error=" << rooms[i]->Error()
				<< " term=" << authority.term << " revision=" << authority.revision << " writable=" << authority.writable
				<< " applied=" << recoveryPeers[i].recovery.AppliedRevision() << " members=" << clients[i]->GetRoomSnapshot().members.size()
				<< " recovery_error=" << recoveryPeers[i].recovery.Error()
				<< " probe=" << probe.status << " samples=" << probe.samples;
			if(matches[i]) std::cerr << " phase=" << static_cast<int>(matches[i]->GetPhase()) << " match=" << matches[i]->Error();
			std::cerr << '\n';
		}
		CHECK(false);
	};
	wait([&]() { return std::all_of(helpers.begin(), helpers.end(), [](const platform::HelperClient& helper) { return helper.State() == platform::HelperState::Connected; }); });
	CHECK(rooms[0]->Host("authorized-match-test"));
	wait([&]() { return rooms[0]->GetState() == session::IrohRoom::State::Ready; });
	for (std::size_t i = 0; i < Count; ++i) recoveryPeers[i].room = rooms[i];
	std::vector<test::IrohServerPeer*> admittedPeers;
	std::vector<SessionClient*> admittedClients;
	serviceAdmissions = [&]() {
		CHECK(test::PumpIrohRecoveryPeers(admittedPeers));
		CHECK(test::PumpIrohIntegrationClients(admittedClients));
	};
	for (std::size_t i = 0; i < Count; ++i) {
		if (i) {
			CHECK(rooms[i]->Join(rooms[0]->Invitation(), "authorized-match-test"));
			wait([&]() { return rooms[i]->GetState() == session::IrohRoom::State::Ready; });
		}
		CHECK(test::ConfigureIrohIntegrationServer(recoveryPeers[i], "authorized-match-test", static_cast<std::uint8_t>(Count)));
		clients[i]->RequireCustomRooms();
		clients[i]->RequireMatchAuthorization();
		CHECK(clients[i]->Connect(rooms[i]->Client(), false) == 0);
		matches[i].reset(new session::IrohMatchSession(*clients[i], rooms[i],
			[&]() { return GetTickCount64() + matchClockOffset; }));
		admittedPeers.push_back(&recoveryPeers[i]); admittedClients.push_back(clients[i].get());
		wait([&]() {
			return std::all_of(admittedClients.begin(), admittedClients.end(), [&](const SessionClient* client) {
				return client->GetRoomSnapshot().members.size() == admittedClients.size();
			});
		});
	}
	serviceAdmissions = {};
	auto& server = *recoveryPeers[0].server;
	std::vector<test::IrohServerPeer*> recovery;
	for (auto& peer : recoveryPeers) recovery.push_back(&peer);
	std::vector<SessionClient*> clientViews;
	for (auto& client : clients) clientViews.push_back(client.get());
	auto pump = [&]() {
		std::vector<test::IrohServerPeer*> activeRecovery;
		std::vector<SessionClient*> activeClients;
		for (std::size_t i = 0; i < Count; ++i) if (!held[i]) {
			activeRecovery.push_back(recovery[i]); activeClients.push_back(clientViews[i]);
		}
		CHECK(test::PumpIrohRecoveryPeers(activeRecovery));
		CHECK(test::PumpIrohIntegrationClients(activeClients));
		for (std::size_t i = 0; i < Count; ++i) {
			if (held[i] || holdMatchTicks) continue;
			if (!matches[i]->Tick(ggpo[i] != nullptr)) {
				const auto& authority=rooms[i]->Coordination();
				std::cerr << "match " << i << " " << matches[i]->Error()
					<< " generation=" << matches[i]->Generation() << " room=" << static_cast<int>(rooms[i]->GetState())
					<< " room_error=" << rooms[i]->Error() << " term=" << authority.term
					<< " revision=" << authority.revision << " writable=" << authority.writable
					<< " rebound=" << authority.rebound << " applied=" << recoveryPeers[i].recovery.AppliedRevision() << '\n';
				for(std::size_t peer=0;peer<Count;++peer) if(peer!=i) {
					const auto game=rooms[i]->Game(rooms[peer]->LocalIdentity());
					std::cerr << " game peer=" << peer << " generation=" << game.generation
						<< " state=" << static_cast<int>(game.state) << " port=" << game.virtualPort << " route=" << game.route << '\n';
				}
				CHECK(false);
			}
		}
	};
	wait([&]() { pump(); return std::all_of(clients.begin(), clients.end(), [&](const std::unique_ptr<SessionClient>& client) { return client->GetRoomSnapshot().members.size() == Count; }); });
	auto roomAction = [](SessionClient& client, room::ActionKind kind) {
		const auto& view = client.GetRoomSnapshot(); room::Action action;
		action.kind = kind; action.table = 0; action.roomEpoch = view.roomEpoch;
		action.revision = view.revision; action.tableRevision = view.tables[0].revision;
		return action;
	};
	auto waitRoomAction = [&](SessionClient& client, room::Action action) {
		std::uint64_t actionId=0;
		wait([&](){pump();const auto revision=server.RoomSnapshot()->revision;
			for (std::size_t i = 0; i < Count; ++i)
				if (!held[i] && clients[i]->GetRoomSnapshot().revision != revision) return false;
			const auto& view=client.GetRoomSnapshot(); action.roomEpoch=view.roomEpoch;
			action.revision=view.revision; action.tableRevision=view.tables[action.table].revision;
			const auto sent=client.SendRoomAction(action,&actionId);
			if (sent == session::SendResult::NotConnected || sent == session::SendResult::QueueFull) return false;
			CHECK(sent == session::SendResult::Queued);
			return true;
		});
		wait([&]() { pump(); SessionClient::ActionReply reply;
			while(client.TakeActionReply(reply)) if(reply.actionId==actionId) {
				if(!reply.accepted) std::cerr << "Room action rejected: " << client.RoomError() << '\n';
				CHECK(reply.accepted);return true;
			}return false;});
	};
	// IrohMatchSession observes the custom-room seats. Queue the two fighters
	// and explicitly put the other two members into the spectator path before
	// any Ready action is attempted.
	waitRoomAction(*clients[0], roomAction(*clients[0], room::ActionKind::Queue));
	waitRoomAction(*clients[1], roomAction(*clients[1], room::ActionKind::Queue));
	waitRoomAction(*clients[2], roomAction(*clients[2], room::ActionKind::Watch));
	waitRoomAction(*clients[3], roomAction(*clients[3], room::ActionKind::Watch));
	wait([&]() { pump();
		auto watching = [](const SessionClient& client) {
			const auto& view = client.GetRoomSnapshot();
			const auto member = std::find_if(view.members.begin(), view.members.end(), [&](const room::Member& row) { return row.id == view.localMember; });
			return member != view.members.end() && member->status == room::MemberStatus::WatchingNext &&
				std::find(view.tables[0].spectators.begin(),view.tables[0].spectators.end(),view.localMember)!=view.tables[0].spectators.end();
		};
		return clients[0]->GetRoomSnapshot().tables[0].p1 && clients[1]->GetRoomSnapshot().tables[0].p2 &&
			watching(*clients[2]) && watching(*clients[3]); });
	const auto probePeer = rooms[1]->LocalIdentity();
	CHECK(probePeer.size() == 64);
	wait([&]() {
		pump();
		const auto control = rooms[0]->ConnectionForIdentity(probePeer);
		return control != 0 && rooms[0]->PeerIncarnation(control) != 0;
	});
	// Match the player UI: probe occupied seats before Ready sends selections.
	CHECK(clients[0]->GetRoomSnapshot().members[0].fighter == -1);
	const auto pairRevision = clients[0]->GetRoomSnapshot().tables[0].revision;
	CHECK(rooms[0]->RequestProbe(probePeer, 1, pairRevision, benchmark));
	wait([&]() {
		pump();
		const auto& probe = rooms[0]->Probe();
		return (probe.status == "ready" || probe.status == "complete") && probe.samples >= (benchmark ? 480U : 80U) &&
            probe.sent == (benchmark ? 600U : 100U) &&
			probe.samples + probe.lost == (benchmark ? 600 : 100) && probe.route != "" && probe.recommended >= 0;
	});
	const auto probeRoute = rooms[0]->Probe().route;
	std::cout << "Probe route=" << probeRoute << " valid=" << rooms[0]->Probe().samples
		<< " lost=" << rooms[0]->Probe().lost << " p95_rtt_us=" << rooms[0]->Probe().p95RttUs
		<< " recommended_delay=" << rooms[0]->Probe().recommended << '\n';
	const auto probeControl = rooms[0]->ConnectionForIdentity(probePeer);
	CHECK(probeControl != 0);
    const auto& measured=rooms[0]->Probe();
    if(benchmark) CHECK(measured.packetBytes==session::GgpoMaximumPacket+26);
    std::cout << "Datagram measurement bytes=" << measured.packetBytes << " sent=" << measured.sent << " scheduled=" << measured.expected
        << " replies=" << measured.samples << " missed=" << measured.lost << " p50_us=" << measured.p50RttUs
        << " p95_us=" << measured.p95RttUs << " p99_us=" << measured.p99RttUs << " variation_us=" << measured.jitterUs << '\n';
	CHECK(rooms[1]->RequestProbe(rooms[0]->LocalIdentity(), 1, pairRevision));
	wait([&]() {
		pump();
		const auto& probe = rooms[1]->Probe();
		return (probe.status == "ready" || probe.status == "complete") && probe.samples >= 80 && probe.recommended >= 0;
	});

	std::cout << "Both seated players completed the connection check before Ready\n";
    const auto finalProbeRoute=rooms[1]->Probe().route;
    wait([&](){pump();return rooms[0]->Probe().status=="invalidated";});
	// The pair reservation is bound to the committed native table, including
	// both selected fighters and the exact table revision.  Exercise the same
	// pre-battle path the game uses before asking the helper to reserve QUIC.
	CHECK(clients[0]->PreBattle_SetChara(Chara(2)) == session::SendResult::Queued);
	CHECK(clients[1]->PreBattle_SetChara(Chara(7)) == session::SendResult::Queued);
	wait([&]() {
		pump();
		return std::all_of(clients.begin(), clients.end(), [](const std::unique_ptr<SessionClient>& client) {
			const auto& table = client->GetRoomSnapshot().tables[0];
			const auto find = [&](room::MemberId member) {
				const auto row = std::find_if(client->GetRoomSnapshot().members.begin(), client->GetRoomSnapshot().members.end(),
					[&](const room::Member& value) { return value.id == member; });
				return row == client->GetRoomSnapshot().members.end() ? -1 : row->fighter;
			};
			return table.p1 && table.p2 && find(table.p1) == 2 && find(table.p2) == 7;
		});
	});
	using Phase = session::IrohMatchSession::Phase;
	for (std::uint64_t cycle = 1; cycle <= 3; ++cycle) {
		const unsigned p1Delay = static_cast<unsigned>(cycle % 11);
		const unsigned p2Delay = static_cast<unsigned>((cycle * 3) % 11);
		clients[0]->SetSelectedDelay(p1Delay);
		clients[1]->SetSelectedDelay(p2Delay);
		for (std::size_t player=0;player<2;++player) {
			auto ready=roomAction(*clients[player],room::ActionKind::Ready);ready.inputDelay=static_cast<std::uint8_t>(player?p2Delay:p1Delay);
			waitRoomAction(*clients[player],ready);
		}
		wait([&]() { pump(); return std::all_of(matches.begin(), matches.end(), [](const std::unique_ptr<session::IrohMatchSession>& match) { return match->GetPhase() == Phase::Started; }); });
		for (const auto& match : matches) CHECK(match->Generation() == cycle);
		std::array<std::map<std::string, std::uint16_t>, Count> authorizedEdges;
		for (std::size_t i = 0; i < Count; ++i) {
			for (std::size_t peer = 0; peer < Count; ++peer) if (i != peer) {
				const auto endpoint = rooms[peer]->LocalIdentity();
				const auto game = rooms[i]->Game(endpoint);
				if (game.generation == cycle) {
					CHECK(game.state == session::IrohRoom::GameState::Ready && game.virtualPort != 0);
					CHECK(!game.route.empty());
					if (relayOnly) CHECK(game.route.rfind("relay:",0)==0);
					authorizedEdges[i].emplace(endpoint, game.virtualPort);
				}
			}
			CHECK(authorizedEdges[i].size() == (matches[i]->LocalSlot() == 0 ? Count - 1 : 1));
		}
		const auto& readyTable = clients[0]->GetRoomSnapshot().tables[0];
		CHECK(readyTable.inputDelay[0] == p1Delay && readyTable.inputDelay[1] == p2Delay);
		std::cout << "Generation " << cycle << " selected game routes=" << rooms[0]->Game(probePeer).route
			<< "," << rooms[1]->Game(rooms[0]->LocalIdentity()).route
			<< " frozen_delays=" << p1Delay << "," << p2Delay << '\n';
		if (cycle == 1) {
			// Gameplay must upgrade the most recent check, initiated by P2.
            // P1's earlier recommendation was explicitly invalidated. GameSnapshot exposes its selected path; the control
			// identity also must remain the same while that reservation is used.
			CHECK(rooms[1]->Game(rooms[0]->LocalIdentity()).route == finalProbeRoute);
			CHECK(rooms[0]->ConnectionForIdentity(probePeer) == probeControl);
		}
		std::cout << "Generation " << cycle << " authorized; creating GGPO sessions\n";
		std::array<GGPOPlayerHandle, Count> localHandles = {};
		auto callbacks = GameCallbacks();
		for (std::size_t i = 0; i < Count; ++i) {
			const auto slot = matches[i]->LocalSlot();
			const auto& roster = matches[i]->Roster();
			matches[i]->ReleasePortToGgpo();
			if (slot >= 2) {
				CHECK(matches[i]->RemotePort(roster[0]) != 0);
				CHECK(ggpo_start_spectating(&ggpo[i], &callbacks, "transport-test", 2, static_cast<int>(session::GgpoInputBytes), clients[i]->_ggpoPort,
					"127.0.0.1", matches[i]->RemotePort(roster[0])) == GGPO_OK);
			} else {
				CHECK(ggpo_start_session(&ggpo[i], &callbacks, "transport-test", 2, static_cast<int>(session::GgpoInputBytes), clients[i]->_ggpoPort) == GGPO_OK);
				activeSession = ggpo[i];
				const auto members = slot == 0 ? Count : 2;
				for (std::size_t p = 0; p < members; ++p) {
					GGPOPlayer player = {}; player.size = sizeof(player); player.player_num = static_cast<int>(p) + 1;
					player.type = p == slot ? GGPO_PLAYERTYPE_LOCAL : p < 2 ? GGPO_PLAYERTYPE_REMOTE : GGPO_PLAYERTYPE_SPECTATOR;
					if (p != slot) {
						CHECK(matches[i]->RemotePort(roster[p]) != 0);
						strcpy_s(player.u.remote.ip_address, "127.0.0.1"); player.u.remote.port = matches[i]->RemotePort(roster[p]);
					}
					GGPOPlayerHandle handle; CHECK(ggpo_add_player(ggpo[i], &player, &handle) == GGPO_OK);
					if (p == slot) { localHandles[i] = handle; CHECK(ggpo_set_frame_delay(ggpo[i], handle, readyTable.inputDelay[slot]) == GGPO_OK); }
				}
			}
		}
		std::array<int, Count> frames = {};
		std::array<bool, Count> inputAdded = {};
		wait([&]() {
			pump();
			for (std::size_t i = 0; i < Count; ++i) {
				activeSession = ggpo[i]; CHECK(ggpo_idle(ggpo[i], 0) == GGPO_OK);
				const bool player = matches[i]->LocalSlot() < 2;
				if (frames[i] >= (player ? 80 : 60)) continue;
				if (player && !inputAdded[i]) {
					std::array<unsigned char, session::GgpoInputBytes> input;
					for (std::size_t byte = 0; byte < input.size(); ++byte) input[byte] = static_cast<unsigned char>((frames[i] * 73 + byte * 31 + i) & 255);
					if (ggpo_add_local_input(ggpo[i], localHandles[i], input.data(), static_cast<int>(input.size())) != GGPO_OK) continue;
					inputAdded[i] = true;
				}
				unsigned char inputs[2 * session::GgpoInputBytes] = {}; int disconnected = 0;
				if (ggpo_synchronize_input(ggpo[i], inputs, sizeof(inputs), &disconnected) != GGPO_OK) continue;
				if (!player) {
					for (std::size_t side = 0; side < 2; ++side) {
						std::size_t sender = 0;
						while (sender < Count && matches[sender]->LocalSlot() != side) ++sender;
						CHECK(sender < Count);
						for (std::size_t byte = 0; byte < session::GgpoInputBytes; ++byte) {
							const auto delay=readyTable.inputDelay[side];
							const auto expected = frames[i]<delay ? 0 : static_cast<unsigned char>(((frames[i] - delay) * 73 + byte * 31 + sender) & 255);
							CHECK(inputs[side * session::GgpoInputBytes + byte] == expected);
						}
					}
				}
				CHECK(disconnected == 0); CHECK(ggpo_advance_frame(ggpo[i]) == GGPO_OK);
				++frames[i]; inputAdded[i] = false;
			}
			Sleep(14); // Together with the outer pump, approximate a game tick.
			return std::all_of(frames.begin(), frames.end(), [](int frame) { return frame >= 60; });
		});
		std::cout << "Generation " << cycle << " input streams completed; retiring GGPO\n";
		wait([&]() {
			pump();
			if (server.HasRecoveryCandidate() || server.PendingProposal()) return false;
			for (std::size_t i = 0; i < Count; ++i)
				if (recoveryPeers[i].recovery.AppliedRevision() != rooms[0]->Coordination().revision) return false;
			return true;
		});
		// Mirror the game's match-ended notification before retiring its socket.
		// Control and QUIC close events may otherwise arrive in either order.
		for (auto& match : matches) match->End();
		if (cycle == 1) {
			// The real GGPO sessions still own their UDP sockets. Advancing the
			// injected match clock past both spectator-drain boundaries must
			// leave every existing helper mapping and generation untouched.
			for (const ULONGLONG advance : {31000ULL, 90000ULL}) {
				matchClockOffset += advance;
				for (std::size_t i = 0; i < Count; ++i) {
					CHECK(ggpo[i] != nullptr && matches[i]->Tick(true));
					CHECK(matches[i]->GetPhase() == Phase::Ending && matches[i]->Generation() == cycle);
					for (const auto& edge : authorizedEdges[i]) {
						const auto game = rooms[i]->Game(edge.first);
						CHECK(game.generation == cycle && game.state == session::IrohRoom::GameState::Ready && game.virtualPort == edge.second);
					}
				}
			}
			std::cout << "Native socket ownership retained all player/spectator mappings after 31 and 121 simulated seconds\n";
		}
		for (auto& session : ggpo) { activeSession = session; CHECK(ggpo_close_session(session) == GGPO_OK); session = nullptr; }
		for (std::size_t i = 0; i < Count; ++i) if (matches[i]->LocalSlot() < 2) CHECK(clients[i]->Lobby_ReportResults(0) == session::SendResult::Queued);
		if (terminalRecovery && cycle == 1) {
			// Neither held recipient may observe the original terminal effect.
			// Keep every match coordinator from closing its mappings until the
			// control-only fault is over, so lifecycle receipts are never lost.
			held[1] = held[2] = true;
			holdMatchTicks = true;
			const auto heldRevision = recoveryPeers[1].recovery.AppliedRevision();
			std::uint64_t terminalTerm = 0, terminalSequence = 0;
			std::array<std::set<std::string>, Count> originalTerminalTokens;
			wait([&]() {
				pump();
				if (server.HasRecoveryCandidate() || server.PendingProposal()) return false;
				const auto current = server.RecoveryCheckpoint();
				for (const auto& receipt : current.at("room").at("terminal_receipts"))
					if (receipt.at("table") == 0 && receipt.at("generation") == cycle) return true;
				return false;
			});
			{
				const auto initial = server.RecoveryCheckpoint();
				unsigned ends = 0, outcomes = 0;
				terminalTerm = rooms[0]->Coordination().term;
				for (const auto& effect : initial.at("effect_journal").get<std::vector<session::EffectEnvelope>>()) {
					if (effect.term == terminalTerm) terminalSequence = (std::max)(terminalSequence, effect.sequence);
					const bool end = effect.type == "game_end" && effect.generation == cycle;
					const bool outcome = effect.type == "room_event" && effect.publicPayload.is_object() &&
						effect.publicPayload.at("event").at("kind") == static_cast<int>(room::Event::Kind::MatchEnded) &&
						effect.publicPayload.at("event").at("match_generation") == cycle;
					if (end) ++ends;
					if (outcome) ++outcomes;
					if (end || outcome) for (const auto i : {1, 2})
						if (effect.recipient == clients[i]->GetRoomSnapshot().localMember)
							originalTerminalTokens[i].insert(DiscardedControlStream::TokenKey(effect));
				}
				CHECK(terminalSequence && ends >= Count && outcomes >= Count);
				CHECK(originalTerminalTokens[1].size() >= 2 && originalTerminalTokens[2].size() >= 2);
			}
			// Each rule edit produces actual committed snapshot/reply effects.
			// Continue until the original terminal envelopes are outside the
			// bounded journal; the dedicated unacknowledged receipt must survive.
			std::uint64_t edits = 0;
			for (; edits < 100; ++edits) {
				auto action = roomAction(*clients[0], room::ActionKind::SetRules);
				action.table = 1;
				action.rules = clients[0]->GetRoomSnapshot().tables[1].rules;
				action.rules.roundTime = edits % 2 ? 60 : 99;
				waitRoomAction(*clients[0], action);
				if ((edits + 1) % 20 == 0) std::cout << "Terminal fault committed " << edits + 1 << "/100 unrelated edits\n";
			}
			wait([&]() {
				pump();
				for (const auto i : {1, 2}) for (const auto& token : originalTerminalTokens[i])
					if (!discarded[i].terminalTokens.count(token)) return false;
				return !server.HasRecoveryCandidate() && !server.PendingProposal() &&
					discarded[1].revision >= rooms[0]->Coordination().revision &&
					discarded[2].revision >= rooms[0]->Coordination().revision &&
					!discarded[1].receiver.Active() && !discarded[2].receiver.Active();
			});
			const auto checkpoint = server.RecoveryCheckpoint();
			const auto compactedRevision = rooms[0]->Coordination().revision;
			CHECK(checkpoint.at("effect_journal").size() <= session::MaxEffectJournalEntries);
			for (const auto& effect : checkpoint.at("effect_journal").get<std::vector<session::EffectEnvelope>>()) {
				// All effects present at terminal commitment must be gone,
				// including nested room_event/room_result and projection payloads
				// whose envelope generation field may legitimately be zero.
				CHECK(effect.term != terminalTerm || effect.sequence > terminalSequence);
			}
			bool retained = false;
			for (const auto& receipt : checkpoint.at("room").at("terminal_receipts"))
				if (receipt.at("table") == 0 && receipt.at("generation") == cycle) {
					CHECK(!receipt.at("acknowledged").get<bool>());
					CHECK(receipt.at("result") == static_cast<int>(room::MatchResult::P2Win));
					CHECK(receipt.at("recipients").size() == Count);
					retained = true;
				}
			CHECK(retained && discarded[1].checkpoints > 1 && discarded[2].checkpoints > 1);
			CHECK(recoveryPeers[1].recovery.AppliedRevision() == heldRevision);
			for (const auto i : {1, 2}) {
				room::Event event;
				while (clients[i]->TakeRoomEvent(event))
					CHECK(event.kind != room::Event::Kind::MatchEnded || event.matchGeneration != cycle);
				CHECK(matches[i]->GetPhase() == Phase::Ending && matches[i]->Generation() == cycle);
			}
			// Re-enable the real adapters and force one fresh complete checkpoint.
			// The old control deliveries have been consumed by the fault sink.
			held[1] = held[2] = false;
			holdMatchTicks = false;
			auto finalAction = roomAction(*clients[0], room::ActionKind::SetRules);
			finalAction.table = 1;
			finalAction.rules = clients[0]->GetRoomSnapshot().tables[1].rules;
			finalAction.rules.roundTime = 99;
			finalAction.revision = clients[0]->GetRoomSnapshot().revision;
			finalAction.tableRevision = clients[0]->GetRoomSnapshot().tables[1].revision;
			CHECK(clients[0]->SendRoomAction(finalAction) == session::SendResult::Queued);
			wait([&]() {
				pump();
				for (const auto i : {1, 2})
					if (recoveryPeers[i].recovery.AppliedRevision() <= compactedRevision) return false;
				return true;
			});
			CHECK(!recoveryPeers[1].server->PendingProposal() && !recoveryPeers[2].server->PendingProposal());
			CHECK(!recoveryPeers[1].server->HasRecoveryCandidate() && !recoveryPeers[2].server->HasRecoveryCandidate());
			std::cout << "Terminal control fault: discarded " << discarded[1].checkpoints << '/' << discarded[2].checkpoints
				<< " validated checkpoints after " << edits << " unrelated committed rule changes\n";
		}
		wait([&]() { pump(); return std::all_of(matches.begin(), matches.end(), [](const std::unique_ptr<session::IrohMatchSession>& match) { return match->GetPhase() == Phase::Idle; }); });
		for (std::size_t i = 0; i < Count; ++i) {
			CHECK(ggpo[i] == nullptr && matches[i]->Generation() == cycle);
			for (const auto& edge : authorizedEdges[i]) {
				const auto game = rooms[i]->Game(edge.first);
				CHECK(game.generation == cycle && game.state == session::IrohRoom::GameState::Closed);
			}
		}
		std::array<bool, Count> observedTerminals{}, queuedTerminalAcks{};
		const auto ackWaitStarted = GetTickCount64();
		bool ackDiagnostic = false;
		wait([&]() {
			pump();
			for (std::size_t i = 0; i < Count; ++i) {
				room::Event event;
				while (clients[i]->TakeRoomEvent(event)) {
					if (event.kind != room::Event::Kind::MatchEnded || event.matchGeneration != cycle) continue;
					CHECK(event.table == 0 && event.result == room::MatchResult::P2Win);
					observedTerminals[i] = true;
				}
					test::AcknowledgeIrohFixtureTerminal(*clients[i], 0, cycle,
					ggpo[i] == nullptr && matches[i]->GetPhase() == Phase::Idle, observedTerminals[i], queuedTerminalAcks[i]);
			}
			if (!ackDiagnostic && GetTickCount64() - ackWaitStarted > 5000) {
				ackDiagnostic = true;
				std::cerr << "Terminal ACK wait: candidate=" << server.HasRecoveryCandidate()
					<< " proposal=" << (server.PendingProposal() != nullptr) << '\n';
				for (std::size_t i = 0; i < Count; ++i)
					std::cerr << "recipient " << i << " observed=" << observedTerminals[i] << " queued=" << queuedTerminalAcks[i] << '\n';
				const auto current = server.RecoveryCheckpoint();
				for (const auto& receipt : current.at("room").at("terminal_receipts"))
					if (receipt.at("table") == 0 && receipt.at("generation") == cycle) {
						std::cerr << "receipt acknowledged=" << receipt.at("acknowledged") << '\n';
						for (const auto& recipient : receipt.at("recipients"))
							std::cerr << "member " << recipient.at("member") << " acknowledged=" << recipient.at("acknowledged") << '\n';
					}
			}
			return std::all_of(queuedTerminalAcks.begin(), queuedTerminalAcks.end(), [](bool value) { return value; }) &&
				test::IrohFixtureTerminalsCommitted(server, {{0, cycle}});
		});
		std::cout << "Authorized generation " << cycle << ": GGPO players and two spectator streams passed\n";
	}
	for (auto& match : matches) match.reset();
	for (auto& client : clients) client->Disconnect();
	server.Close();
	for (auto& helper : helpers) CHECK(helper.Send("{\"type\":\"shutdown\"}"));
	wait([&]() { return std::all_of(processes.begin(), processes.end(), [](const platform::HelperProcess& process) { return !process.IsRunning(); }); });
	std::cout << "Four participants, three fresh authorized GGPO matches, same-pair rematches and spectator mapping passed. No SF4 simulation tested.\n";
}
