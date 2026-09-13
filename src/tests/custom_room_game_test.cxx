#include "../session/IrohMatchSession.hxx"
#include "../session/sf4e__SessionServer.hxx"
#include "../netplay/MatchResultOutbox.hxx"
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

int wmain(int argc, wchar_t** argv) {
	std::cout << std::unitbuf;
	CHECK(argc >= 2 && argc <= 4);
	bool relay = false, singleTable = false;
	for (int i = 2; i < argc; ++i) {
		if (std::wstring(argv[i]) == L"--relay-only") relay = true;
		else if (std::wstring(argv[i]) == L"--single-table") singleTable = true;
		else CHECK(false);
	}
	constexpr std::size_t Count = room::MaxMembers;
	std::array<platform::HelperProcess, Count> processes;
	std::array<platform::HelperClient, Count> helpers;
	std::array<std::shared_ptr<session::IrohRoom>, Count> rooms;
	std::array<test::IrohServerPeer, Count> recoveryPeers;
	std::array<std::unique_ptr<SessionClient>, Count> clients;
	std::array<std::unique_ptr<session::IrohMatchSession>, Count> matches;
	std::array<GGPOSession*, Count> ggpo = {};
	std::array<std::string, Count> names;
	for (std::size_t i = 0; i < Count; ++i) {
		names[i] = "Member " + std::to_string(i + 1);
		CHECK(processes[i].Start(argv[1], GetCurrentProcessId(), relay));
		CHECK(helpers[i].Start(processes[i].Bootstrap()));
		rooms[i] = std::make_shared<session::IrohRoom>(helpers[i]);
		SessionClient::Callbacks callbacks = {};
		callbacks.OnError = [](SessionClient::ErrorType, SessionClient*, const SessionClient::Callbacks&) { CHECK(false); };
		clients[i].reset(new SessionClient(callbacks, "authorized-match-test", 0, names[i]));
	}
	std::function<void()> serviceJoins;
	std::function<void()> failureDump;
	std::string phase="helper startup";
	auto wait = [&](const std::function<bool()>& progress) {
		const auto deadline = GetTickCount64() + 45000;
		do { for (auto& room : rooms) room->Poll(); if(serviceJoins) serviceJoins(); if (progress()) return; Sleep(2); } while (GetTickCount64() < deadline);
		std::cerr << "Large room timeout during " << phase << '\n';
		if (failureDump) failureDump();
		for (std::size_t i = 0; i < Count; ++i) {
			const auto& authority=rooms[i]->Coordination();
			const auto& view=clients[i]->GetRoomSnapshot();
			const auto local=std::find_if(view.members.begin(),view.members.end(),[&](const room::Member& member) {
				return member.id==view.localMember;
			});
			const int selectedTable=local==view.members.end()? -1 : local->table;
			const auto selectedGeneration=selectedTable>=0 && selectedTable<room::TableCount
				? view.tables[static_cast<std::size_t>(selectedTable)].matchGeneration : 0;
			const auto recoveryState=rooms[i]->RecoveryState();
			std::cerr << "Member " << i << ": room=" << static_cast<int>(rooms[i]->GetState())
				<< " ready_for_match=" << rooms[i]->ReadyForMatch()
				<< " members=" << view.members.size() << " view_revision=" << view.revision
				<< " selected_table=" << selectedTable << " view_generation=" << selectedGeneration
				<< " room_error=" << rooms[i]->Error() << " client_error=" << clients[i]->RoomError()
				<< " term=" << authority.term << " revision=" << authority.revision
				<< " applied=" << recoveryPeers[i].recovery.AppliedRevision()
				<< " recovery_error=" << recoveryPeers[i].recovery.Error()
				<< " writable=" << authority.writable << " rebound=" << authority.rebound
				<< " checkpoint=" << recoveryState.checkpointActive << '/' << recoveryState.checkpointComplete
				<< '/' << recoveryState.checkpointTransfer << '/' << recoveryState.checkpointTerm
				<< '/' << recoveryState.checkpointRevision << '/' << recoveryState.checkpointOffset
				<< '/' << recoveryState.checkpointLength << " staged=" << recoveryState.stagedCheckpoints
				<< " checkpoint_pending=" << recoveryState.pendingCheckpointAck
				<< '/' << recoveryState.pendingCommittedMarker << " checkpoint_counts="
				<< recoveryState.checkpointRestarts << '/' << recoveryState.checkpointTimeouts
				<< '/' << recoveryState.checkpointTransferErrors << " proposal="
				<< recoveryState.proposalTransfer << '/' << recoveryState.proposalTerm
				<< '/' << recoveryState.proposalBaseRevision << '/' << recoveryState.proposalElapsedMs
				<< '/' << recoveryState.proposalBytes
				<< '/' << recoveryState.proposalSent << '/' << recoveryState.proposalAcknowledged
				<< '/' << recoveryState.proposalBegun << '/' << recoveryState.proposalEnded
				<< '/' << recoveryState.proposalTimeouts << '/' << recoveryState.proposalTransferErrors
				<< '/' << recoveryState.proposalStatus << " queues="
				<< recoveryState.serverQueueMessages << '/' << recoveryState.clientQueueMessages
				<< '/' << recoveryState.queuedBytes << " replay_queues="
				<< recoveryState.pendingPublicEffects << '/' << recoveryState.pendingTerminalReplays
				<< '/' << recoveryState.terminalReplaysInFlight;
			if (recoveryState.clientQueueMessages) std::cerr << " client_head=" << recoveryState.clientHeadType
				<< '/' << recoveryState.clientHeadCommitted << '/' << recoveryState.clientHeadTerm
				<< '/' << recoveryState.clientHeadRevision << '/' << recoveryState.clientHeadSequence
				<< '/' << recoveryState.clientHeadRecipient;
			std::cerr << " terminal_local=" << view.localTerminalPending << " terminal_generations=";
			for (const auto generation : view.localTerminalGenerations) std::cerr << generation << ',';
			if (matches[i]) std::cerr << " match=" << static_cast<int>(matches[i]->GetPhase())
				<< " generation=" << matches[i]->Generation() << " match_error=" << matches[i]->Error();
			std::cerr << '\n';
		}
		CHECK(false);
	};
	wait([&]() { return std::all_of(helpers.begin(), helpers.end(), [](const platform::HelperClient& helper) { return helper.State() == platform::HelperState::Connected; }); });
	CHECK(rooms[0]->Host("authorized-match-test"));
	wait([&]() { return rooms[0]->GetState() == session::IrohRoom::State::Ready; });
	for (std::size_t i = 0; i < Count; ++i) recoveryPeers[i].room = rooms[i];
	std::vector<test::IrohServerPeer*> joiningPeers;
	std::vector<SessionClient*> joiningClients;
	serviceJoins=[&]() {
		CHECK(test::PumpIrohRecoveryPeers(joiningPeers));
		CHECK(test::PumpIrohIntegrationClients(joiningClients));
	};
	for (std::size_t i = 0; i < Count; ++i) {
		phase="admission " + std::to_string(i+1);
		if (i) {
			CHECK(rooms[i]->Join(rooms[0]->Invitation(), "authorized-match-test"));
			wait([&]() { return rooms[i]->GetState() == session::IrohRoom::State::Ready; });
		}
		CHECK(test::ConfigureIrohIntegrationServer(recoveryPeers[i], "authorized-match-test", static_cast<std::uint8_t>(Count)));
		clients[i]->RequireCustomRooms();
		clients[i]->RequireMatchAuthorization();
		CHECK(clients[i]->Connect(rooms[i]->Client(), false) == 0);
		matches[i].reset(new session::IrohMatchSession(*clients[i], rooms[i]));
		joiningPeers.push_back(&recoveryPeers[i]); joiningClients.push_back(clients[i].get());
		wait([&]() {return std::all_of(joiningClients.begin(),joiningClients.end(),[&](const SessionClient* client) {
			return client->GetRoomSnapshot().members.size()==joiningClients.size();
		});});
		std::cout << "Admitted " << i+1 << " members through committed native state" << std::endl;
	}
	serviceJoins={};
	phase="table setup";
	auto& server = *recoveryPeers[0].server;
	std::vector<test::IrohServerPeer*> recovery;
	for (auto& peer : recoveryPeers) recovery.push_back(&peer);
	std::vector<SessionClient*> clientViews;
	for (auto& client : clients) clientViews.push_back(client.get());
	struct TableZeroTrace {
		int roomPhase = -1;
		std::uint64_t roomRevision = 0;
		std::uint64_t generation = 0;
		std::array<int, 4> clientPhases{{-1, -1, -1, -1}};
		std::array<std::uint64_t, 4> clientGenerations{};
	} tableZeroTrace;
	const auto elapsed = [&]() { return GetTickCount64(); };
	const auto dumpTables = [&](const char* reason) {
		const auto checkpoint = server.RecoveryCheckpoint();
		const auto& history = server.CommittedEffectHistory();
		std::cerr << "Table dump " << reason << ": journal_entries=" << history.size()
			<< " journal_bytes=" << session::EffectJournalBytes(history)
			<< " candidate=" << server.HasRecoveryCandidate()
			<< " candidate_overflow=" << server.RecoveryCandidateOverflowed();
		const auto pendingProposal = server.PendingProposal();
		if (pendingProposal) std::cerr << " pending_request=" << pendingProposal->request
			<< " pending_term=" << pendingProposal->term << " pending_base=" << pendingProposal->baseRevision
			<< " pending_effects=" << pendingProposal->effects.size()
			<< " pending_bytes=" << nlohmann::json(*pendingProposal).dump().size();
		else std::cerr << " pending_proposal=none";
		std::size_t receiptRecipients = 0, receiptAcknowledged = 0;
		const auto receipts = checkpoint.at("room").value("terminal_receipts", nlohmann::json::array());
		for (const auto& receipt : receipts) {
			for (const auto& recipient : receipt.value("recipients", nlohmann::json::array())) {
				++receiptRecipients;
				if (recipient.value("acknowledged", false)) ++receiptAcknowledged;
			}
		}
		std::cerr << " terminal_receipts=" << receipts.size() << " receipt_acked="
			<< receiptAcknowledged << '/' << receiptRecipients << '\n';
		for (const auto& receipt : receipts) {
			std::cerr << "Receipt table=" << receipt.value("table", 255U)
				<< " generation=" << receipt.value("generation", std::uint64_t(0))
				<< " result=" << receipt.value("result", -1) << " recipients=";
			for (const auto& recipient : receipt.value("recipients", nlohmann::json::array()))
				std::cerr << recipient.value("member", room::MemberId(0)) << '/'
					<< recipient.value("incarnation", std::uint64_t(0)) << '/'
					<< recipient.value("acknowledged", false) << ',';
			std::cerr << '\n';
		}
		for (std::size_t tableIndex = 0; tableIndex < room::TableCount; ++tableIndex) {
			const auto& table = server.RoomSnapshot()->tables[tableIndex];
			const auto authority = checkpoint.at("match_authorities").at(tableIndex);
			std::cerr << "Table" << tableIndex << ": room_phase=" << static_cast<int>(table.phase)
				<< " table_revision=" << table.revision << " generation=" << table.matchGeneration
				<< " result_pending=" << table.resultPending << " authority_phase="
				<< (authority.is_null() ? -1 : authority.at("phase").get<int>()) << " authority_generation="
				<< (authority.is_null() ? 0 : authority.at("generation").get<std::uint64_t>()) << '\n';
			for (const auto& effect : history) {
				if (effect.generation != table.matchGeneration) continue;
				if (effect.type != "data_update" && effect.type != "game_prepare" &&
					effect.type != "game_connect" && effect.type != "game_start" &&
					effect.type != "game_end" && effect.type != "room_event") continue;
				std::cerr << "Table" << tableIndex << " effect type=" << effect.type << " recipient=" << effect.recipient
					<< " term=" << effect.term << " revision=" << effect.revision << " sequence=" << effect.sequence
					<< " private=" << effect.privatePayload << " public_null=" << effect.publicPayload.is_null() << '\n';
			}
		}
	};
	failureDump = [&]() { dumpTables("timeout"); };
	const auto traceTableZero = [&]() {
		const auto& table = server.RoomSnapshot()->tables[0];
		bool changed = tableZeroTrace.roomPhase != static_cast<int>(table.phase) ||
			tableZeroTrace.roomRevision != table.revision || tableZeroTrace.generation != table.matchGeneration;
		for (std::size_t i = 0; i < 4; ++i) {
			const int matchPhase = static_cast<int>(matches[i]->GetPhase());
			const auto matchGeneration = matches[i]->Generation();
			changed = changed || tableZeroTrace.clientPhases[i] != matchPhase ||
				tableZeroTrace.clientGenerations[i] != matchGeneration;
			tableZeroTrace.clientPhases[i] = matchPhase;
			tableZeroTrace.clientGenerations[i] = matchGeneration;
		}
		if (!changed) return;
		tableZeroTrace.roomPhase = static_cast<int>(table.phase);
		tableZeroTrace.roomRevision = table.revision;
		tableZeroTrace.generation = table.matchGeneration;
		const auto checkpoint = server.RecoveryCheckpoint();
		const auto authority = checkpoint.at("match_authorities").at(0);
		const int authorityPhase = authority.is_null() ? -1 : authority.at("phase").get<int>();
		const auto authorityGeneration = authority.is_null() ? 0 : authority.at("generation").get<std::uint64_t>();
		std::cout << "Table0 transition tick=" << elapsed() << " room_phase=" << tableZeroTrace.roomPhase
			<< " table_revision=" << tableZeroTrace.roomRevision << " generation=" << tableZeroTrace.generation
			<< " authority_phase=" << authorityPhase << " authority_generation=" << authorityGeneration;
		for (std::size_t i = 0; i < 4; ++i)
			std::cout << " client" << i << "=" << tableZeroTrace.clientPhases[i] << "/" << tableZeroTrace.clientGenerations[i];
		std::cout << '\n';
	};
	auto pump = [&]() {
		CHECK(test::PumpIrohRecoveryPeers(recovery));
		if (!test::PumpIrohIntegrationClients(clientViews)) {
			for (std::size_t i = 0; i < Count; ++i) {
				const auto& coordination = rooms[i]->Coordination();
				std::cerr << "Failure state " << i << ": helper=" << static_cast<int>(helpers[i].State())
					<< " process=" << processes[i].IsRunning() << " room=" << static_cast<int>(rooms[i]->GetState())
					<< " room_error=" << rooms[i]->Error() << " term=" << coordination.term
					<< " revision=" << coordination.revision << " writable=" << coordination.writable
					<< " rebound=" << coordination.rebound;
				if (matches[i]) std::cerr << " match=" << static_cast<int>(matches[i]->GetPhase())
					<< " generation=" << matches[i]->Generation() << " match_error=" << matches[i]->Error();
				std::cerr << '\n';
			}
			CHECK(false);
		}
		for (std::size_t i = 0; i < Count; ++i) if (!matches[i]->Tick(ggpo[i] != nullptr)) {
			std::cerr << "match " << i << " " << matches[i]->Error() << '\n'; CHECK(false);
		}
		traceTableZero();
	};
	auto allViewsCurrent = [&]() {
		const auto revision = server.RoomSnapshot()->revision;
		return std::all_of(clients.begin(), clients.end(), [&](const std::unique_ptr<SessionClient>& client) {
			return client->GetRoomSnapshot().revision == revision;
		});
	};
	wait([&]() { pump(); return std::all_of(clients.begin(), clients.end(), [&](const std::unique_ptr<SessionClient>& client) { return client->GetRoomSnapshot().members.size() == Count; }); });
	for (std::size_t i = 0; i < Count; ++i) {
		const auto table = static_cast<std::uint8_t>(singleTable ? 0 : i / 4);
		bool queueSent = false;
		wait([&]() {
			pump();
			if (!queueSent) {
				const auto& current = clients[i]->GetRoomSnapshot();
				room::Action queue; queue.kind = room::ActionKind::Queue; queue.table = table;
				queue.roomEpoch = current.roomEpoch; queue.revision = current.revision;
				queue.tableRevision = current.tables[table].revision;
				const auto sent = clients[i]->SendRoomAction(queue);
				std::cout << "Queue index=" << i << " table=" << static_cast<unsigned>(table)
					<< " send=" << static_cast<int>(sent) << " tick=" << elapsed() << '\n';
				CHECK(sent == session::SendResult::Queued || sent == session::SendResult::NotConnected ||
					sent == session::SendResult::QueueFull);
				queueSent = sent == session::SendResult::Queued;
			}
			const auto& view = clients[i]->GetRoomSnapshot();
			return queueSent && allViewsCurrent() && std::any_of(view.members.begin(), view.members.end(), [&](const room::Member& member) {
				return member.id == view.localMember && member.table == table;
			});
		});
	}
	using Phase = session::IrohMatchSession::Phase;
	const auto initialTables = server.RoomSnapshot()->tables;
	constexpr std::uint64_t RematchCycles = 6;
	for (std::uint64_t cycle = 1; cycle <= RematchCycles; ++cycle) {
		phase="match " + std::to_string(cycle);
		for (std::size_t readyIndex = 0; readyIndex < clients.size(); ++readyIndex) if (clients[readyIndex]->IsLocalPlayer()) {
			auto& client = clients[readyIndex];
			const auto previousRevision = server.RoomSnapshot()->revision;
			std::uint64_t readyActionId = 0;
			std::uint8_t readyTable = 0;
			bool readySent = false;
			bool readyAccepted = false;
			wait([&]() {
				pump();
				if (!readySent) {
					const auto& view = client->GetRoomSnapshot();
					const auto member = std::find_if(view.members.begin(), view.members.end(), [&](const room::Member& item) {
						return item.id == view.localMember;
					});
					CHECK(member != view.members.end() && member->table >= 0 && member->seat >= 0);
					room::Action ready;
					ready.kind = room::ActionKind::Ready;
					ready.inputDelay = 2;
					ready.roomEpoch = view.roomEpoch;
					ready.revision = view.revision;
					ready.table = static_cast<std::uint8_t>(member->table);
					ready.tableRevision = view.tables[ready.table].revision;
					const auto sent = client->SendRoomAction(ready, &readyActionId);
					std::cout << "Ready index=" << readyIndex << " table=" << static_cast<unsigned>(ready.table)
						<< " send=" << static_cast<int>(sent) << " action=" << readyActionId
						<< " tick=" << elapsed() << '\n';
					CHECK(sent == session::SendResult::Queued || sent == session::SendResult::NotConnected ||
						sent == session::SendResult::QueueFull);
					readySent = sent == session::SendResult::Queued;
					readyTable = ready.table;
				}
				SessionClient::ActionReply reply;
				while (client->TakeActionReply(reply)) if (reply.actionId == readyActionId) {
					std::cout << "Ready reply index=" << readyIndex << " action=" << reply.actionId
						<< " accepted=" << reply.accepted << " reason=" << static_cast<int>(reply.reason)
						<< " tick=" << elapsed() << '\n';
					CHECK(reply.accepted);
					readyAccepted = reply.accepted;
				}
				return readySent && readyAccepted && server.RoomSnapshot()->revision > previousRevision && allViewsCurrent();
			});
			CHECK(client->GetRoomSnapshot().tables[readyTable].revision >= 1);
		}
		wait([&]() { pump(); return std::all_of(matches.begin(), matches.end(), [](const std::unique_ptr<session::IrohMatchSession>& match) { return match->GetPhase() == Phase::Started; }); });
		for (const auto& match : matches) CHECK(match->Generation() != 0);
		for(std::size_t i=0;i<Count;++i) if(matches[i]->LocalSlot()==1) {
			const auto table=singleTable?0:i/4;
			const auto hostIndex=singleTable?0:table*4;
			const auto hostRoute=rooms[hostIndex]->Game(rooms[i]->LocalIdentity()).route;
			const auto guestRoute=rooms[i]->Game(rooms[hostIndex]->LocalIdentity()).route;
			CHECK(!hostRoute.empty() && !guestRoute.empty());
			if(relay) CHECK(hostRoute.rfind("relay:",0)==0 && guestRoute.rfind("relay:",0)==0);
			std::cout << "Cycle " << cycle << " table " << table << " generation=" << matches[i]->Generation()
				<< " selected routes=" << hostRoute << "," << guestRoute << '\n';
		}
		if (!singleTable) for (std::size_t table = 1; table < room::TableCount; ++table)
			CHECK(matches[table * 4]->Generation() != matches[0]->Generation());
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
				const auto members = slot == 0 ? roster.size() : 2;
				for (std::size_t p = 0; p < members; ++p) {
					GGPOPlayer player = {}; player.size = sizeof(player); player.player_num = static_cast<int>(p) + 1;
					player.type = p == slot ? GGPO_PLAYERTYPE_LOCAL : p < 2 ? GGPO_PLAYERTYPE_REMOTE : GGPO_PLAYERTYPE_SPECTATOR;
					if (p != slot) {
						CHECK(matches[i]->RemotePort(roster[p]) != 0);
						strcpy_s(player.u.remote.ip_address, "127.0.0.1"); player.u.remote.port = matches[i]->RemotePort(roster[p]);
					}
					GGPOPlayerHandle handle; CHECK(ggpo_add_player(ggpo[i], &player, &handle) == GGPO_OK);
					if (p == slot) { localHandles[i] = handle; CHECK(ggpo_set_frame_delay(ggpo[i], handle, 2) == GGPO_OK); }
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
				if (!player && frames[i] >= 2) {
					for (std::size_t side = 0; side < 2; ++side) {
						std::size_t sender = 0;
						while (sender < Count && !(clients[sender]->_cid == matches[i]->Roster()[side])) ++sender;
						CHECK(sender < Count);
						for (std::size_t byte = 0; byte < session::GgpoInputBytes; ++byte) {
							const auto expected = static_cast<unsigned char>(((frames[i] - 2) * 73 + byte * 31 + sender) & 255);
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
		std::array<std::uint64_t, Count> retiredGenerations{};
		for (std::size_t i = 0; i < Count; ++i) retiredGenerations[i] = matches[i]->Generation();
		// Mirror the game's match-ended notification before retiring its socket.
		// Control and QUIC close events may otherwise arrive in either order.
		for (auto& match : matches) match->End();
		for (auto& session : ggpo) { activeSession = session; CHECK(ggpo_close_session(session) == GGPO_OK); session = nullptr; }
		// Capture the native outcome identity before pumping teardown. Control may
		// be transiently unavailable and LocalSlot may retire while this immutable
		// report remains pending.
		std::array<room::Action, Count> retainedResults{};
		std::array<bool, Count> resultParticipant{}, resultConfirmed{};
		std::array<std::uint64_t, Count> resultRetryAt{};
		std::array<std::uint64_t, Count> resultActionIds{};
		for (std::size_t i = 0; i < Count; ++i) if (matches[i]->LocalSlot() < 2) {
			const auto& view = clients[i]->GetRoomSnapshot();
			const auto member = std::find_if(view.members.begin(), view.members.end(), [&](const room::Member& item) { return item.id == view.localMember; });
			CHECK(member != view.members.end() && member->table >= 0);
			auto& result = retainedResults[i]; result.kind = room::ActionKind::RecordResult;
			result.roomEpoch = view.roomEpoch; result.revision = view.revision;
			result.table = static_cast<std::uint8_t>(member->table);
			result.tableRevision = view.tables[result.table].revision;
			result.matchGeneration = matches[i]->Generation(); result.result = room::MatchResult::P1Win;
			resultParticipant[i] = true;
		}
		phase = "result submission " + std::to_string(cycle);
		wait([&]() {
			pump();
			const auto now = GetTickCount64();
			for (std::size_t i = 0; i < Count; ++i) if (resultParticipant[i] && !resultConfirmed[i]) {
				SessionClient::ActionReply reply;
				while (clients[i]->TakeActionReply(reply)) {
					if (!resultActionIds[i] || resultActionIds[i] != reply.actionId) continue;
					const bool confirmed = reply.accepted || reply.reason == room::RejectReason::DuplicateResult;
					std::cout << "Result reply index=" << i << " action=" << reply.actionId
						<< " accepted=" << reply.accepted << " reason=" << static_cast<int>(reply.reason)
						<< " generation=" << retainedResults[i].matchGeneration << " tick=" << now << '\n';
					CHECK(confirmed);
					resultConfirmed[i] = true;
				}
				if (resultConfirmed[i] || now < resultRetryAt[i]) continue;
				auto actionId = resultActionIds[i];
				const auto sent = actionId ? clients[i]->RetryRoomResult(retainedResults[i], actionId) :
					clients[i]->SendRoomAction(retainedResults[i], &actionId);
				std::cout << "Result index=" << i << " table=" << static_cast<unsigned>(retainedResults[i].table)
					<< " generation=" << retainedResults[i].matchGeneration << " send=" << static_cast<int>(sent)
					<< " action=" << actionId << " tick=" << now << '\n';
				CHECK(sent == session::SendResult::Queued || sent == session::SendResult::NotConnected ||
					sent == session::SendResult::QueueFull);
				if (sent == session::SendResult::Queued) {
					CHECK(actionId != 0);
					if (!resultActionIds[i]) resultActionIds[i] = actionId;
					CHECK(resultActionIds[i] == actionId);
					resultRetryAt[i] = now + netplay::MatchResultOutbox::RetryDelayMs;
				} else resultRetryAt[i] = now + netplay::MatchResultOutbox::UnsentRetryDelayMs;
			}
			return std::equal(resultParticipant.begin(), resultParticipant.end(), resultConfirmed.begin());
		});
		wait([&]() { pump(); return allViewsCurrent() && std::all_of(matches.begin(), matches.end(), [](const std::unique_ptr<session::IrohMatchSession>& match) { return match->GetPhase() == Phase::Idle; }); });
		std::array<bool, Count> observedTerminals{}, queuedTerminalAcks{};
		std::vector<std::pair<std::uint8_t, std::uint64_t>> terminalKeys;
		for (std::size_t table = 0; table < (singleTable ? 1 : room::TableCount); ++table)
			terminalKeys.emplace_back(static_cast<std::uint8_t>(table), retiredGenerations[singleTable ? 0 : table * 4]);
		phase = "terminal receipt acknowledgement " + std::to_string(cycle);
		wait([&]() {
			pump();
			for (std::size_t i = 0; i < Count; ++i)
				test::AcknowledgeIrohFixtureTerminal(*clients[i], static_cast<std::uint8_t>(singleTable ? 0 : i / 4),
					retiredGenerations[i], ggpo[i] == nullptr && matches[i]->GetPhase() == Phase::Idle,
					observedTerminals[i], queuedTerminalAcks[i]);
			return std::all_of(queuedTerminalAcks.begin(), queuedTerminalAcks.end(), [](bool value) { return value; }) &&
				test::IrohFixtureTerminalsCommitted(server, terminalKeys) && allViewsCurrent();
		});
		for (std::size_t table = 0; table < (singleTable ? 1 : room::TableCount); ++table) {
			const auto& current = server.RoomSnapshot()->tables[table];
			CHECK(current.p1 == initialTables[table].p1 && current.p2 == initialTables[table].p2);
			CHECK(current.queue == initialTables[table].queue);
			CHECK(current.score[0] == cycle && current.score[1] == 0);
		}
		std::cout << "Custom room cycle " << cycle << ": independently authorized input streams passed\n";
	}
	for (auto& match : matches) match.reset();
	for (auto& client : clients) client->Disconnect();
	server.Close();
	for (auto& helper : helpers) CHECK(helper.Send("{\"type\":\"shutdown\"}"));
	wait([&]() { return std::all_of(processes.begin(), processes.end(), [](const platform::HelperProcess& process) { return !process.IsRunning(); }); });
	std::cout << "Sixteen members; " << (singleTable ? "two fighters and fourteen spectators" : "four concurrent matches")
		<< "; six games without automatic rotation and spectator input verification passed. No SF4 simulation tested.\n";
}
