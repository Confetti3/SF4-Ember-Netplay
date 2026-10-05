#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"

namespace sf4e { namespace room {
using namespace detail;


RoomAuthority::RoomAuthority(std::string name, std::uint8_t capacity, std::uint64_t roomEpoch, Rules defaults) {
	snapshot_.protocolVersion = ProtocolVersion;
	snapshot_.roomEpoch = roomEpoch == 0 ? 1 : roomEpoch;
	snapshot_.name = (!name.empty() && name.size() <= MaximumRoomNameBytes && IsValidUtf8(name) && IsSingleLineText(name))
		? std::move(name) : std::string("Private room");
	snapshot_.capacity = InRange<std::uint8_t>(capacity, 2, static_cast<std::uint8_t>(MaximumMembers))
		? capacity : static_cast<std::uint8_t>(MaximumMembers);
	if (!ValidRules(defaults)) defaults = Rules();
	for (std::size_t i = 0; i < TableCount; ++i) { snapshot_.tables[i].id = static_cast<std::uint8_t>(i); snapshot_.tables[i].rules = defaults; }
}

void RoomAuthority::NoteActive(MemberId member) {
	// Only a member still in the room: an accepted Leave must not leave one
	// behind, so the map stays bounded by the roster.
	if (!Find(member)) return;
	lastActiveMs_[member] = recoveryPaused_ ? 0 : nowMs_;
}

std::uint32_t RoomAuthority::IdleSeconds(MemberId member) const {
	const auto found = lastActiveMs_.find(member);
	if (found == lastActiveMs_.end()) return 0;
	const std::uint64_t ms = recoveryPaused_ ? found->second : (nowMs_ >= found->second ? nowMs_ - found->second : 0);
	return static_cast<std::uint32_t>((std::min)(ms / 1000, static_cast<std::uint64_t>(MaximumIdleSeconds)));
}

Snapshot RoomAuthority::SnapshotFor(MemberId member) const {
	Snapshot result = snapshot_;
	result.localMember = Find(member) ? member : 0;
	for (auto& item : result.members) item.idleSeconds = IdleSeconds(item.id);
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

template <typename Visit> void RoomAuthority::ForEachTableTimer(Visit&& visit, bool permits) {
	for (std::size_t i = 0; i < TableCount; ++i) {
		if (snapshot_.tables[i].resultPending) visit(resultPendingSince_[i], ResultDisputeTimeoutMs);
		if (snapshot_.tables[i].spectatorHold) visit(startHeldSince_[i], SpectatorStartHoldMs);
		if (permits && snapshot_.tables[i].permitGeneration) visit(permitHeldSince_[i], MaximumPermitWindowMs);
	}
}

namespace {
// `value` plus `elapsed`, held at `maximum`: an age past its deadline only
// needs to stay past it.
std::uint64_t AddCapped(std::uint64_t value, std::uint64_t elapsed, std::uint64_t maximum) {
	return value >= maximum || elapsed >= maximum - value ? maximum : value + elapsed;
}
}

void RoomAuthority::PauseForRecovery() {
    if (recoveryPaused_) return;
    ForEachTableTimer([&](std::uint64_t& since, std::uint64_t) { since = nowMs_ >= since ? nowMs_ - since : 0; });
    for (auto& entry : lastChatMs_)
        entry.second = nowMs_ >= entry.second ? nowMs_ - entry.second : 0;
    for (auto& entry : lastActiveMs_)
        entry.second = nowMs_ >= entry.second ? nowMs_ - entry.second : 0;
    recoveryPaused_ = true;
    // The permit ages were just measured on this process's clock, so they
    // age on from that time. A restored room's ages have no such time here.
    permitClockMs_ = nowMs_;
    permitAgesKept_ = false;
}

void RoomAuthority::AdvancePausedTimers(std::uint64_t elapsedMs) {
	if (!recoveryPaused_ || !elapsedMs) return;
	const auto add = [elapsedMs](std::uint64_t value, std::uint64_t maximum) { return AddCapped(value, elapsedMs, maximum); };
	ForEachTableTimer([&](std::uint64_t& age, std::uint64_t timeout) { age = add(age, timeout); }, false);
	for (auto& entry : lastChatMs_) entry.second = add(entry.second, 1000);
	for (auto& entry : lastActiveMs_) entry.second = add(entry.second, MaximumIdleSeconds * 1000ull);
}

void RoomAuthority::AgePermitHolds(std::uint64_t nowMs) {
	if (!recoveryPaused_) return;
	const auto since = [nowMs](std::uint64_t clockMs) { return clockMs && nowMs > clockMs ? nowMs - clockMs : 0; };
	for (std::size_t i = 0; i < TableCount; ++i) {
		const auto generation = snapshot_.tables[i].permitGeneration;
		if (!generation) continue;
		auto age = AddCapped(permitHeldSince_[i], since(permitClockMs_), MaximumPermitWindowMs);
		// Generations are never reused, so the same one is the same reservation.
		if (permitAgesKept_ && keptPermitAges_.tables[i].first == generation)
			age = (std::max)(age, AddCapped(keptPermitAges_.tables[i].second, since(keptPermitAges_.clockMs), MaximumPermitWindowMs));
		permitHeldSince_[i] = age;
	}
	permitAgesKept_ = false;
	permitClockMs_ = nowMs;
}

RoomAuthority::PermitAgeList RoomAuthority::PermitAges() const {
	// Restored and not yet aged: the kept ages are the newest local ones, and
	// the restored ones come back with the checkpoint.
	if (permitAgesKept_) return keptPermitAges_;
	PermitAgeList ages;
	ages.clockMs = recoveryPaused_ ? permitClockMs_ : nowMs_;
	for (std::size_t i = 0; i < TableCount; ++i) {
		const auto generation = snapshot_.tables[i].permitGeneration;
		if (!generation) continue;
		const auto since = permitHeldSince_[i];
		ages.tables[i] = {generation, recoveryPaused_ ? since : nowMs_ >= since ? nowMs_ - since : 0};
	}
	return ages;
}

void RoomAuthority::KeepPermitAges(const PermitAgeList& ages) {
	if (!recoveryPaused_) return;
	keptPermitAges_ = ages;
	permitAgesKept_ = true;
}

void RoomAuthority::ResumeRecovery(std::uint64_t nowMs) {
    if (!recoveryPaused_) {
        if (nowMs >= nowMs_) nowMs_ = nowMs;
        return;
    }
	// Brings permit holds, and any ages kept across a restore, to now first.
	AgePermitHolds(nowMs);
	std::uint64_t rebasedNow = nowMs;
	ForEachTableTimer([&](std::uint64_t& age, std::uint64_t) { rebasedNow = (std::max)(rebasedNow, age); });
	for (auto& entry : lastChatMs_) {
		const auto age = entry.second;
		rebasedNow = (std::max)(rebasedNow, age);
	}
	for (auto& entry : lastActiveMs_) rebasedNow = (std::max)(rebasedNow, entry.second);
	ForEachTableTimer([&](std::uint64_t& age, std::uint64_t) { age = rebasedNow - age; });
	for (auto& entry : lastChatMs_) entry.second = rebasedNow - entry.second;
	for (auto& entry : lastActiveMs_) entry.second = rebasedNow - entry.second;
	nowMs_ = rebasedNow;
    recoveryPaused_ = false;
    permitClockMs_ = 0;
    permitAgesKept_ = false;
}

Result RoomAuthority::TransferHost(MemberId actor, MemberId successor) {
    if (snapshot_.closed) return Reject(RejectReason::Closed);
    if (!IsHost(actor)) return Reject(RejectReason::NotHost);
    if (!Find(successor)) return Reject(RejectReason::UnknownMember);
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

bool RoomAuthority::ValidTable(std::uint8_t table) const { return table < TableCount; }

bool RoomAuthority::IsHost(MemberId member) const { return member != 0 && member == snapshot_.host; }

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

RoomAuthority::TerminalReceipt* RoomAuthority::FindTerminalReceipt(std::uint8_t table,
	std::uint64_t generation, MatchResult result, MemberId member) {
	return const_cast<TerminalReceipt*>(static_cast<const RoomAuthority*>(this)->FindTerminalReceipt(table, generation, result, member));
}

const RoomAuthority::TerminalReceipt* RoomAuthority::FindTerminalReceipt(std::uint8_t table,
	std::uint64_t generation, MatchResult result, MemberId member) const {
	for (const auto& receipt : terminalReceipts_) {
		if (receipt.table != table || receipt.generation != generation || receipt.result != result) continue;
		if (std::any_of(receipt.recipients.begin(), receipt.recipients.end(),
			[&](const TerminalRecipient& recipient) { return recipient.member == member; })) return &receipt;
	}
	return nullptr;
}

void RoomAuthority::RememberTerminalAck(const TerminalReceipt& receipt, const TerminalRecipient& recipient) {
	if (!recipient.member || !recipient.incarnation || recipient.endpoint.host.empty() || recipient.endpoint.user.empty()) return;
	const auto found = std::find_if(terminalAckTombstones_.begin(), terminalAckTombstones_.end(), [&](const TerminalAckTombstone& prior) {
		return prior.table == receipt.table && prior.generation == receipt.generation && prior.member == recipient.member &&
			prior.endpoint == recipient.endpoint && prior.incarnation == recipient.incarnation;
	});
	if (found != terminalAckTombstones_.end()) return;
	if (terminalAckTombstones_.size() >= MaximumTerminalAckTombstones) {
		const auto sameIdentity = [](const TerminalAckTombstone& left, const TerminalAckTombstone& right) {
			return left.table == right.table && left.member == right.member && left.endpoint == right.endpoint &&
				left.incarnation == right.incarnation;
		};
		const auto isLive = [&](const TerminalAckTombstone& value) {
			return std::any_of(snapshot_.members.begin(), snapshot_.members.end(), [&](const Member& member) {
				return member.id == value.member && member.connection == value.endpoint &&
					member.incarnation == value.incarnation;
			});
		};
		// Keep at least one durable confirmation for every currently live
		// authenticated incarnation.  Unrelated old members are the first
		// eviction candidates; if all slots are live, evict an older duplicate
		// for a recipient before dropping a different recipient's only proof.
		auto victim = std::find_if(terminalAckTombstones_.begin(), terminalAckTombstones_.end(),
			[&](const TerminalAckTombstone& value) { return !isLive(value); });
		if (victim == terminalAckTombstones_.end()) {
			victim = std::find_if(terminalAckTombstones_.begin(), terminalAckTombstones_.end(),
				[&](const TerminalAckTombstone& value) {
					return std::count_if(terminalAckTombstones_.begin(), terminalAckTombstones_.end(),
						[&](const TerminalAckTombstone& other) { return sameIdentity(other, value); }) > 1;
				});
		}
		if (victim == terminalAckTombstones_.end()) return;
		terminalAckTombstones_.erase(victim);
	}
	terminalAckTombstones_.push_back({receipt.table, receipt.generation, recipient.member, recipient.endpoint, recipient.incarnation});
}

bool RoomAuthority::StoreTerminalReceipt(std::uint8_t table, const Table& value, MatchResult result) {
	if (!value.p1 || !value.p2 || !value.matchGeneration) return true;
	for (const auto& prior : terminalReceipts_)
		if (prior.table == table && prior.generation == value.matchGeneration) return prior.result == result;
	for (auto it = terminalReceipts_.begin(); it != terminalReceipts_.end();) {
		if (it->acknowledged) {
			for (const auto& recipient : it->recipients) if (recipient.acknowledged) RememberTerminalAck(*it, recipient);
			it = terminalReceipts_.erase(it);
		} else ++it;
	}
	if (terminalReceipts_.size() >= MaximumTerminalReceipts) return false;
	TerminalReceipt receipt;
	receipt.table = table; receipt.generation = value.matchGeneration; receipt.result = result;
	const auto addRecipient = [&](MemberId member) {
		if (!member || std::any_of(receipt.recipients.begin(), receipt.recipients.end(),
			[&](const TerminalRecipient& prior) { return prior.member == member; })) return;
		TerminalRecipient recipient;
		recipient.member = member;
		if (const auto* current = Find(member)) {
			recipient.endpoint = current->connection;
			recipient.incarnation = current->incarnation;
		} else {
			const auto frozen = std::find_if(activeMatchRecipients_[table].begin(), activeMatchRecipients_[table].end(),
				[&](const TerminalRecipient& prior) { return prior.member == member; });
			if (frozen == activeMatchRecipients_[table].end()) return;
			recipient = *frozen;
			recipient.acknowledged = true;
		}
		receipt.recipients.push_back(std::move(recipient));
	};
	if (!activeMatchRecipients_[table].empty() && activeMatchRecipients_[table].front().member) {
		for (const auto& recipient : activeMatchRecipients_[table]) addRecipient(recipient.member);
	} else {
		addRecipient(value.p1); addRecipient(value.p2);
		for (const auto spectator : value.spectators) addRecipient(spectator);
	}
	if (receipt.recipients.size() < 2) return false;
	receipt.fighters = {receipt.recipients[0].member, receipt.recipients[1].member};
	receipt.acknowledged = std::all_of(receipt.recipients.begin(), receipt.recipients.end(),
		[](const TerminalRecipient& recipient) { return recipient.acknowledged; });
	terminalReceipts_.push_back(std::move(receipt));
	return true;
}

void RoomAuthority::RetireTerminalReceipts(MemberId member) {
	for (auto& receipt : terminalReceipts_) {
		for (auto& recipient : receipt.recipients)
			if (recipient.member == member) recipient.acknowledged = true;
		receipt.acknowledged = std::all_of(receipt.recipients.begin(), receipt.recipients.end(),
			[](const TerminalRecipient& recipient) { return recipient.acknowledged; });
	}
}

std::vector<MemberId> RoomAuthority::TerminalMembers(std::uint8_t table, std::uint64_t generation) const {
	for (const auto& receipt : terminalReceipts_)
		if (receipt.table == table && receipt.generation == generation) {
			std::vector<MemberId> members;
			for (const auto& recipient : receipt.recipients) members.push_back(recipient.member);
			return members;
		}
	return {};
}

std::vector<RoomAuthority::TerminalReplay> RoomAuthority::PendingTerminalEvents(MemberId member) const {
	std::vector<TerminalReplay> result;
	if (!member) return result;
	for (const auto& receipt : terminalReceipts_) {
		const auto recipient = std::find_if(receipt.recipients.begin(), receipt.recipients.end(),
			[&](const TerminalRecipient& value) { return value.member == member; });
		if (recipient == receipt.recipients.end() || recipient->acknowledged) continue;
		result.push_back({receipt.table, receipt.generation, receipt.result});
	}
	return result;
}

bool RoomAuthority::FightersOutstanding(const TerminalReceipt& receipt) {
	// Only the fighters hold the table. A spectator that is still retiring its
	// session keeps its own receipt row (replay, idempotence, its own Queue/
	// Watch/Ready gate) but cannot delay the next generation; BeginMatch leaves
	// it out of that generation instead.
	return !receipt.acknowledged &&
		std::any_of(receipt.recipients.begin(), receipt.recipients.end(), [&](const TerminalRecipient& recipient) {
			return !recipient.acknowledged &&
				(recipient.member == receipt.fighters[0] || recipient.member == receipt.fighters[1]);
		});
}

bool RoomAuthority::HasOutstandingTerminalReceipt(std::uint8_t table) const {
	return std::any_of(terminalReceipts_.begin(), terminalReceipts_.end(),
		[&](const TerminalReceipt& receipt) { return receipt.table == table && FightersOutstanding(receipt); });
}

std::vector<MemberId> RoomAuthority::MatchRoster(std::uint8_t table) const {
	std::vector<MemberId> roster;
	if (table >= TableCount) return roster;
	for (const auto& recipient : activeMatchRecipients_[table]) roster.push_back(recipient.member);
	return roster;
}

std::vector<MemberId> RoomAuthority::LiveMatchRoster(std::uint8_t table) const {
	const auto roster = MatchRoster(table);
	const Table* current = FindTable(table);
	if (!current || roster.size() < 2 || roster[0] != current->p1 || roster[1] != current->p2) return {};
	return roster;
}

bool RoomAuthority::HasOutstandingTerminalReceiptForMember(MemberId member) const {
	if (!member) return false;
	return std::any_of(terminalReceipts_.begin(), terminalReceipts_.end(),
		[&](const TerminalReceipt& receipt) {
			return !receipt.acknowledged && std::any_of(receipt.recipients.begin(), receipt.recipients.end(),
				[&](const TerminalRecipient& recipient) { return recipient.member == member && !recipient.acknowledged; });
		});
}

bool RoomAuthority::ValidRules(const Rules& rules) const {
	const auto format = static_cast<std::uint8_t>(rules.format);
	const bool validRounds = rules.roundCount == 1 || rules.roundCount == 3 || rules.roundCount == 5 ||
		rules.roundCount == 7 || rules.roundCount == 15 || rules.roundCount == 99;
	const bool validTime = rules.roundTime == 30 || rules.roundTime == 60 || rules.roundTime == 99 ||
		rules.roundTime == 300 || rules.roundTime == 9999;
	return (format == 0 || format == 1 || format == 2 || format == 3 || format == 5) &&
		static_cast<std::uint8_t>(rules.rotation) <= static_cast<std::uint8_t>(RotationMode::BothRotate) && validRounds && validTime;
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
	if (snapshot_.members.size() >= snapshot_.capacity) return Reject(RejectReason::RoomFull);
	if (snapshot_.locked && !host) return Reject(RejectReason::AdmissionLocked);
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
	// A room bound to a tournament match admits only its two fighters.
	if (snapshot_.tournament.Active() && snapshot_.tournament.SlotOf(connection.user) < 0)
		return Reject(RejectReason::Unauthorized);
	if (nextMemberId_ == 0 || nextMemberId_ == (std::numeric_limits<MemberId>::max)()) return Reject(RejectReason::RoomFull);
	Member member;
	member.id = nextMemberId_++;
	member.name = name;
	member.mainFighter = profile.mainFighter;
	member.link = NetworkLinkFromWire(static_cast<long long>(profile.link));
	member.nat = NatClassFromWire(static_cast<long long>(profile.nat));
	member.connection = connection;
	member.host = snapshot_.host == 0 || host;
	member.joinOrder = nextJoinOrder_++;
	if (member.host) snapshot_.host = member.id;
	snapshot_.members.push_back(member);
	NoteActive(member.id);
	if (snapshot_.serverOwned) RememberAccount(member.id, profile.account);
	if (snapshot_.tournament.Active()) SeatBoundFighters();
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

void RoomAuthority::ClearReadiness(Table& table) {
	table.ready[0] = table.ready[1] = false;
	for (const MemberId seated : {table.p1, table.p2})
		if (auto* fighter = Find(seated)) fighter->delayLocked = false;
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
		bool ancillaryChanged = false;
		if (table.p1 == member) { table.p1 = 0; table.ready[0] = false; changed = true; }
		if (table.p2 == member) { table.p2 = 0; table.ready[1] = false; changed = true; }
		const auto oldQueueSize = table.queue.size();
		table.queue.erase(std::remove(table.queue.begin(), table.queue.end(), member), table.queue.end());
		ancillaryChanged = ancillaryChanged || oldQueueSize != table.queue.size();
		if (!preserveSpectator) {
			const auto oldSpectatorSize = table.spectators.size();
			table.spectators.erase(std::remove(table.spectators.begin(), table.spectators.end(), member), table.spectators.end());
			const auto oldNextSize = table.watchingNext.size();
			table.watchingNext.erase(std::remove(table.watchingNext.begin(), table.watchingNext.end(), member), table.watchingNext.end());
			const auto oldEndingSize = table.endingWatchers.size();
			table.endingWatchers.erase(std::remove(table.endingWatchers.begin(), table.endingWatchers.end(), member), table.endingWatchers.end());
			ancillaryChanged = ancillaryChanged || oldSpectatorSize != table.spectators.size() ||
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
			table.score[0] = table.score[1] = 0;
			if (live) CloseLiveGeneration(table);
			KeepStreak(table);
			// A departed seat always leaves a vacancy. Clear the transient phase
			// before filling it so Ready/Playing tables cannot remain Paused with
			// one stale fighter after a disconnect or explicit Leave.
			table.phase = TablePhase::Idle;
			Touch(table);
			SeatQueued(table);
			NormalizeTableMembers(table);
		} else if (ancillaryChanged) Touch(table);
	}
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
		const Member* successor = nullptr;
		for (const auto& other : snapshot_.members) {
			if (other.id == member) continue;
			if (!successor || other.joinOrder < successor->joinOrder) successor = &other;
		}
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
		// The final start gate: a bound table starts only inside its permit's window.
		(BoundTable(*table) && (!table->permitGeneration || PermitStartPassed(*table, nowMs_))) ||
		nextMatchGeneration_ == (std::numeric_limits<std::uint64_t>::max)()) return Reject(table ? RejectReason::WrongPhase : RejectReason::UnknownTable);
	// A bound table plays the generation its permit names.
	table->matchGeneration = table->permitGeneration ? table->permitGeneration : nextMatchGeneration_++;
	ClearPermit(*table);
	table->phase = TablePhase::Playing;
	table->resultPending = false;
	resultReporter_[tableId] = 0;
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
	if (result == MatchResult::P1Win) ++table->score[0];
	else if (result == MatchResult::P2Win) ++table->score[1];
	const auto generationValue = table->matchGeneration;
	CloseLiveGeneration(*table);
	// With no set length the same pair stays seated until one leaves. A
	// first-to-N table ends the set when a win reaches N and rotates its seats.
	// Draw/cancel/abort preserve prior wins and never award a point.
	const auto goal = static_cast<std::uint32_t>(table->rules.format);
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

bool RoomAuthority::HasDueTimerTransition(std::uint64_t nowMs) const {
	for (std::size_t i = 0; i < TableCount; ++i) {
		const auto& table = snapshot_.tables[i];
		if (table.spectatorHold && TimerDue(startHeldSince_[i], SpectatorStartHoldMs, nowMs)) return true;
		if (PermitCalledOff(table, nowMs)) return true;
		if (table.resultPending && table.phase == TablePhase::Playing &&
			TimerDue(resultPendingSince_[i], ResultDisputeTimeoutMs, nowMs)) return true;
	}
	return false;
}

bool RoomAuthority::TimerDue(std::uint64_t since, std::uint64_t timeout, std::uint64_t nowMs) const {
	// A paused authority holds ages rather than start times.
	if (recoveryPaused_) return since >= timeout;
	return (std::max)(nowMs, nowMs_) - since >= timeout;
}

bool RoomAuthority::PermitStartPassed(const Table& table, std::uint64_t nowMs) const {
	if (!table.permitGeneration) return false;
	std::uint64_t window = MaximumPermitWindowMs;
	bool held = false;
	for (std::size_t seat = 0; seat < 2; ++seat) {
		if (table.permits[seat].empty()) continue;
		held = true;
		window = (std::min)(window, table.permitWindows[seat]);
	}
	if (!held) return false;
	return window <= PermitStartMarginMs || TimerDue(permitHeldSince_[table.id], window - PermitStartMarginMs, nowMs);
}

bool RoomAuthority::PermitCalledOff(const Table& table, std::uint64_t nowMs) const {
	return table.permitGeneration && ((PermitPending(table) && TimerDue(permitHeldSince_[table.id], PermitHoldMs, nowMs)) ||
		PermitStartPassed(table, nowMs));
}

std::vector<Event> RoomAuthority::AdvanceTime(std::uint64_t nowMs) {
	if (recoveryPaused_) {
		ResumeRecovery(nowMs);
	}
	nowMs = (std::max)(nowMs, nowMs_);
	nowMs_ = nowMs;
	// A fighter in a game is busy, not idle, and a member the room has no
	// activity for (after a checkpoint restore) counts from now.
	for (const auto& member : snapshot_.members)
		if (member.status == MemberStatus::Playing || !lastActiveMs_.count(member.id)) NoteActive(member.id);
	std::vector<Event> events;
	for (std::size_t i = 0; i < TableCount; ++i) {
		Table& table = snapshot_.tables[i];
		if (!table.resultPending || table.phase != TablePhase::Playing ||
			nowMs_ - resultPendingSince_[i] < ResultDisputeTimeoutMs) continue;
		table.phase = TablePhase::Paused;
		Touch(table);
		events.push_back(Event{Event::Kind::ResultDisputed, static_cast<std::uint8_t>(i), table.matchGeneration, resultReporter_[i], pendingResult_[i]});
	}
	// A permit that never reached both fighters, or whose window has passed,
	// calls the start off; they ready again, and a new generation needs a new
	// permit.
	for (auto& table : snapshot_.tables) {
		if (!PermitCalledOff(table, nowMs_)) continue;
		ClearReadiness(table);
		ClearPermit(table);
		table.spectatorHold = false;
		startHeldSince_[table.id] = 0;
		table.phase = table.p1 && table.p2 ? TablePhase::Waiting : TablePhase::Idle;
		Touch(table);
		NormalizeTableMembers(table);
		events.push_back(Event{Event::Kind::SnapshotChanged, table.id, 0, 0, MatchResult::Abort});
	}
	ReleaseHeldStarts(events);
	return events;
}

bool RoomAuthority::LockedSpectatorReturning(const Table& table) const {
	return std::any_of(table.spectators.begin(), table.spectators.end(), [&](MemberId spectator) {
		const auto* member = Find(spectator);
		return member && member->spectatorLocked && HasOutstandingTerminalReceiptForMember(spectator);
	});
}

bool RoomAuthority::ReleaseHeldStarts(std::vector<Event>& events) {
	bool changed = false;
	for (auto& table : snapshot_.tables) {
		if (!table.spectatorHold) continue;
		// Any other way out of Ready (Unready, a departed fighter, a closed
		// room) ends the hold without a start.
		const bool starting = table.phase == TablePhase::Ready;
		if (starting && LockedSpectatorReturning(table) && !TimerDue(startHeldSince_[table.id], SpectatorStartHoldMs, nowMs_)) continue;
		table.spectatorHold = false;
		startHeldSince_[table.id] = 0;
		Touch(table);
		changed = true;
		if (starting && !PermitPending(table) && !PermitStartPassed(table, nowMs_))
			events.push_back(Event{Event::Kind::MatchReady, table.id, table.matchGeneration, 0, MatchResult::Abort});
	}
	return changed;
}

// nlohmann serialization uses numeric enum values. The wire protocol wraps
// these values with its message type and version, so unknown action values are
// rejected by the authority rather than silently selecting a default.

} }
