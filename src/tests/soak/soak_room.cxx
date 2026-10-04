#include "soak_workload.hxx"
#include "soak_metrics.hxx"

namespace sf4e { namespace test { namespace soak {

// ---- Flows ---------------------------------------------------------------

// A flow is stamped with the tick's time for the same reason as a chat sample:
// TickFlows checks its age later in that tick.
bool Room::StartFlow(Flow::Kind kind, std::uint8_t table, Member* a, Member* b, Clock now) {
	if (flows[table] || !a || !b || a->busy || b->busy) return false;
	flows[table].reset(new Flow());
	auto& flow = *flows[table];
	flow.kind = kind; flow.table = table; flow.a = a; flow.b = b; flow.since = now;
	a->busy = b->busy = true;
	if (kind == Flow::Kind::Match) ++st.matchesStarted;
	return true;
}

void Room::EndFlow(std::uint8_t table) {
	auto& flow = *flows[table];
	flow.a->busy = flow.b->busy = false;
	flows[table].reset();
}

void Room::FailFlow(Flow& flow, const std::string& step, const std::string& detail) {
	const char* kinds[] = {"probe", "ready", "match"};
	const std::string name = std::string(kinds[static_cast<int>(flow.kind)]) + ":" + step;
	if (flow.kind == Flow::Kind::Match) { ++st.matchesFailed; ++st.matchFailureSteps[step]; }
	Event(number, -1, "table " + std::to_string(flow.table) + " " + name + " failed: " + detail);
	for (auto* member : {flow.a, flow.b}) {
		if (!member->IsActive()) continue;
		if (member->s) member->s->match.reset();
		// A table left half-way is cleared by the members leaving it.
		if (flow.kind == Flow::Kind::Match || flow.kind == Flow::Kind::Ready) member->forceRejoin = true;
	}
}

// One leg of the connection check, like the fixture's Probe.
int Room::ProbeLeg(Flow& flow, Member& from, Member& to, Clock now) {
	if (!flow.probeStarted) {
		const auto peer = to.Room_().LocalIdentity();
		const auto control = from.Room_().ConnectionForIdentity(peer);
		if (control == 0 || from.Room_().PeerIncarnation(control) == 0) {
			if (now - flow.since > 15000) { flow.detail = "peer endpoint unknown to the room"; return 2; }
			return 0;
		}
		if (!from.Room_().RequestProbe(peer, ++requestCounter, from.View().tables[flow.table].revision)) { flow.detail = "request refused"; return 2; }
		flow.probeStarted = true;
		flow.since = now;
		return 0;
	}
	const auto& probe = from.Room_().Probe();
	if (probe.status == "checking") {
		if (now - flow.since > 40000) { flow.detail = "no result in 40 s"; return 2; }
		return 0;
	}
	flow.detail = "status=" + probe.status + " route=" + probe.route + " replies=" + std::to_string(probe.samples) + "/" +
		std::to_string(probe.samples + probe.lost) + " failure=" + std::to_string(probe.failureReason);
	return (probe.status == "ready" || probe.status == "complete") && probe.samples >= 80 && probe.recommended >= 0 ? 1 : 2;
}

int Room::AdvanceFlow(Flow& flow, Clock now) {
	Member& a = *flow.a;
	Member& b = *flow.b;
	if (!a.IsActive() || !b.IsActive()) { flow.detail = "a member left"; return 2; }
	const auto tableId = flow.table;
	const auto fail = [&](const std::string& what) { flow.detail = what; return 2; };
	const auto settled = [&](Member& member, std::uint64_t id, Clock limit, std::string& why) {
		const int outcome = member.Outcome(id);
		if (outcome == 1 || outcome == 3) return 1;
		if (outcome == 2) { why = "rejected " + RejectName(member.Reason(id)); return 2; }
		if (now - flow.since > limit) { why = "no reply in " + std::to_string(limit / 1000) + " s"; return 2; }
		return 0;
	};
	room::Action action;
	action.table = tableId;
	switch (flow.kind) {
	case Flow::Kind::Probe: {
		const int r = ProbeLeg(flow, a, b, now);
		if (r == 0) return 0;
		++(r == 1 ? st.probesOk : st.probesFailed);
		if (r == 2) Event(number, -1, "table " + std::to_string(tableId) + " connection check failed: " + flow.detail);
		return 1;
	}
	case Flow::Kind::Ready: {
		std::string why;
		if (flow.step == 0) {
			action.kind = room::ActionKind::Ready; action.inputDelay = 2;
			if (!a.Send(action, &flow.idA)) return fail("send failed");
			flow.step = 1; flow.since = now;
		} else if (flow.step == 1) {
			const int r = settled(a, flow.idA, 15000, why);
			if (r == 2) { flow.step = 5; flow.detail = why; return 1; } // nothing to take back
			if (r == 1) { flow.step = 2; flow.holdUntil = now + Seconds(1, 3); }
		} else if (flow.step == 2) {
			if (now >= flow.holdUntil) {
				action.kind = room::ActionKind::Unready;
				if (!a.Send(action, &flow.idA)) return fail("unready send failed");
				flow.step = 3; flow.since = now;
			}
		} else if (flow.step == 3) {
			const int r = settled(a, flow.idA, 15000, why);
			if (r == 2) return fail("unready " + why);
			if (r == 1) return 1;
		} else return 1;
		return 0;
	}
	case Flow::Kind::Match: {
		std::string why;
		switch (flow.step) {
		case 0: case 1: {
			const int r = flow.step == 0 ? ProbeLeg(flow, a, b, now) : ProbeLeg(flow, b, a, now);
			if (r == 0) return 0;
			++(r == 1 ? st.probesOk : st.probesFailed);
			// A failed check is recorded and the match goes on, as the fixture does.
			if (r == 2) Event(number, -1, std::string("table ") + std::to_string(tableId) + " match check " + (flow.step == 0 ? "A to B" : "B to A") + " failed: " + flow.detail);
			flow.probeStarted = false; flow.since = now; ++flow.step;
			return 0;
		}
		case 2:
			a.s->match.reset(new session::IrohMatchSession(*a.s->peer.client, a.s->peer.room));
			b.s->match.reset(new session::IrohMatchSession(*b.s->peer.client, b.s->peer.room));
			action.kind = room::ActionKind::Ready; action.inputDelay = 2;
			if (!a.Send(action, &flow.idA) || !b.Send(action, &flow.idB)) return fail("ready send failed");
			flow.step = 3; flow.since = now;
			return 0;
		case 3: {
			const int ra = settled(a, flow.idA, 15000, why), rb = settled(b, flow.idB, 15000, why);
			if (ra == 2 || rb == 2) return fail("ready " + why);
			if (ra == 1 && rb == 1) { flow.step = 4; flow.since = now; }
			return 0;
		}
		case 4:
			if (a.s->matchFailed || b.s->matchFailed) return fail("match setup: " + a.s->matchError + " " + b.s->matchError);
			if (a.s->match->GetPhase() == MatchPhase::Started && b.s->match->GetPhase() == MatchPhase::Started) {
				flow.generation = a.s->match->Generation();
				flow.step = 5; flow.holdUntil = now + Seconds(2, 6);
			} else if (now - flow.since > 60000) return fail("not started in 60 s: phases " + std::to_string(static_cast<int>(a.s->match->GetPhase())) +
				"/" + std::to_string(static_cast<int>(b.s->match->GetPhase())));
			return 0;
		case 5:
			if (a.s->matchFailed || b.s->matchFailed) return fail("match failed while running: " + a.s->matchError + " " + b.s->matchError);
			if (now < flow.holdUntil) return 0;
			a.s->match->End(); b.s->match->End();
			action.kind = room::ActionKind::RecordResult; action.matchGeneration = flow.generation; action.result = room::MatchResult::P1Win;
			if (!a.Send(action, &flow.idA) || !b.Send(action, &flow.idB)) return fail("result send failed");
			flow.step = 6; flow.since = now;
			return 0;
		case 6: {
			const int ra = settled(a, flow.idA, 20000, why), rb = settled(b, flow.idB, 20000, why);
			if (ra == 2 || rb == 2) return fail("result " + why);
			if (ra == 1 && rb == 1) { flow.step = 7; flow.since = now; }
			return 0;
		}
		default: {
			const bool idle = a.s->match->GetPhase() == MatchPhase::Idle && b.s->match->GetPhase() == MatchPhase::Idle;
			if (!flow.aQueued && a.matchEnded[tableId] == flow.generation && idle)
				flow.aQueued = a.s->peer.client->AcknowledgeTerminal(tableId, flow.generation) == session::SendResult::Queued;
			if (!flow.bQueued && b.matchEnded[tableId] == flow.generation && idle)
				flow.bQueued = b.s->peer.client->AcknowledgeTerminal(tableId, flow.generation) == session::SendResult::Queued;
			if (flow.aQueued && flow.bQueued && !a.View().terminalPending[tableId] && !b.View().terminalPending[tableId]) {
				a.s->match.reset(); b.s->match.reset();
				++st.matchesOk;
				Event(number, -1, "table " + std::to_string(tableId) + " match complete, score " +
					std::to_string(a.View().tables[tableId].score[0]) + "-" + std::to_string(a.View().tables[tableId].score[1]), false);
				return 1;
			}
			if (now - flow.since > 45000) return fail(std::string("not acknowledged in 45 s: ended ") + (a.matchEnded[tableId] == flow.generation ? "A" : "-") +
				(b.matchEnded[tableId] == flow.generation ? "B" : "-") + " queued " + (flow.aQueued ? "A" : "-") + (flow.bQueued ? "B" : "-") +
				(idle ? "" : " sessions not idle"));
			return 0;
		}
		}
	}
	}
	return 1;
}

void Room::TickFlows(Clock now) {
	for (std::uint8_t table = 0; table < flows.size(); ++table) {
		if (!flows[table]) continue;
		auto& flow = *flows[table];
		const int result = AdvanceFlow(flow, now);
		if (result == 0) continue;
		if (result == 2) {
			static const char* const names[] = {"probe_a_to_b", "probe_b_to_a", "ready_send", "ready_reply", "game_start", "game_running", "result", "acknowledge"};
			FailFlow(flow, flow.kind == Flow::Kind::Match ? names[(std::min)(flow.step, 7)] : "step" + std::to_string(flow.step), flow.detail);
		}
		EndFlow(table);
	}
}

// ---- Room ----------------------------------------------------------------

// Every accepted line should reach every member that was in the room when it
// was said: not just the few that are timed. Each line is checked once per
// member, when it is 20 to 80 s old (a row is written every 60 s). The room
// drops a member's lines when they leave, so a line whose sender has since
// left (a planned rejoin counts) is not expected in any snapshot.
void Room::CheckChat(Clock now) {
	for (auto& member : members) {
		if (!member->IsActive()) continue;
		const auto& chat = member->View().chat;
		for (const auto& line : chatLog) {
			if (line.at + 80000 <= now || line.at + 20000 > now || line.at < member->activeSince + 10000) continue;
			if (!line.sender->IsActive() || line.sender->activeSince != line.senderSince) continue;
			++st.chatChecked;
			if (std::any_of(chat.begin(), chat.end(), [&](const room::ChatMessage& value) { return value.text == line.text; })) continue;
			++st.chatMissing;
			const bool senderHas = line.sender->IsActive() && line.sender->activeSince == line.senderSince &&
				std::any_of(line.sender->View().chat.begin(), line.sender->View().chat.end(), [&](const room::ChatMessage& value) { return value.text == line.text; });
			if (st.chatMissing <= 20) Event(number, member->index, "chat line missing from a member's snapshot " + std::to_string((now - line.at) / 1000) +
				" s after it was accepted: \"" + line.text.substr(0, 40) + "\" (length " + std::to_string(line.text.size()) + "), sender " +
				std::to_string(line.sender->index) + (senderHas ? " has it" : " does not have it") + ", this member sees " + std::to_string(chat.size()) +
				" lines, member joined " + std::to_string((now - member->activeSince) / 1000) + " s ago");
		}
	}
}

void Room::Sample(Clock now) {
	std::uint64_t newest = 0;
	for (auto& member : members) if (member->IsActive()) newest = (std::max)(newest, member->View().revision);
	for (auto& member : members) {
		if (!member->IsActive()) { member->behindSince = 0; continue; }
		const auto received = member->s->peer.client->RoomSnapshotReceivedMs();
		if (received) {
			const std::uint64_t age = now >= received ? now - received : 0;
			st.staleSum += age; ++st.staleCount; st.staleMax = (std::max)(st.staleMax, age);
		}
		if (member->View().revision < newest) { if (!member->behindSince) member->behindSince = now; }
		else member->behindSince = 0;
		const auto recovery = member->s->Room().RecoveryState();
		st.queueMax = (std::max<std::uint64_t>)(st.queueMax, recovery.clientQueueMessages + recovery.serverQueueMessages);
		st.queueBytesMax = (std::max<std::uint64_t>)(st.queueBytesMax, recovery.queuedBytes);
		st.stagedMax = (std::max<std::uint64_t>)(st.stagedMax, recovery.stagedCheckpoints);
		const auto& load = member->s->Room().HelperLoad();
		st.helperLagMaxMs = (std::max<std::uint64_t>)(st.helperLagMaxMs, load.actorTickLagMaxUs / 1000);
		if (load.lastEventMs) st.helperSilentMaxMs = (std::max<std::uint64_t>)(st.helperSilentMaxMs, now >= load.lastEventMs ? now - load.lastEventMs : 0);
	}
}

void Room::Tick(Clock now) {
	// Admissions are one at a time per room, the creator first.
	if (!joiner && !closing) {
		Member* next = nullptr;
		for (auto& member : members)
			if (member->phase == Member::Phase::WaitTurn && (member->index == 0 || creatorJoined) && (!next || member->index < next->index)) next = member.get();
		if (next) { joiner = next; next->turnGranted = true; }
	}
	if (joiner && joiner->phase != Member::Phase::WaitTurn && joiner->phase != Member::Phase::Joining &&
		joiner->phase != Member::Phase::Registering) joiner = nullptr;
	TickFlows(now);
	if (closing) return;

	// Chat round trips: a sampled message is seen when it is in another
	// member's snapshot.
	for (auto it = samples.begin(); it != samples.end();) {
		bool done = false;
		if (!it->observer->IsActive() || it->observer->activeSince != it->observerSince) done = true; // the observer's session ended
		else if (it->sender && (!it->sender->IsActive() || it->sender->activeSince != it->senderSince)) done = true; // the room dropped the leaver's lines
		else {
			for (const auto& chat : it->observer->View().chat)
				if (chat.text == it->text) {
					const auto rtt = now - it->sentAt;
					st.chatRtt.push_back(rtt); st.chatRttAll.push_back(rtt); ++st.chatSeen;
					done = true;
					break;
				}
			if (!done && now - it->sentAt > 30000) {
				++st.chatLost;
				// Where the line is, to tell a lost line from a late snapshot.
				const auto has = [&](Member* member) {
					if (!member->IsActive() || member->activeSince != (member == it->sender ? it->senderSince : it->observerSince)) return std::string("gone");
					for (const auto& chat : member->View().chat) if (chat.text == it->text) return std::string("yes");
					return std::string("no");
				};
				// Whether the host answered the send, and when: a line with no
				// accepted reply was never in the room to be seen.
				std::string reply = "none";
				for (const auto& line : chatLog)
					if (line.text == it->text) reply = "accepted " + std::to_string((now - line.at) / 1000) + " s ago";
				const auto& view = it->observer->View();
				Event(number, it->observer->index, "chat not seen within 30 s: " + it->text.substr(0, 24) + " | reply: " + reply + ", sender has it: " + has(it->sender) +
					", observer has it: " + has(it->observer) + ", observer chat lines " + std::to_string(view.chat.size()) + " revision " +
					std::to_string(view.revision) + " snapshot age " + std::to_string(it->observer->s->peer.client->RoomSnapshotReceivedMs() <= now ? now - it->observer->s->peer.client->RoomSnapshotReceivedMs() : 0) + " ms");
				done = true;
			}
		}
		it = done ? samples.erase(it) : it + 1;
	}

	if (now >= nextSampleAt) { nextSampleAt = now + 1000; Sample(now); }

	// A match at table 0 between its two fighters, when both are free.
	if (now >= nextMatchAt && ActiveCount() >= 2) {
		nextMatchAt = now + Seconds(options.matchEvery * 0.5, options.matchEvery * 1.5);
		Member* anyone = nullptr;
		for (auto& member : members) if (member->IsActive()) { anyone = member.get(); break; }
		const auto& table = anyone->View().tables[0];
		Member* a = ByMemberId(table.p1);
		Member* b = ByMemberId(table.p2);
		if (a && b && !flows[0] && !a->busy && !b->busy && table.phase == room::TablePhase::Waiting && !table.ready[0] && !table.ready[1] &&
			table.spectators.empty() && table.queue.empty() && !a->View().localTerminalPending && !b->View().localTerminalPending)
			StartFlow(Flow::Kind::Match, 0, a, b, now);
		else nextMatchAt = now + 20000; // the fighters are not both seated and free yet
	}

	// Every few minutes one member leaves and rejoins.
	if (now >= nextRejoinAt && ActiveCount() >= 3) {
		nextRejoinAt = now + Seconds(options.rejoinEvery * 0.5, options.rejoinEvery * 1.5);
		std::vector<Member*> candidates;
		for (auto& member : members)
			if (member->IsActive() && !member->busy && !member->forceRejoin && now - member->activeSince > 30000) candidates.push_back(member.get());
		if (!candidates.empty()) {
			auto* chosen = candidates[Uniform(0, candidates.size() - 1)];
			chosen->planned = true;
			chosen->BeginLeave("planned rejoin", Seconds(2, 10));
		}
	}
}

// One room, on its own thread: every member of a real room runs on a PC of its
// own, and a passive replica imports every commit, so a single thread for many
// rooms would be slower than the people it stands in for. Also ends the room
// cleanly (every member out, every helper ended) when the time is up.
void RoomMain(Room* roomPtr, Clock endAt, std::ofstream* csv, std::uint64_t seed) {
	Room& room = *roomPtr;
	rng.seed(seed + 7919ull * room.number);
	Clock nextRow = runStart + 60000;
	room.lastRowAt = room.cpuAt = runStart;
	while (!stopRequested && Now() < endAt) {
		const auto began = Now();
		for (auto& member : room.members) member->Tick(began);
		room.Tick(began);
		room.activeNow = room.ActiveCount();
		if (began >= nextRow) {
			nextRow += 60000;
			WriteRow(*csv, room, began);
		}
		const auto spent = Now() - began;
		room.loop.sum += spent; ++room.loop.count; room.loop.max = (std::max<std::uint64_t>)(room.loop.max, spent);
		Sleep(spent < 5 ? static_cast<DWORD>(5 - spent) : 1);
	}
	room.closing = true;
	for (auto& flow : room.flows) if (flow) {
		// A match cut off by the end of the run is neither a success nor a failure.
		if (flow->kind == Flow::Kind::Match) --room.st.matchesStarted;
		flow->a->busy = flow->b->busy = false;
		flow.reset();
	}
	for (auto& member : room.members) member->BeginLeave("shutdown", 0);
	const Clock stopBy = Now() + 40000;
	for (;;) {
		bool pendingLeave = false;
		const auto now = Now();
		for (auto& member : room.members) if (member->phase != Member::Phase::Down) { member->Tick(now); pendingLeave = true; }
		if (!pendingLeave || Now() > stopBy) break;
		Sleep(5);
	}
	room.activeNow = 0;
	WriteRow(*csv, room, Now());
}

} } }
