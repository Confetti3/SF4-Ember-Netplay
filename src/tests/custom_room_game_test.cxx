#include "../session/IrohMatchSession.hxx"
#include "../session/sf4e__SessionServer.hxx"
#include "../netplay/MatchResultOutbox.hxx"
#include "iroh_integration_fixture.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include <ggponet.h>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>

#include "test_support.hxx"
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
	CHECK(argc >= 2);
	// Layout: members fill tables in order, perTable at each; the first two at
	// a table fight and the rest spectate. --perf measures each member's tick
	// as its own slice, with a busy spin standing in for SF4's frame cost.
	// --stall-spectator: the last member, a spectator, does not tick its match
	// session during the first game's setup. The fighters must start without it
	// after P1's grace, and it must retire that generation cleanly and join the
	// next one.
	// --late-spectator: the last member, a spectator, withholds its terminal
	// acknowledgement of game one while the fighters rematch. Game two must start
	// without it, and once it acknowledges it must be back in game three.
	// --late-join: the last member is admitted while game one is being played
	// (F-017). No room may fail and no match may end early; after the game it
	// queues for its table and spectates game two.
	bool relay = false, perf = false, stallSpectator = false, lateSpectator = false, lateJoin = false;
	std::size_t count = room::MaxMembers, perTable = 4;
	int perfFrames = 600, gameCostUs = 1000;
	std::uint64_t rematchCycles = 6;
	std::wstring jsonPath;
	for (int i = 2; i < argc; ++i) {
		const std::wstring arg = argv[i];
		const auto number = [&]() { CHECK(i + 1 < argc); return std::wcstoul(argv[++i], nullptr, 10); };
		if (arg == L"--relay-only") relay = true;
		else if (arg == L"--single-table") perTable = 0;
		else if (arg == L"--perf") perf = true;
		else if (arg == L"--stall-spectator") stallSpectator = true;
		else if (arg == L"--late-spectator") lateSpectator = true;
		else if (arg == L"--late-join") lateJoin = true;
		else if (arg == L"--members") count = number();
		else if (arg == L"--per-table") perTable = number();
		else if (arg == L"--frames") perfFrames = static_cast<int>(number());
		else if (arg == L"--game-cost-us") gameCostUs = static_cast<int>(number());
		else if (arg == L"--cycles") rematchCycles = number();
		else if (arg == L"--json") { CHECK(i + 1 < argc); jsonPath = argv[++i]; }
		else CHECK(false);
	}
	if (!perTable) perTable = count;
	CHECK(count >= 2 && count <= room::MaxMembers && perTable >= 2 && count % perTable == 0);
	CHECK(perTable <= room::MaxMatchParticipants && count / perTable <= room::TableCount && rematchCycles >= 1);
	const bool singleTable = count == perTable;
	const std::size_t tableCount = count / perTable;
	const int playerFrames = perf ? perfFrames + 20 : 80, watchFrames = perf ? perfFrames : 60;
	if (perf) diag::SetEnabled(true);
	const std::size_t Count = count;
	const std::size_t stalled = stallSpectator ? Count - 1 : Count;
	CHECK(!stallSpectator || (Count - 1) % perTable >= 2);
	const std::size_t late = lateSpectator ? Count - 1 : Count;
	CHECK(!lateSpectator || (!stallSpectator && (Count - 1) % perTable >= 2 && rematchCycles >= 3));
	std::uint64_t lateGeneration = 0;
	// The late joiner is admitted inside game one; until then it has no server,
	// client connection or match session and nothing may tick it.
	CHECK(!lateJoin || (!stallSpectator && !lateSpectator && !perf && (Count - 1) % perTable >= 2 && rematchCycles >= 2));
	const std::size_t joiner = lateJoin ? Count - 1 : Count;
	std::size_t admitted = lateJoin ? Count - 1 : Count;
	bool stallActive = false;
	const auto live = [&](std::size_t i) { return !(stallActive && i == stalled) && i < admitted; };
	std::vector<platform::HelperProcess> processes(Count);
	std::vector<platform::HelperClient> helpers(Count);
	std::vector<std::shared_ptr<session::IrohRoom>> rooms(Count);
	std::vector<test::IrohServerPeer> recoveryPeers(Count);
	std::vector<std::unique_ptr<SessionClient>> clients(Count);
	std::vector<std::unique_ptr<session::IrohMatchSession>> matches(Count);
	std::vector<GGPOSession*> ggpo(Count, nullptr);
	std::vector<std::string> names(Count);
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
	// Once set, each member's tick is a timed slice that polls its own room
	// (RoomRecoveryRuntime::Tick does), so the shared poll below is skipped.
	bool sliced = false;
	auto wait = [&](const std::function<bool()>& progress, std::uint64_t timeoutMs = 45000) {
		const auto deadline = GetTickCount64() + timeoutMs;
		do { if (!sliced) for (auto& room : rooms) room->Poll(); if(serviceJoins) serviceJoins(); if (progress()) return; Sleep(2); } while (GetTickCount64() < deadline);
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
	// A member whose room is Ready gets its server, client connection and
	// match session; the late joiner does the same in the middle of game one.
	const auto attach = [&](std::size_t i) {
		CHECK(test::ConfigureIrohIntegrationServer(recoveryPeers[i], "authorized-match-test", static_cast<std::uint8_t>(Count)));
		clients[i]->RequireCustomRooms();
		clients[i]->RequireMatchAuthorization();
		CHECK(clients[i]->Connect(rooms[i]->Client(), false) == 0);
		matches[i].reset(new session::IrohMatchSession(*clients[i], rooms[i]));
	};
	for (std::size_t i = 0; i < admitted; ++i) {
		phase="admission " + std::to_string(i+1);
		if (i) {
			CHECK(rooms[i]->Join(rooms[0]->Invitation(), "authorized-match-test"));
			wait([&]() { return rooms[i]->GetState() == session::IrohRoom::State::Ready; });
		}
		attach(i);
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
	for (std::size_t i = 0; i < admitted; ++i) recovery.push_back(&recoveryPeers[i]);
	std::vector<SessionClient*> clientViews;
	for (std::size_t i = 0; i < admitted; ++i) clientViews.push_back(clients[i].get());
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
		for (std::size_t i = 0; i < (std::min<std::size_t>)(4, Count); ++i) {
			if (!matches[i]) continue; // the late joiner has no match session until it is admitted
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
		for (std::size_t i = 0; i < (std::min<std::size_t>)(4, Count); ++i)
			std::cout << " client" << i << "=" << tableZeroTrace.clientPhases[i] << "/" << tableZeroTrace.clientGenerations[i];
		std::cout << '\n';
	};
	// --perf: one member's whole tick in the order the application runs it.
	// Raw samples are kept because TimingStat percentiles are bucket bounds.
	static const char* const kParts[] = {"recovery_tick", "room_advance", "server_step", "client_step", "match_tick", "game"};
	// Attributed from the diagnostics' per-frame accumulator. room.poll nests
	// inside recovery_tick and includes the checkpoint pump.
	static const int kOps[] = {diag::OP_ROOM_POLL, diag::OP_ROOM_IMPORT_APPLY,
		diag::OP_ROOM_IMPORT_REBIND, diag::OP_ROOM_CHECKPOINT_BUILD, diag::OP_ROOM_BROADCAST, diag::OP_ROOM_PROPOSE,
		diag::OP_ROOM_JOURNAL, diag::OP_ROOM_COMMIT_SEND, diag::OP_ROOM_COMPACT};
	constexpr std::size_t kOpCount = sizeof(kOps) / sizeof(kOps[0]);
	struct Samples { std::vector<double> total, parts[6], ops[kOpCount]; };
	std::map<std::string, Samples> samples;
	std::uint64_t helperTickLagMaxUs = 0, helperTickBodyMaxUs = 0, helperEventFreeMin = UINT64_MAX;
	const auto role = [&](std::size_t i) {
		return i == 0 ? "host_p1" : i % perTable == 0 ? "p1" : i % perTable == 1 ? "p2" : "spectator";
	};
	const auto slice = [&](std::size_t i, const char* phaseName, const std::function<void()>& game) {
		auto& d = diag::G();
		d.OnOuterFrame(0.0); // clears per-frame attribution
		const double t0 = diag::NowMs();
		double mark[7] = {t0};
		CHECK(recoveryPeers[i].recovery.Tick(*recoveryPeers[i].server, *rooms[i])); mark[1] = diag::NowMs();
		recoveryPeers[i].server->AdvanceCustomRoom(GetTickCount64()); mark[2] = diag::NowMs();
		CHECK(recoveryPeers[i].server->Step() == 0); mark[3] = diag::NowMs();
		CHECK(clients[i]->Step() == 0); mark[4] = diag::NowMs();
		if (live(i) && !matches[i]->Tick(ggpo[i] != nullptr)) { std::cerr << "match " << i << " " << matches[i]->Error() << '\n'; CHECK(false); }
		mark[5] = diag::NowMs();
		if (game) game();
		mark[6] = diag::NowMs();
		auto& bucket = samples[std::string(role(i)) + "." + phaseName];
		bucket.total.push_back(mark[6] - t0);
		for (std::size_t part = 0; part < 6; ++part) bucket.parts[part].push_back(mark[part + 1] - mark[part]);
		for (std::size_t op = 0; op < kOpCount; ++op) bucket.ops[op].push_back(d.frameMs[kOps[op]]);
	};
	auto pump = [&]() {
		if (sliced) { for (std::size_t i = 0; i < Count; ++i) slice(i, "lifecycle", {}); traceTableZero(); return; }
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
		for (std::size_t i = 0; i < Count; ++i) if (live(i) && !matches[i]->Tick(ggpo[i] != nullptr)) {
			std::cerr << "match " << i << " " << matches[i]->Error() << '\n'; CHECK(false);
		}
		traceTableZero();
	};
	auto allViewsCurrent = [&]() {
		const auto revision = server.RoomSnapshot()->revision;
		return std::all_of(clientViews.begin(), clientViews.end(), [&](const SessionClient* client) {
			return client->GetRoomSnapshot().revision == revision;
		});
	};
	wait([&]() { pump(); return std::all_of(clientViews.begin(), clientViews.end(), [&](const SessionClient* client) { return client->GetRoomSnapshot().members.size() == admitted; }); });
	sliced = perf;
	const auto queueMember = [&](std::size_t i) {
		const auto table = static_cast<std::uint8_t>(i / perTable);
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
	};
	for (std::size_t i = 0; i < admitted; ++i) queueMember(i);
	using Phase = session::IrohMatchSession::Phase;
	auto initialTables = server.RoomSnapshot()->tables;
	for (std::uint64_t cycle = 1; cycle <= rematchCycles; ++cycle) {
		phase="match " + std::to_string(cycle);
		stallActive = stallSpectator && cycle == 1;
		const bool stalledThisCycle = stallActive;
		const bool lateThisCycle = lateSpectator && cycle == 2;
		const bool joinsThisCycle = lateJoin && cycle == 1;
		// The members this cycle withholds: the stalled spectator while its
		// pre-start grace runs, the late spectator that still owes the previous
		// generation's terminal acknowledgement, and the member not yet admitted.
		const auto skip = [&](std::size_t i) {
			return (stalledThisCycle && i == stalled) || (lateThisCycle && i == late) || (joinsThisCycle && i == joiner);
		};
		const auto allLive = [&](const std::function<bool(const session::IrohMatchSession&)>& predicate) {
			for (std::size_t i = 0; i < Count; ++i) if (!skip(i) && !predicate(*matches[i])) return false;
			return true;
		};
		for (std::size_t readyIndex = 0; readyIndex < admitted; ++readyIndex) if (clients[readyIndex]->IsLocalPlayer()) {
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
        // Preparation and connection each have their own native deadline.
        // A sixteen-client fixture must observe both phases rather than
        // spend one 45-second allowance across both replicated fan-outs.
        wait([&]() { pump(); return allLive([](const session::IrohMatchSession& match) {
            const auto phase=match.GetPhase();
            return phase==Phase::Prepared || phase==Phase::Connecting || phase==Phase::Started;
        }); });
		const auto setupStarted = GetTickCount64();
		wait([&]() { pump(); return allLive([](const session::IrohMatchSession& match) { return match.GetPhase() == Phase::Started; }); });
		for (std::size_t i = 0; i < Count; ++i) if (!skip(i)) CHECK(matches[i]->Generation() != 0);
		if (stalledThisCycle) {
			// The fighters and the other spectators started without it, within
			// P1's grace plus ordinary setup time. It then resumes, retires the
			// generation it never joined, and must be back in the next one.
			CHECK(matches[stalled]->GetPhase() == Phase::Idle);
			std::cout << "Started without the stalled spectator after " << (GetTickCount64() - setupStarted) << " ms\n";
			stallActive = false;
		}
		if (lateThisCycle) {
			// The grant and every projection of this generation left it out, so
			// the fighters accepted the roster. Now it acknowledges game one.
			CHECK(matches[late]->GetPhase() == Phase::Idle && matches[late]->Generation() == lateGeneration);
			CHECK(matches[0]->Roster().size() == perTable - 1);
			std::cout << "Started without the retiring spectator\n";
			bool observed = false, queued = false;
			const std::vector<std::pair<std::uint8_t, std::uint64_t>> owed{{static_cast<std::uint8_t>(late / perTable), lateGeneration}};
			wait([&]() {
				pump();
				test::AcknowledgeIrohFixtureTerminal(*clients[late], owed[0].first, lateGeneration, true, observed, queued);
				return queued && test::IrohFixtureTerminalsCommitted(server, owed) && allViewsCurrent();
			});
		}
		if (lateSpectator && cycle == 3) CHECK(matches[late]->Generation() == matches[late / perTable * perTable]->Generation());
		{
			// A fighter's desync check is forwarded by the leader straight to
			// the other participants of its table, without a commit token and
			// without building a room checkpoint. Every spectator and the
			// opponent must receive it; a member at another table must not.
			CHECK(matches[0]->LocalSlot() == 0);
			// Count from a quiet server: a commit still settling from setup, or
			// from the late spectator's acknowledgement, builds its own checkpoint.
			wait([&]() { pump(); return !server.HasRecoveryCandidate() && !server.PendingProposal() && allViewsCurrent(); });
			const auto builds = server.RecoveryCheckpointBuilds();
			const auto revisionBefore = server.RoomSnapshot()->revision;
			SessionProtocol::BattleHashV2 hash;
			hash.frameIdx = static_cast<int>(30 * cycle); hash.fromPlayer = true;
			nlohmann::json payload = hash;
			// Like the game, which offers an unsent hash again every frame: a
			// relayed room can be briefly non-writable right after setup.
			wait([&]() { pump(); return clients[0]->Send(payload, nullptr) == session::SendResult::Queued; });
			// A late joiner at table 0 is not connected yet and cannot receive it.
			const std::size_t participants = (std::min)(perTable, admitted);
			wait([&]() { pump(); return std::all_of(clients.begin() + 1, clients.begin() + participants,
				[&](const std::unique_ptr<SessionClient>& client) { return client->pendingRemoteHashes.count(hash.frameIdx) == 1; }); });
			for (std::size_t i = participants; i < Count; ++i) CHECK(clients[i]->pendingRemoteHashes.count(hash.frameIdx) == 0);
			// The late spectator's client may still be retrying the acknowledgement
			// it just committed; the duplicate's reply is a commit of its own, at
			// an unchanged room revision, and says nothing about the forward.
			CHECK(lateThisCycle ? server.RoomSnapshot()->revision == revisionBefore : server.RecoveryCheckpointBuilds() == builds);
			std::cout << "Cycle " << cycle << " verification frame " << hash.frameIdx << " forwarded to " << (participants - 1)
				<< " participants without a checkpoint\n";
		}
		for(std::size_t i=0;i<Count;++i) if(!skip(i) && matches[i]->LocalSlot()==1) {
			const auto table=i/perTable;
			const auto hostIndex=table*perTable;
			const auto hostRoute=rooms[hostIndex]->Game(rooms[i]->LocalIdentity()).route;
			const auto guestRoute=rooms[i]->Game(rooms[hostIndex]->LocalIdentity()).route;
			CHECK(!hostRoute.empty() && !guestRoute.empty());
			if(relay) CHECK(hostRoute.rfind("relay:",0)==0 && guestRoute.rfind("relay:",0)==0);
			std::cout << "Cycle " << cycle << " table " << table << " generation=" << matches[i]->Generation()
				<< " selected routes=" << hostRoute << "," << guestRoute << '\n';
		}
		for (std::size_t table = 1; table < tableCount; ++table)
			CHECK(matches[table * perTable]->Generation() != matches[0]->Generation());
		std::cout << "Generation " << cycle << " authorized; creating GGPO sessions\n";
		std::vector<GGPOPlayerHandle> localHandles(Count);
		auto callbacks = GameCallbacks();
		for (std::size_t i = 0; i < Count; ++i) {
			if (skip(i)) continue;
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
					// P1 dropped the stalled spectator's link; like StartRuntimeGgpo,
					// leave it out of GGPO.
					if (p >= 2 && !matches[i]->RemotePort(roster[p])) {
						CHECK(stalledThisCycle && slot == 0);
						std::cout << "P1 started GGPO without spectator slot " << p << '\n';
						continue;
					}
					if (p != slot) {
						CHECK(matches[i]->RemotePort(roster[p]) != 0);
						strcpy_s(player.u.remote.ip_address, "127.0.0.1"); player.u.remote.port = matches[i]->RemotePort(roster[p]);
					}
					GGPOPlayerHandle handle; CHECK(ggpo_add_player(ggpo[i], &player, &handle) == GGPO_OK);
					if (p == slot) { localHandles[i] = handle; CHECK(ggpo_set_frame_delay(ggpo[i], handle, 2) == GGPO_OK); }
				}
			}
		}
		std::vector<int> frames(Count, 0);
		if (stalledThisCycle) frames[stalled] = watchFrames;
		if (lateThisCycle) frames[late] = watchFrames;
		if (joinsThisCycle) frames[joiner] = watchFrames;
		// The late join runs in stages between game ticks, so the fighters keep
		// exchanging inputs throughout: join the room, attach once the room is
		// Ready, then wait until every member sees the full roster.
		int joinStage = joinsThisCycle ? 0 : 3;
		const auto joinDuringFight = [&]() {
			if (joinStage == 0 && frames[0] >= 10) {
				phase = "late join " + std::to_string(cycle);
				CHECK(rooms[joiner]->Join(rooms[0]->Invitation(), "authorized-match-test"));
				joinStage = 1;
			} else if (joinStage == 1 && rooms[joiner]->GetState() == session::IrohRoom::State::Ready) {
				attach(joiner);
				recovery.push_back(&recoveryPeers[joiner]); clientViews.push_back(clients[joiner].get());
				admitted = Count;
				joinStage = 2;
			} else if (joinStage == 2 && std::all_of(clientViews.begin(), clientViews.end(), [&](const SessionClient* client) {
				return client->GetRoomSnapshot().members.size() == Count; })) {
				std::cout << "Member " << joiner + 1 << " joined during the fight at frame " << frames[0] << '\n';
				joinStage = 3;
			}
		};
		std::deque<bool> inputAdded(Count, false);
		const auto advance = [&](std::size_t i) {
				if (!ggpo[i]) return;
				activeSession = ggpo[i]; CHECK(ggpo_idle(ggpo[i], 0) == GGPO_OK);
				const bool player = matches[i]->LocalSlot() < 2;
				if (frames[i] >= (player ? playerFrames : watchFrames)) return;
				if (player && !inputAdded[i]) {
					std::array<unsigned char, session::GgpoInputBytes> input;
					for (std::size_t byte = 0; byte < input.size(); ++byte) input[byte] = static_cast<unsigned char>((frames[i] * 73 + byte * 31 + i) & 255);
					if (ggpo_add_local_input(ggpo[i], localHandles[i], input.data(), static_cast<int>(input.size())) != GGPO_OK) return;
					inputAdded[i] = true;
				}
				unsigned char inputs[2 * session::GgpoInputBytes] = {}; int disconnected = 0;
				if (ggpo_synchronize_input(ggpo[i], inputs, sizeof(inputs), &disconnected) != GGPO_OK) return;
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
		};
		// The 45 s allowance covers 60 frames; scale it for a long timed run.
		wait([&]() {
			const double tickStart = diag::NowMs();
			if (!sliced) { pump(); joinDuringFight(); for (std::size_t i = 0; i < Count; ++i) advance(i); Sleep(14); } // approximate a game tick
			else {
				for (std::size_t i = 0; i < Count; ++i) slice(i, "match", [&]() {
					advance(i);
					// Synthetic SF4 frame cost, inside the slice so the 16.67 ms counts mean something.
					const double spinStart = diag::NowMs();
					while ((diag::NowMs() - spinStart) * 1000.0 < gameCostUs) {}
				});
				traceTableZero();
				for (const auto& room : rooms) {
					const auto& load = room->HelperLoad();
					if (!load.samples) continue;
					helperTickLagMaxUs = (std::max)(helperTickLagMaxUs, load.actorTickLagMaxUs);
					helperTickBodyMaxUs = (std::max)(helperTickBodyMaxUs, load.actorTickBodyMaxUs);
					helperEventFreeMin = (std::min)(helperEventFreeMin, load.eventQueueFreeMin);
				}
				const double spent = diag::NowMs() - tickStart;
				if (spent < 16.0) Sleep(static_cast<DWORD>(16.0 - spent));
			}
			return joinStage == 3 && std::all_of(frames.begin(), frames.end(), [&](int frame) { return frame >= watchFrames; });
		}, 45000 + static_cast<std::uint64_t>(watchFrames) * (20 + Count * (2 + gameCostUs / 1000)));
		if (joinsThisCycle) {
			// The fight ran to its end with the room whole: every room is still
			// Ready and no match session failed (both are checked every tick).
			for (const auto& room : rooms) CHECK(room->GetState() == session::IrohRoom::State::Ready);
		}
		std::cout << "Generation " << cycle << " input streams completed; retiring GGPO\n";
		std::vector<std::uint64_t> retiredGenerations(Count, 0);
		for (std::size_t i = 0; i < Count; ++i) retiredGenerations[i] = matches[(skip(i) ? i / perTable * perTable : i)]->Generation();
		// Mirror the game's match-ended notification before retiring its socket.
		// Control and QUIC close events may otherwise arrive in either order.
		for (auto& match : matches) match->End();
		for (auto& session : ggpo) if (session) { activeSession = session; CHECK(ggpo_close_session(session) == GGPO_OK); session = nullptr; }
		// Capture the native outcome identity before pumping teardown. Control may
		// be transiently unavailable and LocalSlot may retire while this immutable
		// report remains pending.
		std::vector<room::Action> retainedResults(Count);
		std::deque<bool> resultParticipant(Count, false), resultConfirmed(Count, false);
		std::vector<std::uint64_t> resultRetryAt(Count, 0);
		std::vector<std::uint64_t> resultActionIds(Count, 0);
		for (std::size_t i = 0; i < Count; ++i) if (!skip(i) && matches[i]->LocalSlot() < 2) {
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
		std::deque<bool> observedTerminals(Count, false), queuedTerminalAcks(Count, false);
		// Game one: the late spectator keeps its receipt open. Game two: it was
		// never a recipient.
		const bool lateWithheld = lateSpectator && cycle <= 2;
		if (lateWithheld) queuedTerminalAcks[late] = true;
		// The late joiner was never a recipient of game one's receipt.
		if (joinsThisCycle) queuedTerminalAcks[joiner] = true;
		if (lateSpectator && cycle == 1) lateGeneration = retiredGenerations[late];
		std::vector<std::pair<std::uint8_t, std::uint64_t>> terminalKeys;
		for (std::size_t table = 0; table < tableCount; ++table)
			terminalKeys.emplace_back(static_cast<std::uint8_t>(table), retiredGenerations[table * perTable]);
		phase = "terminal receipt acknowledgement " + std::to_string(cycle);
		wait([&]() {
			pump();
			for (std::size_t i = 0; i < Count; ++i) if (!(lateWithheld && i == late) && !(joinsThisCycle && i == joiner))
				test::AcknowledgeIrohFixtureTerminal(*clients[i], static_cast<std::uint8_t>(i / perTable),
					retiredGenerations[i], ggpo[i] == nullptr && matches[i]->GetPhase() == Phase::Idle,
					observedTerminals[i], queuedTerminalAcks[i]);
			// While the late spectator still withholds game one's receipt, its
			// table must not show as settled: no recovery candidate, no proposal
			// and no pending terminal. Every other table commits as usual.
			const bool settled = lateSpectator && cycle == 1
				? !server.HasRecoveryCandidate() && !server.PendingProposal() &&
					!clients[late / perTable * perTable]->GetRoomSnapshot().terminalPending[late / perTable]
				: test::IrohFixtureTerminalsCommitted(server, terminalKeys);
			return std::all_of(queuedTerminalAcks.begin(), queuedTerminalAcks.end(), [](bool value) { return value; }) &&
				settled && allViewsCurrent();
		});
		for (std::size_t table = 0; table < tableCount; ++table) {
			const auto& current = server.RoomSnapshot()->tables[table];
			CHECK(current.p1 == initialTables[table].p1 && current.p2 == initialTables[table].p2);
			CHECK(current.queue == initialTables[table].queue);
			CHECK(current.score[0] == cycle && current.score[1] == 0);
		}
		if (joinsThisCycle) {
			// Now a spectator at its table, in every game from here on.
			queueMember(joiner);
			initialTables = server.RoomSnapshot()->tables;
		}
		std::cout << "Custom room cycle " << cycle << ": independently authorized input streams passed\n";
	}
	sliced = false;
	if (perf) {
		// "lifecycle" slices are a member's control-plane tick during ready,
		// prepare, connect, result and terminal phases: what a fighter at
		// another table pays on top of its frame while this one rematches.
		nlohmann::json report = {{"members", Count}, {"per_table", perTable}, {"relay", relay}, {"frames", perfFrames},
			{"game_cost_us", gameCostUs}, {"cycles", rematchCycles}, {"roles", nlohmann::json::object()},
			// Worst helper-side load reported during matches, across all helpers.
			{"helper", {{"tick_lag_max_us", helperTickLagMaxUs}, {"tick_body_max_us", helperTickBodyMaxUs},
				{"event_queue_free_min", helperEventFreeMin == UINT64_MAX ? 0 : helperEventFreeMin}}}};
		std::cout << "RoomPerf helper " << report["helper"].dump() << '\n';
		const auto summary = [](std::vector<double> values) {
			std::sort(values.begin(), values.end());
			const auto at = [&](double p) { return values.empty() ? 0.0 : values[(std::min)(values.size() - 1, static_cast<std::size_t>(p * values.size()))]; };
			return nlohmann::json{{"n", values.size()}, {"p50", at(0.50)}, {"p95", at(0.95)}, {"p99", at(0.99)},
				{"max", values.empty() ? 0.0 : values.back()},
				{"over_16_67", std::count_if(values.begin(), values.end(), [](double v) { return v > 16.67; })},
				{"over_25", std::count_if(values.begin(), values.end(), [](double v) { return v > 25.0; })}};
		};
		for (const auto& entry : samples) {
			auto& out = report["roles"][entry.first];
			out = {{"tick_ms", summary(entry.second.total)}};
			for (std::size_t part = 0; part < 6; ++part) out[kParts[part]] = summary(entry.second.parts[part]);
			for (std::size_t op = 0; op < kOpCount; ++op) out[diag::TimedOpName(kOps[op])] = summary(entry.second.ops[op]);
		}
		// One line per role and phase; the JSON file carries every distribution.
		std::cout << std::fixed << std::setprecision(2);
		for (const auto& entry : report["roles"].items()) {
			std::cout << "RoomPerf " << entry.key() << " [p99/max ms]";
			for (const auto& field : entry.value().items())
				std::cout << ' ' << field.key() << '=' << field.value()["p99"].get<double>() << '/' << field.value()["max"].get<double>();
			std::cout << " over16.67=" << entry.value()["tick_ms"]["over_16_67"] << " over25=" << entry.value()["tick_ms"]["over_25"] << '\n';
		}
		static char diagnostics[16384];
		if (diag::G().FormatSummary(diagnostics, sizeof(diagnostics), "room-perf")) std::cout << diagnostics << '\n';
		if (!jsonPath.empty()) { std::ofstream file(jsonPath); file << report.dump(1); CHECK(file.good()); }
	}
	for (auto& match : matches) match.reset();
	for (auto& client : clients) client->Disconnect();
	server.Close();
	for (auto& helper : helpers) CHECK(helper.Send("{\"type\":\"shutdown\"}"));
	wait([&]() { return std::all_of(processes.begin(), processes.end(), [](const platform::HelperProcess& process) { return !process.IsRunning(); }); });
	std::cout << Count << " members; " << tableCount << " concurrent match(es) with " << (perTable - 2) << " spectator(s) each; "
		<< rematchCycles << " games without automatic rotation and spectator input verification passed. No SF4 simulation tested.\n";
}
