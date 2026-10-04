#include "soak_workload.hxx"
#include "../public_room_support.hxx"
#include <nlohmann/json.hpp>
#include <sstream>

namespace sf4e { namespace test { namespace soak {
using namespace sf4e::test::publicroom;

const char* const RejectNames[] = {"None", "Closed", "RoomFull", "AdmissionLocked", "NameTaken", "UnknownMember", "UnknownTable",
	"NotHost", "NotSeated", "AlreadySeated", "AlreadyQueued", "NotQueued", "NotWatching", "InvalidSeat", "InvalidRules",
	"InvalidCapacity", "NotReady", "StaleRoom", "StaleTable", "WrongPhase", "WrongGeneration", "Unauthorized", "DuplicateResult",
	"InvalidChat", "MemberKicked", "TerminalLedgerFull"};
std::string RejectName(room::RejectReason reason) {
	const auto index = static_cast<std::size_t>(reason);
	return index < sizeof(RejectNames) / sizeof(RejectNames[0]) ? RejectNames[index] : "Reason" + std::to_string(index);
}

// ---- Member -------------------------------------------------------------

void Member::StartSession(Clock now) {
	SlowCall slow{"helper start", room->number, index};
	s.reset(new Session());
	s->peer.name = label;
	phase = Phase::Starting;
	phaseSince = now;
	statusAsked = false;
	turnGranted = false;
	if (!s->process.Start(options.helper, GetCurrentProcessId()) || !s->helper.Start(s->process.Bootstrap())) {
		FailJoin("helper_start", "error " + std::to_string(s->process.LastError()), now);
		return;
	}
	s->peer.room = std::make_shared<session::IrohRoom>(s->helper);
}

void Member::FailJoin(const std::string& reason, const std::string& detail, Clock now) {
	auto& st = room->st;
	if (reason == "refused") ++st.refused; else ++st.joinFailures;
	++st.joinFailureReasons[reason];
	Event(room->number, index, "join failed: " + reason + (detail.empty() ? "" : " (" + detail + ")") + " after " +
		std::to_string(Elapsed(now, joinStarted)) + " ms, attempt " + std::to_string(attempts + 1));
	++attempts;
	const Clock backoff = (std::min<Clock>)(60000, 5000ull << (std::min)(attempts - 1, 4));
	BeginLeave("join failed", backoff + Seconds(0, 3), now);
}

void Member::Activated(Clock now) {
	phase = Phase::Active;
	phaseSince = activeSince = now;
	attempts = 0;
	auto& st = room->st;
	const auto took = Elapsed(now, joinStarted);
	++st.joins;
	if (everJoined) ++st.rejoins;
	st.joinMs.push_back(took);
	st.joinMsAll.push_back(took);
	Event(room->number, index, std::string(everJoined ? "rejoined" : "joined") + " in " + std::to_string(took) + " ms, " +
		std::to_string(View().members.size()) + " members in view", false);
	everJoined = true;
	planned = false;
	if (index == 0) room->creatorJoined = true;
	if (room->joiner == this) room->joiner = nullptr;
	nextChatAt = now + Seconds(30 / options.activity, 200 / options.activity);
	nextActAt = now + Seconds(1, 6);
	degradedSince = 0;
	matchEnded = {};
	closedSeen = false;
}

// Ends the session (the fixture's StopPlayer, without blocking) and schedules
// the next start retryInMs after the helper is gone.
void Member::BeginLeave(const std::string& why, Clock retryInMs, Clock now) {
	SlowCall slow{"leave", room->number, index};
	if (phase == Phase::Leaving || phase == Phase::Down) return;
	if (room->joiner == this) room->joiner = nullptr;
	pending.clear();
	busy = false;
	turnGranted = false;
	nextAt = retryInMs;
	if (s) {
		auto& peer = s->peer;
		s->match.reset();
		if (peer.client) peer.client->Disconnect();
		else if (peer.room) peer.room->Leave();
		peer.client.reset();
		peer.server.reset();
		peer.configured = false;
	}
	Event(room->number, index, "leaving: " + why, false);
	phase = Phase::Leaving;
	leaveStage = 0;
	phaseSince = now;
}

void Member::ControlLoss(const std::string& why, Clock now) {
	auto& st = room->st;
	++st.controlLosses;
	++st.lossReasons[why.substr(0, why.find(':'))];
	std::string detail = why;
	if (s && s->peer.room) detail += " | room state " + std::to_string(static_cast<int>(s->Room().GetState())) + " error '" + s->Room().Error() + "'";
	Event(room->number, index, "control lost: " + detail + " after " + std::to_string(Elapsed(now, activeSince) / 1000) + " s");
	BeginLeave("control lost", Seconds(3, 8), now);
}

bool Member::Send(room::Action action, Clock now, std::uint64_t* idOut) {
	if (!IsActive() || !s->peer.client) return false;
	const auto& view = View();
	action.roomEpoch = view.roomEpoch;
	action.revision = view.revision;
	action.tableRevision = view.tables[action.table % room::TableCount].revision;
	std::uint64_t id = 0;
	if (s->peer.client->SendRoomAction(action, &id) != session::SendResult::Queued) { ++room->st.sendFailures; return false; }
	pending[id] = Pending{action.kind, now, 0, room::RejectReason::None, action.kind == room::ActionKind::Chat ? action.text : std::string()};
	++room->st.actionsSent;
	if (idOut) *idOut = id;
	return true;
}

void Member::Drain(Clock now) {
	auto& st = room->st;
	SessionClient::ActionReply reply;
	while (s->peer.client->TakeActionReply(reply)) {
		const auto found = pending.find(reply.actionId);
		if (found == pending.end()) continue; // a terminal acknowledgement's own retries
		auto& entry = found->second;
		if (reply.superseded) { entry.outcome = 3; ++st.actionsSuperseded; }
		else if (reply.accepted) {
			entry.outcome = 1; ++st.actionsAccepted;
			if (entry.kind == room::ActionKind::Chat) { room->chatLog.push_back({entry.text, now, this, activeSince}); if (room->chatLog.size() > 400) room->chatLog.pop_front(); }
		}
		else if (reply.reason == room::RejectReason::DuplicateResult) { entry.outcome = 1; entry.reason = reply.reason; ++st.actionsAccepted; }
		else { entry.outcome = 2; entry.reason = reply.reason; ++st.actionsRejected; ++st.rejects[RejectName(reply.reason)]; }
	}
	room::Event event;
	while (s->peer.client->TakeRoomEvent(event)) {
		if (event.kind == room::Event::Kind::MatchEnded && event.table < matchEnded.size()) matchEnded[event.table] = event.matchGeneration;
		else if (event.kind == room::Event::Kind::RoomClosed && !closedSeen) { closedSeen = true; ++st.roomClosed; Event(room->number, index, "room closed event"); }
	}
	for (auto it = pending.begin(); it != pending.end();) {
		const auto age = Elapsed(now, it->second.sentAt);
		if (it->second.outcome == 0 && age > 15000) {
			++st.actionTimeouts;
			Event(room->number, index, "action timeout: kind " + std::to_string(static_cast<int>(it->second.kind)));
			it = pending.erase(it);
		} else if (it->second.outcome != 0 && age > 20000) it = pending.erase(it);
		else ++it;
	}
	// A fighter change is seen when the member's own row shows it.
	if (fighterWanted >= 0) {
		const auto* me = room::FindMember(View(), View().localMember);
		if (me && me->fighter == fighterWanted) fighterWanted = -1;
		else if (Elapsed(now, fighterSentAt) > 10000) { ++st.fighterUnseen; fighterWanted = -1; }
	}
}

void Member::Health(Clock now) {
	// A crash is a control loss too: ControlLoss counts it, helperCrashes notes the cause.
	if (!s->process.IsRunning()) { ++room->st.helperCrashes; ControlLoss("helper_crash", now); return; }
	if (s->failed) { ControlLoss(s->failure.empty() ? "pump_failed" : s->failure, now); return; }
	const auto state = s->Room().GetState();
	if (state == State::Degraded) {
		if (!degradedSince) { degradedSince = now; ++room->st.degraded; Event(room->number, index, "room degraded (control down)"); }
		else if (Elapsed(now, degradedSince) > 20000) { ControlLoss("degraded_20s", now); return; }
	} else degradedSince = 0;
	if (state == State::Failed || state == State::Idle) { ControlLoss("room_" + std::string(state == State::Failed ? "failed" : "idle") + ": " + s->Room().Error(), now); return; }
	if (!s->peer.client->IsConnected()) { ControlLoss("client_disconnected: " + s->peer.client->RoomError(), now); return; }
	const auto& view = View();
	if (!room::FindMember(view, view.localMember)) { ControlLoss("not_a_member", now); return; }
	if (view.closed && !closedSeen) { closedSeen = true; ++room->st.roomClosed; Event(room->number, index, "snapshot says the room is closed"); }
}

void Member::Chat(Clock now) {
	std::ostringstream text;
	text << "soak r" << room->number << " m" << index << " #" << ++chatSeq << ' ';
	static const char letters[] = "abcdefghijklmnopqrstuvwxyz     ";
	for (auto n = Uniform(8, 70); n; --n) text << letters[Uniform(0, sizeof(letters) - 2)];
	text << '.'; // never ends in a space
	room::Action action;
	action.kind = room::ActionKind::Chat;
	action.text = text.str();
	if (!Send(action, now)) return;
	++room->st.chatSent;
	// One message in four is timed to another member's snapshot. The sample
	// is stamped with this tick's time, like every stamp a tick makes, and
	// Room::Tick measures it with Elapsed.
	if (Chance(0.25)) {
		std::vector<Member*> others;
		for (auto& other : room->members) if (other.get() != this && other->IsActive()) others.push_back(other.get());
		if (!others.empty()) {
			auto* observer = others[Uniform(0, others.size() - 1)];
			room->samples.push_back(ChatSample{action.text, now, observer, observer->activeSince, this, activeSince});
			++room->st.chatSampled;
		}
	}
}

// One thing a player does, chosen by where the member stands now.
void Member::Act(Clock now) {
	const auto& view = View();
	const auto* me = room::FindMember(view, view.localMember);
	if (!me || view.localTerminalPending) return;
	const auto place = room::PlaceOf(view, view.localMember);
	const auto otherTable = [&]() { return index < 2 ? 0 : 1 + static_cast<int>(Uniform(0, 2)); };
	room::Action action;
	const auto simple = [&](room::ActionKind kind, int table) {
		action.kind = kind;
		action.table = static_cast<std::uint8_t>(table);
		Send(action, now);
	};
	if (place.kind == room::Place::Kind::Seat) {
		const auto& table = view.tables[place.table];
		auto* opponent = room->ByMemberId(place.seat == 0 ? table.p2 : table.p1);
		const bool idle = (table.phase == room::TablePhase::Waiting || table.phase == room::TablePhase::Idle) && !table.ready[0] && !table.ready[1];
		const auto roll = std::uniform_real_distribution<double>(0, 1)(rng);
		if (roll < 0.30) {
			if (!room::SeatEditable(table, place.seat)) return;
			++room->st.fighterSent;
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions chara{};
			fighterWanted = static_cast<int>(Uniform(0, 34));
			if (me->fighter == fighterWanted) fighterWanted = (fighterWanted + 1) % 35;
			chara.charaID = static_cast<std::uint8_t>(fighterWanted);
			chara.unc_edition = 14;
			fighterSentAt = now;
			if (s->peer.client->PreBattle_SetChara(chara) != session::SendResult::Queued) { ++room->st.fighterRefused; fighterWanted = -1; }
		} else if (roll < 0.55) {
			if (opponent && idle) room->StartFlow(Flow::Kind::Ready, static_cast<std::uint8_t>(place.table), this, opponent, now);
		} else if (roll < 0.70) {
			if (opponent && idle) room->StartFlow(Flow::Kind::Probe, static_cast<std::uint8_t>(place.table), this, opponent, now);
		} else if (roll < 0.85) {
			if (idle && !room->flows[place.table]) simple(room::ActionKind::Unqueue, place.table);
		}
	} else if (place.kind == room::Place::Kind::Queue) {
		if (Chance(0.5)) simple(room::ActionKind::Unqueue, place.table);
	} else if (me->status == room::MemberStatus::Watching || me->status == room::MemberStatus::WatchingNext) {
		const auto roll = std::uniform_real_distribution<double>(0, 1)(rng);
		if (roll < 0.40) {
			for (const auto& table : view.tables)
				if (std::find(table.spectators.begin(), table.spectators.end(), view.localMember) != table.spectators.end() ||
					std::find(table.watchingNext.begin(), table.watchingNext.end(), view.localMember) != table.watchingNext.end()) {
					simple(room::ActionKind::Unwatch, table.id);
					break;
				}
		} else if (roll < 0.70) simple(room::ActionKind::Watch, otherTable());
	} else if (index < 2) {
		simple(room::ActionKind::Queue, 0); // table 0 is the match table: only its two fighters come near it
	} else if (index < 8) {
		simple(Chance(0.85) ? room::ActionKind::Queue : room::ActionKind::Watch, HomeTable());
	} else {
		const auto roll = std::uniform_real_distribution<double>(0, 1)(rng);
		if (roll < 0.60) simple(room::ActionKind::Watch, HomeTable());
		else if (roll < 0.85) simple(room::ActionKind::Queue, HomeTable());
	}
}

void Member::Tick(Clock now) {
	switch (phase) {
	case Phase::Down:
		if (!room->closing && now >= nextAt) { joinStarted = now; StartSession(now); }
		return;
	case Phase::Starting:
		s->Pump();
		if (s->helper.State() == platform::HelperState::Connected) {
			if (!statusAsked) { statusAsked = s->helper.Send("{\"type\":\"status\"}"); }
			if (statusAsked) { phase = Phase::Identity; phaseSince = now; }
		} else if (Elapsed(now, phaseSince) > 15000) FailJoin("helper_connect_timeout", "", now);
		return;
	case Phase::Identity:
		s->Pump();
		if (!s->Room().LocalIdentity().empty()) { phase = Phase::WaitTurn; phaseSince = now; }
		else if (Elapsed(now, phaseSince) > 15000) FailJoin("identity_timeout", "", now);
		return;
	case Phase::WaitTurn:
		s->Pump();
		if (!turnGranted) return;
		{
			std::vector<std::string> lines;
			SlowCall slow{"ticket and join command", room->number, index};
			if (!RunTool("sign " + Seed('1') + " " + room->kid + " " + Bridge + " " + room->roomIdHex + " " + emberId + " " + s->Room().LocalIdentity(), lines) ||
				lines.empty()) { FailJoin("ticket_tool", "", now); return; }
			const auto ticket = nlohmann::json::parse(lines[0], nullptr, false);
			joinStarted = now;
			if (ticket.is_discarded() || !s->Room().JoinPublic(room->invitation, ticket, Build)) { FailJoin("join_command_refused", "", now); return; }
			phase = Phase::Joining;
			phaseSince = now;
		}
		return;
	case Phase::Joining: {
		s->Pump();
		const auto state = s->Room().GetState();
		if (state == State::Ready) {
			SessionClient::Callbacks callbacks = {};
			callbacks.data = this;
			callbacks.OnError = [](SessionClient::ErrorType, SessionClient* const, const SessionClient::Callbacks& self) {
				++static_cast<Member*>(self.data)->room->st.clientErrors;
			};
			if (!test::ConfigureIrohIntegrationPeer(s->peer, callbacks, Build, 0, static_cast<std::uint8_t>(room::MaximumMembers))) { FailJoin("client_not_attached", "", now); return; }
			phase = Phase::Registering;
			phaseSince = now;
		} else if (state == State::Failed || state == State::Idle) {
			const auto reason = s->Room().FailureReason();
			FailJoin(s->Room().Error() == "join_failed" ? (reason.empty() ? "join_failed_no_reason" : reason) : "room_" + s->Room().Error(),
				"state " + std::to_string(static_cast<int>(state)), now);
		} else if (Elapsed(now, phaseSince) > 45000) FailJoin("join_timeout", "state " + std::to_string(static_cast<int>(state)), now);
		return;
	}
	case Phase::Registering: {
		s->Pump();
		const auto& client = s->peer.client;
		const auto& view = View();
		const bool joined = view.localMember && room::FindMember(view, view.localMember) != nullptr;
		if (joined) Activated(now);
		else if (s->failed) FailJoin("registration_failed", s->failure, now);
		else if (client->JoinRejection()) FailJoin(std::string("rejected_") + SessionClient::PublicJoinRejectionKey(*client->JoinRejection()), "", now);
		else if (Elapsed(now, phaseSince) > 30000) FailJoin("registration_timeout", s->peer.recovery.Error(), now);
		return;
	}
	case Phase::Active:
		s->Pump();
		Drain(now);
		Health(now);
		if (phase != Phase::Active) return;
		if (forceRejoin) { forceRejoin = false; BeginLeave("rejoin after a failed table flow", Seconds(2, 6), now); return; }
		if (now >= nextChatAt) { nextChatAt = now + Seconds(90 / options.activity, 300 / options.activity); Chat(now); }
		if (now >= nextActAt) { nextActAt = now + Seconds(45 / options.activity, 150 / options.activity); if (!busy) Act(now); }
		return;
	case Phase::Leaving:
		s->Pump();
		if (leaveStage == 0) {
			const auto state = s->peer.room ? s->Room().GetState() : State::Idle;
			if (state == State::Idle || state == State::Failed || !s->process.IsRunning() ||
				Elapsed(now, phaseSince) > session::IrohRoom::LeaveTimeoutMs + 2000) {
				s->helper.Send("{\"type\":\"shutdown\"}");
				leaveStage = 1;
				phaseSince = now;
			}
		} else if (!s->process.IsRunning() || Elapsed(now, phaseSince) > 3000) {
			SlowCall slow{"helper stop", room->number, index};
			s->process.Stop(0);
			s.reset();
			phase = Phase::Down;
			nextAt = now + nextAt; // nextAt held the delay
		}
		return;
	}
}

} } }
