#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"
#include <limits>

namespace sf4e { namespace room {
using namespace detail;


RoomAuthority::RoomAuthority(std::string name, std::uint8_t capacity, std::uint64_t roomEpoch, Rules defaults, bool practice, DelayPolicy delayPolicy, RoomType arcade) {
	snapshot_.protocolVersion = ProtocolVersion;
	snapshot_.practice = practice;
	snapshot_.arcade = arcade;
	snapshot_.delayPolicy = delayPolicy;
	snapshot_.roomEpoch = roomEpoch == 0 ? 1 : roomEpoch;
	snapshot_.name = (!name.empty() && name.size() <= MaximumRoomNameBytes && IsValidUtf8(name) && IsSingleLineText(name))
		? std::move(name) : std::string("Private room");
	snapshot_.capacity = InRange<std::uint8_t>(capacity, 2, static_cast<std::uint8_t>(MaximumMembers))
		? capacity : static_cast<std::uint8_t>(MaximumMembers);
	if (!defaults.Valid()) defaults = Rules();
	for (std::size_t i = 0; i < TableCount; ++i) { snapshot_.tables[i].id = static_cast<std::uint8_t>(i); snapshot_.tables[i].rules = defaults; }
}

Snapshot RoomAuthority::SnapshotFor(MemberId member) const { return SnapshotFor(member, true); }

Snapshot RoomAuthority::SnapshotFor(MemberId member, bool withChat) const {
	// Room chat is most of a busy room's snapshot. Without it, every other
	// field is copied and the chat is left empty; a field added to Snapshot
	// must be added here too (RoomSnapshot checks both forms agree).
	Snapshot result;
	if (withChat) result = snapshot_;
	else {
		result.protocolVersion = snapshot_.protocolVersion;
		result.roomEpoch = snapshot_.roomEpoch;
		result.revision = snapshot_.revision;
		result.name = snapshot_.name;
		result.capacity = snapshot_.capacity;
		result.locked = snapshot_.locked;
		result.closed = snapshot_.closed;
		result.serverOwned = snapshot_.serverOwned;
		result.practice = snapshot_.practice;
		result.delayPolicy = snapshot_.delayPolicy;
		result.arcade = snapshot_.arcade;
		result.cabinets = snapshot_.cabinets;
		result.streaks = snapshot_.streaks;
		result.topSlot = snapshot_.topSlot;
		result.host = snapshot_.host;
		result.localMember = snapshot_.localMember;
		result.members = snapshot_.members;
		result.tables = snapshot_.tables;
		result.terminalPending = snapshot_.terminalPending;
		result.localTerminalPending = snapshot_.localTerminalPending;
		result.localTerminalGenerations = snapshot_.localTerminalGenerations;
		result.localMatchGenerationsSent = snapshot_.localMatchGenerationsSent;
		result.localMatchGenerations = snapshot_.localMatchGenerations;
		result.tournament = snapshot_.tournament;
	}
	result.localMember = Find(member) ? member : 0;
	for (auto& item : result.members) { item.idleSeconds = IdleSeconds(item.id); item.typingMs = TypingLeftMs(item.id); }
	for (std::size_t i = 0; i < TableCount; ++i) {
		if (readyWaitingFor_[i] && WaitingForReady(snapshot_.tables[i]) == readyWaitingFor_[i]) {
			const auto age = TimerAge(readyWaitingSince_[i]);
			result.tables[i].readyRemainingMs = static_cast<std::uint32_t>(age < ReadyTimeoutMs ? ReadyTimeoutMs - age : 1);
		}
		if (!snapshot_.tables[i].spectatorHold) continue;
		const auto age = TimerAge(startHeldSince_[i]);
		result.tables[i].holdRemainingMs = static_cast<std::uint32_t>(age < SpectatorStartHoldMs ? SpectatorStartHoldMs - age : 1);
	}
	result.terminalPending.fill(false);
	result.localTerminalPending = false;
	result.localTerminalGenerations.fill(0);
	for (const auto& receipt : terminalReceipts_) {
		if (receipt.acknowledged || receipt.table >= TableCount) continue;
		if (FightersOutstanding(receipt)) result.terminalPending[receipt.table] = true;
		if (std::any_of(receipt.recipients.begin(), receipt.recipients.end(),
			[&](const TerminalRecipient& recipient) { return recipient.member == member && !recipient.acknowledged; })) {
			result.localTerminalPending = true;
			result.localTerminalGenerations[receipt.table] = (std::max)(
				result.localTerminalGenerations[receipt.table], receipt.generation);
		}
	}
	// Always sent, even all zero, so a reader can tell "in no game" from a host
	// that does not say.
	result.localMatchGenerationsSent = true;
	result.localMatchGenerations.fill(0);
	for (std::size_t i = 0; i < TableCount; ++i)
		if (std::any_of(activeMatchRecipients_[i].begin(), activeMatchRecipients_[i].end(),
			[&](const TerminalRecipient& recipient) { return recipient.member == member; }))
			result.localMatchGenerations[i] = snapshot_.tables[i].matchGeneration;
	return result;
}

bool RoomAuthority::SetMemberFighter(MemberId member,int fighter,bool* withdrewOpponentReady) {
    if(withdrewOpponentReady)*withdrewOpponentReady=false;
    auto* value=Find(member);
    if(value)NoteActive(member);
    if(!value||fighter<0||fighter>=44||value->fighter==fighter)return false;
    const int previous=value->fighter;
    value->fighter=fighter;
    Table* table=FindTable(static_cast<std::uint8_t>(value->table));
    const bool seated=table&&(table->p1==member||table->p2==member);
    // A first fighter changes no matchup. A known one does: a Ready given
    // against the old fighter, ready already or still in flight under the
    // old table revision, must not start the game.
    if(previous<0||!seated){TouchRoom();return true;}
    const int seat=table->p1==member?0:1, other=1-seat;
    auto* opponent=Find(other==0?table->p1:table->p2);
    if(opponent&&!table->ready[seat]&&ReadyCancellable(*table,other)){
        ReleaseReady(*table,other,*opponent);
        SettleReadiness(*table,opponent->id);
        if(withdrewOpponentReady)*withdrewOpponentReady=true;
    } else Touch(*table);
    return true;
}

Result RoomAuthority::TransferHost(MemberId actor, MemberId successor) {
    if (snapshot_.closed) return Reject(RejectReason::Closed);
    if (!IsHost(actor)) return Reject(RejectReason::NotHost);
    if (!Find(successor)) return Reject(RejectReason::UnknownMember);
    if (IsWatcher(snapshot_, successor)) return Reject(RejectReason::Unauthorized);
    if (successor == actor) return Accept();
    for (auto& member : snapshot_.members) member.host = member.id == successor;
    snapshot_.host = successor;
    TouchRoom();
    return Accept();
}
Member* RoomAuthority::Find(MemberId member) {
	for (auto& item : snapshot_.members) if (item.id == member) return &item;
	return nullptr;
}

const Member* RoomAuthority::Find(MemberId member) const {
	for (const auto& item : snapshot_.members) if (item.id == member) return &item;
	return nullptr;
}

Table* RoomAuthority::FindTable(std::uint8_t table) {
	return ValidTable(table) ? &snapshot_.tables[table] : nullptr;
}

const Table* RoomAuthority::FindTable(std::uint8_t table) const {
	return ValidTable(table) ? &snapshot_.tables[table] : nullptr;
}

bool RoomAuthority::ValidTable(std::uint8_t table) const { return table < TablesInPlay(snapshot_); }

bool RoomAuthority::IsHost(MemberId member) const { return member != 0 && member == ActingHost(snapshot_); }

bool RoomAuthority::CanEditRules(MemberId member) const { return IsHost(member); }

bool RoomAuthority::IsTableMember(const Table& table, MemberId member) const {
	return table.p1 == member || table.p2 == member ||
	std::find(table.queue.begin(), table.queue.end(), member) != table.queue.end() ||
	std::find(table.spectators.begin(), table.spectators.end(), member) != table.spectators.end() ||
	std::find(table.watchingNext.begin(), table.watchingNext.end(), member) != table.watchingNext.end();
}

bool RoomAuthority::IsParticipant(const Table& table, MemberId member) const {
	return table.p1 == member || table.p2 == member;
}

Result RoomAuthority::Reject(RejectReason reason) {
	Result result;
	result.accepted = false;
	result.reason = reason;
	result.snapshot = snapshot_;
	activeActionMember_ = 0;
	activeActionId_ = 0;
	return result;
}

Result RoomAuthority::Accept(std::vector<Event> events) {
	RefreshReadyTimers();
	Result result;
	result.accepted = true;
	result.snapshot = snapshot_;
	result.events = std::move(events);
	// Leave removes the member and its history before accepting the action.
	// Its acknowledgment remains in the committed effect journal; recreating
	// a member-scoped dedup entry here would invalidate follower checkpoints.
	if (activeActionMember_ != 0 && activeActionId_ != 0 && Find(activeActionMember_)) {
		auto& watermark = lastAcceptedActions_[activeActionMember_];
		watermark = (std::max)(watermark, activeActionId_);
	}
	activeActionMember_ = 0;
	activeActionId_ = 0;
	return result;
}

void RoomAuthority::TouchRoom() {
	if (snapshot_.revision != (std::numeric_limits<std::uint64_t>::max)()) ++snapshot_.revision;
}

void RoomAuthority::Touch(Table& table) {
	if (table.revision != (std::numeric_limits<std::uint64_t>::max)()) ++table.revision;
	TouchRoom();
}

void RoomAuthority::SetMemberIncarnation(MemberId member, std::uint64_t incarnation) {
	if (!incarnation) return;
	if (auto* value = Find(member)) {
		if (value->incarnation != incarnation) {
			value->incarnation = incarnation;
			TouchRoom();
		}
	}
}

Result RoomAuthority::Join(const std::string& name, const ConnectionRef& connection, bool host, MemberProfile profile) {
	if(profile.mainFighter < -1 || profile.mainFighter >= 44) return Reject(RejectReason::Unauthorized);
	// No process hosts a server-owned room as a player: the first joiner is host.
	if (snapshot_.serverOwned) host = false;
	if (snapshot_.closed) return Reject(RejectReason::Closed);
	const auto admission = BoundJoin(connection, profile);
	if (admission == BoundAdmission::Refuse) return Reject(RejectReason::Unauthorized);
	// A member the binding names comes whatever the capacity or the lock (a
	// returning fighter always fits among the watchers); only the room's own
	// member limit holds.
	const bool named = admission == BoundAdmission::Named;
	if (snapshot_.members.size() >= (named ? MaximumMembers : snapshot_.capacity)) return Reject(RejectReason::RoomFull);
	if (snapshot_.locked && !host && !named) return Reject(RejectReason::AdmissionLocked);
	if (snapshot_.host == 0 && !host && !snapshot_.serverOwned) return Reject(RejectReason::Unauthorized);
	if (name.empty() || name.size() > 32 || !IsValidUtf8(name) || !IsSingleLineText(name)) return Reject(RejectReason::NameTaken);
	if (kicked_.count(connection)) return Reject(RejectReason::MemberKicked);
	if (snapshot_.serverOwned) {
		const auto refused = CheckServerOwnedJoin(profile);
		if (refused != RejectReason::None) return Reject(refused);
	}
	for (const auto& member : snapshot_.members) {
		if (member.name == name) return Reject(RejectReason::NameTaken);
		if (member.connection == connection) return Reject(RejectReason::UnknownMember);
	}
	if (host && snapshot_.host != 0) return Reject(RejectReason::Unauthorized);
	// A room bound to a tournament match admits only the endpoints it names.
	if (admission == BoundAdmission::Stranger) return Reject(RejectReason::Unauthorized);
	if (nextMemberId_ == 0 || nextMemberId_ == (std::numeric_limits<MemberId>::max)()) return Reject(RejectReason::RoomFull);
	Member member;
	member.id = nextMemberId_++;
	member.name = name;
	member.mainFighter = profile.mainFighter;
	member.link = NetworkLinkFromWire(static_cast<long long>(profile.link));
	member.nat = NatClassFromWire(static_cast<long long>(profile.nat));
	member.steamId = profile.steamId;
	member.country = profile.country;
	member.badge = profile.admission.badge;
	member.connection = connection;
	member.host = snapshot_.host == 0 || host;
	member.joinOrder = nextJoinOrder_++;
	if (member.host) snapshot_.host = member.id;
	snapshot_.members.push_back(member);
	NoteActive(member.id);
	if (snapshot_.serverOwned) RememberAccount(member.id, profile.admission.account);
	if (snapshot_.tournament.Active()) SettleBoundJoin(member.id);
	TouchRoom();
	return Accept({Event{Event::Kind::SnapshotChanged, 0, 0, member.id, MatchResult::Abort}});
}

void RoomAuthority::NormalizeMemberStatus(MemberId member) {
	Member* item = Find(member);
	if (!item) return;
	item->status = MemberStatus::Idle;
	item->table = -1;
	item->seat = -1;
	for (std::size_t i = 0; i < TableCount; ++i) {
		const Table& table = snapshot_.tables[i];
		if (table.p1 == member || table.p2 == member) {
			item->table = static_cast<std::int8_t>(i);
			item->seat = table.p1 == member ? 0 : 1;
			item->status = table.phase == TablePhase::Playing ? MemberStatus::Playing :
				(table.ready[item->seat] ? MemberStatus::Ready : MemberStatus::Seated);
			// A fighter in a game is not in Training, whatever they last said.
			if (item->status == MemberStatus::Playing) item->training = false;
			return;
		}
		if (std::find(table.queue.begin(), table.queue.end(), member) != table.queue.end()) {
			item->table = static_cast<std::int8_t>(i);
			item->status = MemberStatus::Queued;
			return;
		}
		if (std::find(table.watchingNext.begin(), table.watchingNext.end(), member) != table.watchingNext.end()) {
			item->table = static_cast<std::int8_t>(i);
			item->status = MemberStatus::WatchingNext;
			return;
		}
		if (std::find(table.spectators.begin(), table.spectators.end(), member) != table.spectators.end()) {
			// Only the roster frozen at the start watches the game; a spectator
			// left out of it (still retiring the last one) waits for the next.
			const auto roster = MatchRoster(static_cast<std::uint8_t>(i));
			const bool inGame = table.phase == TablePhase::Playing &&
				std::find(roster.begin(), roster.end(), member) != roster.end();
			item->table = static_cast<std::int8_t>(i);
			item->status = inGame ? MemberStatus::Watching : MemberStatus::WatchingNext;
			return;
		}
	}
}

// A phase change moves every watcher between Watching and Watching next, not
// only the fighters.
void RoomAuthority::NormalizeTableMembers(const Table& table) {
	NormalizeMemberStatus(table.p1); NormalizeMemberStatus(table.p2);
	for (const auto spectator : table.spectators) NormalizeMemberStatus(spectator);
	for (const auto watcher : table.watchingNext) NormalizeMemberStatus(watcher);
}

void RoomAuthority::FillVacancy(Table& table, int seat) {
	if (seat < 0 || seat > 1 || (seat == 0 ? table.p1 != 0 : table.p2 != 0) || table.queue.empty()) return;
	const MemberId member = table.queue.front();
	table.queue.erase(table.queue.begin());
	if (seat == 0) table.p1 = member; else table.p2 = member;
	for (auto& other : snapshot_.tables) {
		other.spectators.erase(std::remove(other.spectators.begin(), other.spectators.end(), member), other.spectators.end());
		other.watchingNext.erase(std::remove(other.watchingNext.begin(), other.watchingNext.end(), member), other.watchingNext.end());
	}
	Member* item = Find(member);
	if (item) { item->table = static_cast<std::int8_t>(table.id); item->seat = static_cast<std::int8_t>(seat); item->status = MemberStatus::Seated; }
}

void RoomAuthority::SeatQueued(Table& table) {
	if (table.phase == TablePhase::Playing || table.phase == TablePhase::Paused) return;
	FillVacancy(table, 0);
	FillVacancy(table, 1);
	if (table.p1 != 0 && table.p2 != 0 && table.phase == TablePhase::Idle) table.phase = TablePhase::Waiting;
}

void RoomAuthority::CompleteSet(Table& table, int winnerSeat) {
	table.lastSet.generation = table.matchGeneration;
	table.lastSet.p1 = table.p1;
	table.lastSet.p2 = table.p2;
	table.lastSet.score[0] = table.score[0];
	table.lastSet.score[1] = table.score[1];
	table.lastSet.winnerSeat = static_cast<std::uint8_t>(winnerSeat);
	const MemberId winner = table.lastSet.Winner();
	const MemberId loser = table.lastSet.Loser();
	if (table.streakHolder == winner) {
		if (table.streak != (std::numeric_limits<std::uint32_t>::max)()) ++table.streak;
	} else {
		table.streakHolder = winner;
		table.streak = 1;
	}
	table.score[0] = table.score[1] = 0;
	if (snapshot_.arcade) { ArcadeSetEnded(table, winner, loser); return; }
	// A tournament set has no queue to rotate: the bridge decides what comes
	// next, and the same two fighters stay seated by slot.
	if (BoundTable(table)) return;
	// The rotated fighters join the back of the queue, so an empty queue seats
	// them again for a new set. Only members who queued themselves are ahead of
	// them: nobody else is ever put in the queue.
	std::vector<MemberId> leaving;
	if (table.rules.rotation == RotationMode::WinnerStays) leaving = {loser};
	else if (table.rules.rotation == RotationMode::LoserStays) leaving = {winner};
	else leaving = {winner, loser};
	for (const auto member : leaving) {
		if (table.p1 == member) table.p1 = 0;
		if (table.p2 == member) table.p2 = 0;
		if (std::find(table.queue.begin(), table.queue.end(), member) == table.queue.end()) table.queue.push_back(member);
	}
}

void RoomAuthority::KeepStreak(Table& table) {
	if (table.streakHolder && table.streakHolder != table.p1 && table.streakHolder != table.p2) {
		table.streakHolder = 0;
		table.streak = 0;
	}
}

void RoomAuthority::ClearSeatReadiness(Table& table, int seat) {
	table.ready[seat] = false;
	table.lowerLag[seat] = false;
	if (auto* fighter = Find(seat == 0 ? table.p1 : table.p2)) fighter->delayLocked = false;
}

void RoomAuthority::ClearReadiness(Table& table) {
	ClearSeatReadiness(table, 0);
	ClearSeatReadiness(table, 1);
}

void RoomAuthority::CloseLiveGeneration(Table& table) {
	activeMatchRecipients_[table.id].clear();
	for (const auto watcher : table.watchingNext)
		if (std::find(table.spectators.begin(), table.spectators.end(), watcher) == table.spectators.end())
			table.spectators.push_back(watcher);
	table.watchingNext.clear();
	const auto ending = std::move(table.endingWatchers);
	table.endingWatchers.clear();
	for (const auto leaving : ending) {
		table.spectators.erase(std::remove(table.spectators.begin(), table.spectators.end(), leaving), table.spectators.end());
		NormalizeMemberStatus(leaving);
	}
}

void RoomAuthority::RemoveFromTable(MemberId member, bool preserveSpectator) {
	for (auto& table : snapshot_.tables) {
		bool changed = false;
		bool queueChanged = false;
		bool spectatorsChanged = false;
		if (table.p1 == member) { table.p1 = 0; table.ready[0] = false; changed = true; }
		if (table.p2 == member) { table.p2 = 0; table.ready[1] = false; changed = true; }
		const auto oldQueueSize = table.queue.size();
		table.queue.erase(std::remove(table.queue.begin(), table.queue.end(), member), table.queue.end());
		queueChanged = oldQueueSize != table.queue.size();
		if (!preserveSpectator) {
			const auto oldSpectatorSize = table.spectators.size();
			table.spectators.erase(std::remove(table.spectators.begin(), table.spectators.end(), member), table.spectators.end());
			const auto oldNextSize = table.watchingNext.size();
			table.watchingNext.erase(std::remove(table.watchingNext.begin(), table.watchingNext.end(), member), table.watchingNext.end());
			const auto oldEndingSize = table.endingWatchers.size();
			table.endingWatchers.erase(std::remove(table.endingWatchers.begin(), table.endingWatchers.end(), member), table.endingWatchers.end());
			spectatorsChanged = oldSpectatorSize != table.spectators.size() ||
				oldNextSize != table.watchingNext.size() || oldEndingSize != table.endingWatchers.size();
		}
		if (changed) {
			// Leave stored the game's abort receipt before this, so a live game is
			// over for everyone: close it the way EndMatch does.
			const bool live = table.phase == TablePhase::Playing || table.phase == TablePhase::Paused;
			ClearReadiness(table);
			ClearPermit(table);
			table.resultPending = false;
			resultReporter_[table.id] = 0;
			resultPendingSince_[table.id] = 0;
			// A bound table plays one set, so a fighter who drops rejoins at the
			// score it has played to. Only a casual table starts over.
			if (!BoundTable(table)) table.score[0] = table.score[1] = 0;
			if (live) CloseLiveGeneration(table);
			KeepStreak(table);
			// A departed seat always leaves a vacancy. Clear the transient phase
			// before filling it so Ready/Playing tables cannot remain Paused with
			// one stale fighter after a disconnect or explicit Leave.
			table.phase = TablePhase::Idle;
			Touch(table);
			SeatQueued(table);
			NormalizeTableMembers(table);
			if (snapshot_.arcade) ArcadeVacated(table, member);
		} else if (queueChanged) Touch(table);
		else if (spectatorsChanged) TouchSpectators(table);
	}
	if (snapshot_.arcade) StandUp(member);
	NormalizeMemberStatus(member);
}

void RoomAuthority::ResetTable(Table& table, bool clearScore) {
	ClearReadiness(table);
	table.resultPending = false;
	resultReporter_[table.id] = 0;
	resultPendingSince_[table.id] = 0;
	if (clearScore) table.score[0] = table.score[1] = 0;
	table.phase = table.p1 != 0 && table.p2 != 0 ? TablePhase::Waiting : TablePhase::Idle;
	Touch(table);
	NormalizeMemberStatus(table.p1);
	NormalizeMemberStatus(table.p2);
}

Result RoomAuthority::Leave(MemberId member) {
	Member* item = Find(member);
	if (!item) return Reject(RejectReason::UnknownMember);
	// Capture an abort receipt before removing an active fighter/spectator from
	// the mutable roster. SessionServer may receive Leave after the native
	// authority has already drained; the frozen receipt is then the durable
	// game_end/replay source for every remaining recipient.
	for (const auto& table : snapshot_.tables) {
		if ((table.phase == TablePhase::Playing || table.phase == TablePhase::Paused) &&
			(table.p1 == member || table.p2 == member)) {
			bool alreadyStored = false;
			for (const auto& receipt : terminalReceipts_)
				if (receipt.table == table.id && receipt.generation == table.matchGeneration) { alreadyStored = true; break; }
			if (!alreadyStored && !StoreTerminalReceipt(table.id, table, MatchResult::Abort)) return Reject(RejectReason::TerminalLedgerFull);
		}
	}
	// An explicit member retirement ends any retry authority for that member;
	// the native MatchAuthority owns the live abort/teardown notification while
	// the other frozen recipients still retain their receipt obligations.
	RetireTerminalReceipts(member);
	lastAcceptedActions_.erase(member);
	lastChatMs_.erase(member);
	lastActiveMs_.erase(member);
	typing_.erase(member);
	const bool wasHost = item->host || member == snapshot_.host;
	std::vector<Event> departureEvents;
	// A Ready table has no live game: its matchGeneration still names the last
	// finished one, whose receipt holds the real result. Leaving from Ready only
	// ends the start hold (ReleaseHeldStarts below).
	for (const auto& table : snapshot_.tables) {
		if ((table.p1 == member || table.p2 == member) &&
			(table.phase == TablePhase::Playing || table.phase == TablePhase::Paused)) {
			departureEvents.push_back(Event{Event::Kind::MatchEnded, table.id, table.matchGeneration, member, MatchResult::Abort, true});
		}
	}
	// Moderator ownership is a player-facing role. A graceful owner departure
	// hands it to the oldest remaining member; the technical coordination
	// leader is kept separately by SessionServer/helper and is never inferred
	// from this field.
	if (wasHost) {
		const Member* successor = HostSuccessor(member);
		if (successor) {
			snapshot_.host = successor->id;
			for (auto& other : snapshot_.members) other.host = other.id == successor->id;
		}
	}
	RemoveFromTable(member);
	// A finished set names members only while they are in the room.
	for (auto& table : snapshot_.tables) {
		if (table.lastSet.p1 == member) table.lastSet.p1 = 0;
		if (table.lastSet.p2 == member) table.lastSet.p2 = 0;
	}
	snapshot_.members.erase(std::remove_if(snapshot_.members.begin(), snapshot_.members.end(), [member](const Member& value) { return value.id == member; }), snapshot_.members.end());
	// A bound slot freed by this departure goes to its fighter if it is here.
	if (snapshot_.tournament.Active()) SeatBoundFighters();
	// A chat line names its sender by member id, which no longer resolves;
	// snapshot readers reject a sender outside the roster.
	snapshot_.chat.erase(std::remove_if(snapshot_.chat.begin(), snapshot_.chat.end(),
		[member](const ChatMessage& chat) { return chat.sender == member; }), snapshot_.chat.end());
	if (snapshot_.serverOwned) {
		ForgetAccount(member);
		if (snapshot_.members.empty()) ReopenEmptyRoom();
	}
	TouchRoom();
	if (wasHost && snapshot_.members.empty() && !snapshot_.serverOwned) {
		snapshot_.closed = true;
		snapshot_.locked = true;
		snapshot_.host = 0;
		for (auto& table : snapshot_.tables) { table.phase = TablePhase::Closed; table.ready[0] = table.ready[1] = false; Touch(table); }
		return Accept({Event{Event::Kind::RoomClosed, 0, 0, member, MatchResult::Abort}});
	}
	departureEvents.push_back(Event{Event::Kind::MemberRemoved, 0, 0, member, MatchResult::Abort});
	departureEvents.push_back(Event{Event::Kind::SnapshotChanged, 0, 0, member, MatchResult::Abort});
	ReleaseHeldStarts(departureEvents);
	return Accept(std::move(departureEvents));
}

Result RoomAuthority::BeginMatch(std::uint8_t tableId, MemberId p1, MemberId p2) {
	Table* table = FindTable(tableId);
	if (!table || table->p1 != p1 || table->p2 != p2 || p1 == 0 || p2 == 0 ||
		table->phase != TablePhase::Ready || table->spectatorHold || PermitPending(*table) || table->resultPending ||
		HasOutstandingTerminalReceipt(tableId) ||
		// A bound table begins only inside its permit's window, less the
		// margin its preparation takes; NativeStartExpired checks the window
		// again before the native start.
		(BoundTable(*table) && (!table->permitGeneration || PermitStartPassed(*table, PermitAge(*table)))) ||
		nextMatchGeneration_ == (std::numeric_limits<std::uint64_t>::max)()) return Reject(table ? RejectReason::WrongPhase : RejectReason::UnknownTable);
	// A bound table plays the generation its permit names, and its permit
	// timer runs on until the native start.
	table->matchGeneration = table->permitGeneration ? table->permitGeneration : nextMatchGeneration_++;
	auto timer = permits_.tables[tableId];
	timer.windowMs = PermitWindow(*table);
	ClearPermit(*table);
	if (timer.generation) permits_.tables[tableId] = timer;
	table->phase = TablePhase::Playing;
	table->resultPending = false;
	resultReporter_[tableId] = 0;
	gamesScored_[tableId] = 0;
	activeMatchRecipients_[tableId].clear();
	const auto freezeRecipient = [&](MemberId member) {
		if (!member || std::any_of(activeMatchRecipients_[tableId].begin(), activeMatchRecipients_[tableId].end(),
			[&](const TerminalRecipient& prior) { return prior.member == member; })) return;
		if (const auto* current = Find(member)) {
			TerminalRecipient recipient;
			recipient.member = member;
			recipient.endpoint = current->connection;
			recipient.incarnation = current->incarnation;
			activeMatchRecipients_[tableId].push_back(std::move(recipient));
		}
	};
	freezeRecipient(p1); freezeRecipient(p2);
	// Queue members become spectators for this match at the grant boundary.
	// This freezes the complete native roster before MatchAuthority emits its
	// prepare message while retaining FIFO order for any overflow.
	for (const auto queued : table->queue) {
		if (table->spectators.size() >= MaxSpectators) break;
		if (std::find(table->spectators.begin(), table->spectators.end(), queued) == table->spectators.end()) {
			table->spectators.push_back(queued);
			// Keep the queue entry. It remains the FIFO source for the next
			// vacancy, while this duplicate view is the frozen spectator grant
			// for the match that just began.
		}
	}
	for (const auto spectator : table->spectators)
		if (!HasOutstandingTerminalReceiptForMember(spectator)) freezeRecipient(spectator);
	Touch(*table);
	NormalizeTableMembers(*table);
	return Accept({Event{Event::Kind::MatchStarted, tableId, table->matchGeneration, 0, MatchResult::Abort}});
}

Result RoomAuthority::EndMatch(std::uint8_t tableId, std::uint64_t generation, MatchResult result) {
	Table* table = FindTable(tableId);
	if (!table) return Reject(RejectReason::UnknownTable);
	if ((table->phase != TablePhase::Playing && table->phase != TablePhase::Paused) || table->matchGeneration != generation) return Reject(RejectReason::WrongGeneration);
	if (table->phase == TablePhase::Paused && result != MatchResult::Cancel && result != MatchResult::Abort) return Reject(RejectReason::WrongPhase);
	// A duplicate native completion is an acknowledgement of the same durable
	// receipt. It must never award the score a second time or rotate the table
	// before the first teardown has been delivered.
	if (auto* existing = FindTerminalReceipt(tableId, generation, result, table->p1)) {
		Result replay = Accept({Event{Event::Kind::MatchEnded, tableId, generation, 0, result, true},
			Event{Event::Kind::SnapshotChanged, tableId, generation, 0, result, true}});
		replay.terminalReplay = true;
		return replay;
	}
	// Reserve the receipt before changing the score. A full unacknowledged
	// ledger is backpressure, so a failed EndMatch must leave the live table
	// untouched and retryable.
	if (!StoreTerminalReceipt(tableId, *table, result)) return Reject(RejectReason::TerminalLedgerFull);
	RecordWonGame(tableId, *table, result);
	if (result == MatchResult::P1Win) ++table->score[0];
	else if (result == MatchResult::P2Win) ++table->score[1];
	const auto generationValue = table->matchGeneration;
	CloseLiveGeneration(*table);
	// A game that ends, started or not, needs no permit timer.
	NativeStarted(tableId, generationValue);
	// With no set length the same pair stays seated until one leaves. A
	// first-to-N table ends the set when a win reaches N and rotates its seats.
	// Draw/cancel/abort preserve prior wins and never award a point.
	// ponytail: an Arcade room with no set length plays sets of one game, so its pairs still change.
	const auto goal = (std::max)(static_cast<std::uint32_t>(table->rules.format), snapshot_.arcade ? 1u : 0u);
	const int winnerSeat = result == MatchResult::P1Win ? 0 : result == MatchResult::P2Win ? 1 : -1;
	// Clear readiness while the fighters who readied are still seated: it also
	// unlocks their delays.
	ClearReadiness(*table);
	if (goal != 0 && winnerSeat >= 0 && table->score[winnerSeat] >= goal) CompleteSet(*table, winnerSeat);
	table->phase = table->p1 != 0 && table->p2 != 0 ? TablePhase::Waiting : TablePhase::Idle;
	table->resultPending = false;
	resultReporter_[tableId] = 0;
	resultPendingSince_[tableId] = 0;
	if (table->phase == TablePhase::Waiting || table->phase == TablePhase::Idle) SeatQueued(*table);
	KeepStreak(*table);
	Touch(*table);
	NormalizeTableMembers(*table);
	for (const auto queued : table->queue) NormalizeMemberStatus(queued);
	return Accept({Event{Event::Kind::MatchEnded, tableId, generationValue, 0, result, true}, Event{Event::Kind::SnapshotChanged, tableId, generationValue, 0, result, true}});
}

// nlohmann serialization uses numeric enum values. The wire protocol wraps
// these values with its message type and version, so unknown action values are
// rejected by the authority rather than silently selecting a default.

} }
