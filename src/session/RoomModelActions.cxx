// Room model rules: RoomAuthority::Apply and one method per action kind.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"

namespace sf4e { namespace room {
using namespace detail;

Result RoomAuthority::ApplyClose(MemberId member) {
	if (!IsHost(member)) return Reject(RejectReason::NotHost);
	snapshot_.closed = true; snapshot_.locked = true;
	for (auto& table : snapshot_.tables) { table.phase = TablePhase::Closed; Touch(table); }
	TouchRoom();
	return Accept({Event{Event::Kind::RoomClosed, 0, 0, member, MatchResult::Abort}});
}

Result RoomAuthority::ApplySetCapacity(MemberId member, const Action& action) {
	if (!IsHost(member)) return Reject(RejectReason::NotHost);
	if (!InRange<std::uint8_t>(action.capacity, 2, static_cast<std::uint8_t>(MaximumMembers)) || action.capacity < snapshot_.members.size()) return Reject(RejectReason::InvalidCapacity);
	snapshot_.capacity = action.capacity; TouchRoom(); return Accept();
}

Result RoomAuthority::ApplyLock(MemberId member, const Action& action) {
	if (!IsHost(member)) return Reject(RejectReason::NotHost);
	snapshot_.locked = action.locked; TouchRoom(); return Accept();
}

Result RoomAuthority::ApplyKick(MemberId member, const Action& action) {
	if (!IsHost(member) || action.target == 0 || action.target == snapshot_.host) return Reject(RejectReason::Unauthorized);
	const auto* targetMember = Find(action.target);
	if (!targetMember) return Reject(RejectReason::UnknownMember);
	kicked_.insert(targetMember->connection);
	auto result = Leave(action.target);
	if (result.accepted && !snapshot_.closed) result.events.push_back(Event{Event::Kind::MemberRemoved, 0, 0, action.target, MatchResult::Abort});
	return result;
}

Result RoomAuthority::ApplyChat(MemberId member, const Action& action) {
	if (action.text.empty() || action.text.size() > MaximumChatBytes || !IsValidUtf8(action.text) || !IsSingleLineText(action.text) ||
		(lastChatMs_.count(member) && nowMs_ >= lastChatMs_[member] && nowMs_ - lastChatMs_[member] < 1000)) return Reject(RejectReason::InvalidChat);
	ChatMessage chat;
	chat.sequence = nextChatSequence_++;
	chat.sender = member;
	chat.text = action.text;
	snapshot_.chat.push_back(chat);
	if (snapshot_.chat.size() > MaximumChatMessages) snapshot_.chat.erase(snapshot_.chat.begin());
	lastChatMs_[member] = nowMs_;
	TouchRoom();
	return Accept({Event{Event::Kind::ChatMessage, 0, 0, member, MatchResult::Abort}});
}

Result RoomAuthority::ApplyRename(MemberId member, const Action& action) {
	if (!IsHost(member) || action.text.empty() || action.text.size() > MaximumRoomNameBytes || !IsValidUtf8(action.text) || !IsSingleLineText(action.text)) return Reject(RejectReason::Unauthorized);
	snapshot_.name = action.text; TouchRoom(); return Accept();
}

Result RoomAuthority::ApplyAcknowledgeTerminal(MemberId member, const Action& action) {
	if (!action.matchGeneration) return Reject(RejectReason::WrongGeneration);
	for (auto& receipt : terminalReceipts_) {
		if (receipt.table != action.table || receipt.generation != action.matchGeneration) continue;
		for (auto& recipient : receipt.recipients) {
			if (recipient.member != member) continue;
			const auto* current = Find(member);
			if (!current || current->incarnation != recipient.incarnation) return Reject(RejectReason::Unauthorized);
			if (recipient.acknowledged) return Accept();
			recipient.acknowledged = true;
			RememberTerminalAck(receipt, recipient);
			receipt.acknowledged = std::all_of(receipt.recipients.begin(), receipt.recipients.end(),
				[](const TerminalRecipient& value) { return value.acknowledged; });
			// An ACK is a durable room mutation. Advance the room revision so
			// checkpoint/rebind consumers observe the release and local action
			// IDs cannot collide with a subsequent Ready/Queue command.
			TouchRoom();
			return Accept();
		}
		return Reject(RejectReason::Unauthorized);
	}
	for (const auto& tombstone : terminalAckTombstones_) {
		if (tombstone.table != action.table || tombstone.generation != action.matchGeneration || tombstone.member != member) continue;
		const auto* current = Find(member);
		if (!current || !(current->connection == tombstone.endpoint) || current->incarnation != tombstone.incarnation)
			return Reject(RejectReason::Unauthorized);
		// The original receipt was already acknowledged by this exact
		// authenticated incarnation. Re-accepting the lost ACK is idempotent,
		// but no unknown generation can reach this path.
		return Accept();
	}
	return Reject(RejectReason::WrongGeneration);
}

Result RoomAuthority::ApplySetRules(MemberId member, const Action& action, Table* table) {
	if (!CanEditRules(member)) return Reject(RejectReason::NotHost);
	if (table->phase == TablePhase::Playing || table->phase == TablePhase::Ready || table->phase == TablePhase::Paused || !ValidRules(action.rules)) return Reject(RejectReason::InvalidRules);
	Rules normalized = action.rules;
	normalized.format = SetFormat::Unlimited;
	normalized.rotation = RotationMode::WinnerStays;
	table->rules = normalized; ResetTable(*table, false); return Accept();
}

Result RoomAuthority::ApplyQueue(MemberId member, Table* table) {
	if (HasOutstandingTerminalReceiptForMember(member)) return Reject(RejectReason::TerminalLedgerFull);
	if (IsTableMember(*table, member)) return table->p1 == member || table->p2 == member ? Reject(RejectReason::AlreadySeated) : Reject(RejectReason::AlreadyQueued);
	for (const auto& other : snapshot_.tables) {
		if (IsTableMember(other, member)) return Reject(RejectReason::AlreadySeated);
		if (std::find(other.queue.begin(), other.queue.end(), member) != other.queue.end()) return Reject(RejectReason::AlreadyQueued);
	}
	table->queue.push_back(member); SeatQueued(*table); Touch(*table);
	NormalizeMemberStatus(member); NormalizeMemberStatus(table->p1); NormalizeMemberStatus(table->p2);
	return Accept();
}

Result RoomAuthority::ApplyUnqueue(MemberId member, Table* table) {
	auto found = std::find(table->queue.begin(), table->queue.end(), member);
	if (found != table->queue.end()) {
		table->queue.erase(found); Touch(*table); NormalizeMemberStatus(member); return Accept();
	}
	// The same action is also the explicit way for a seated fighter to
	// leave a waiting table. Never mutate a frozen match: the match
	// authority owns teardown and the result/abort action must win the
	// race with a UI unseat request.
	const int seat = table->p1 == member ? 0 : table->p2 == member ? 1 : -1;
	if (seat < 0) return Reject(RejectReason::NotQueued);
	if (table->phase == TablePhase::Playing || table->phase == TablePhase::Paused) return Reject(RejectReason::WrongPhase);
	if (seat == 0) table->p1 = 0; else table->p2 = 0;
	table->ready[0] = table->ready[1] = false;
	table->score[0] = table->score[1] = 0;
	table->resultPending = false;
	table->phase = TablePhase::Idle;
	Touch(*table);
	NormalizeMemberStatus(member);
	SeatQueued(*table);
	NormalizeMemberStatus(table->p1);
	NormalizeMemberStatus(table->p2);
	return Accept();
}

Result RoomAuthority::ApplyWatch(MemberId member, Table* table) {
	if (HasOutstandingTerminalReceiptForMember(member)) return Reject(RejectReason::TerminalLedgerFull);
	if (IsTableMember(*table, member)) return Reject(RejectReason::AlreadySeated);
	for (const auto& other : snapshot_.tables) {
		if (std::find(other.queue.begin(), other.queue.end(), member) != other.queue.end()) return Reject(RejectReason::AlreadyQueued);
		if (other.p1 == member || other.p2 == member) return Reject(RejectReason::AlreadySeated);
	}
	if (table->spectators.size() + table->watchingNext.size() >= MaxSpectators) return Reject(RejectReason::WrongPhase);
	for (auto& other : snapshot_.tables) {
		const auto oldSpectatorSize = other.spectators.size();
		const auto oldNextSize = other.watchingNext.size();
		other.spectators.erase(std::remove(other.spectators.begin(), other.spectators.end(), member), other.spectators.end());
		other.watchingNext.erase(std::remove(other.watchingNext.begin(), other.watchingNext.end(), member), other.watchingNext.end());
		if (oldSpectatorSize != other.spectators.size() || oldNextSize != other.watchingNext.size()) Touch(other);
	}
	if (table->phase == TablePhase::Playing) table->watchingNext.push_back(member);
	else table->spectators.push_back(member);
	Touch(*table); NormalizeMemberStatus(member); return Accept();
}

Result RoomAuthority::ApplyUnwatch(MemberId member, Table* table) {
	auto found = std::find(table->spectators.begin(), table->spectators.end(), member);
	if (found != table->spectators.end()) table->spectators.erase(found);
	else {
		auto next = std::find(table->watchingNext.begin(), table->watchingNext.end(), member);
		if (next == table->watchingNext.end()) return Reject(RejectReason::NotWatching);
		table->watchingNext.erase(next);
	}
	Touch(*table); NormalizeMemberStatus(member); return Accept();
}

Result RoomAuthority::ApplyReadiness(MemberId member, const Action& action, Table* table, Member* item) {
	if (!IsParticipant(*table, member)) return Reject(RejectReason::NotSeated);
	if (action.kind == ActionKind::Ready && HasOutstandingTerminalReceiptForMember(member)) return Reject(RejectReason::TerminalLedgerFull);
	if (table->phase != TablePhase::Waiting && table->phase != TablePhase::Ready) return Reject(RejectReason::WrongPhase);
	const int seat = table->p1 == member ? 0 : 1;
	if (action.inputDelay > MaximumInputDelay) return Reject(RejectReason::Unauthorized);
	// Once Ready has captured a value, a second Ready cannot replace it.
	// Unready is the explicit unlock operation.
	if (action.kind == ActionKind::Ready && table->ready[seat]) return Reject(RejectReason::WrongPhase);
	// The native match may have ended before this room event reached one of
	// its frozen recipients. Keep the table in Waiting until every retained
	// recipient has acknowledged outcome persistence and socket/helper
	// retirement. This is the table-start backpressure boundary; it is
	// independent of the generic effect journal and therefore survives
	// compaction.
	if (action.kind == ActionKind::Ready && table->phase == TablePhase::Waiting &&
		table->ready[seat == 0 ? 1 : 0] && HasOutstandingTerminalReceipt(table->id))
		return Reject(RejectReason::TerminalLedgerFull);
	table->ready[seat] = action.kind == ActionKind::Ready;
	if (action.kind == ActionKind::Ready) {
		item->selectedDelay = action.inputDelay;
		item->frozenDelay = action.inputDelay;
		item->delayLocked = true;
		table->inputDelay[seat] = action.inputDelay;
	} else {
		item->delayLocked = false;
		table->inputDelay[seat] = item->selectedDelay;
	}
	table->phase = table->ready[0] && table->ready[1] ? TablePhase::Ready : TablePhase::Waiting;
	Touch(*table); NormalizeMemberStatus(member);
	if (table->phase == TablePhase::Ready) return Accept({Event{Event::Kind::MatchReady, table->id, table->matchGeneration, 0, MatchResult::Abort}});
	return Accept();
}

Result RoomAuthority::ApplyRecordResult(MemberId member, const Action& action, Table* table) {
	if (table->phase != TablePhase::Playing || !IsParticipant(*table, member)) return Reject(RejectReason::WrongPhase);
	if (action.matchGeneration != table->matchGeneration) return Reject(RejectReason::WrongGeneration);
	if (action.result != MatchResult::P1Win && action.result != MatchResult::P2Win && action.result != MatchResult::Draw) return Reject(RejectReason::WrongPhase);
	if (resultReporter_[table->id] == member)
		return pendingResult_[table->id] == action.result ? Reject(RejectReason::DuplicateResult) :
			Reject(RejectReason::WrongPhase);
	if (table->resultPending) {
		if (resultReporter_[table->id] == 0 && pendingResult_[table->id] == MatchResult::Abort) {
			resultReporter_[table->id] = member;
			pendingResult_[table->id] = action.result;
			resultPendingSince_[table->id] = nowMs_;
			Touch(*table);
			return Accept();
		}
		if (pendingResult_[table->id] != action.result) {
			table->phase = TablePhase::Paused; Touch(*table);
			return Accept({Event{Event::Kind::ResultDisputed, table->id, table->matchGeneration, member, action.result}});
		}
		return EndMatch(table->id, table->matchGeneration, action.result);
	}
	resultReporter_[table->id] = member;
	pendingResult_[table->id] = action.result;
	table->resultPending = true;
	resultPendingSince_[table->id] = nowMs_;
	Touch(*table);
	return Accept();
}

Result RoomAuthority::ApplyMatchFinished(MemberId member, const Action& action, Table* table) {
	if (!IsParticipant(*table, member) || (table->phase != TablePhase::Playing && table->phase != TablePhase::Paused) ||
		action.matchGeneration != table->matchGeneration) return Reject(RejectReason::WrongGeneration);
	if (table->resultPending) return Accept();
	// Native teardown is authoritative about the end of the fight, but it
	// does not invent a winner. Keep the frozen roster in place while the
	// two peers reconcile their result, and let the normal 30 second
	// deadline move the table to Paused if neither report arrives.
	table->resultPending = true;
	resultReporter_[table->id] = 0;
	pendingResult_[table->id] = MatchResult::Abort;
	resultPendingSince_[table->id] = nowMs_;
	Touch(*table);
	return Accept();
}

Result RoomAuthority::ApplyCancelResult(MemberId member, const Action& action, Table* table) {
	if (!IsHost(member) || (table->phase != TablePhase::Paused && table->phase != TablePhase::Playing) ||
		action.matchGeneration != table->matchGeneration) return Reject(RejectReason::Unauthorized);
	return EndMatch(table->id, table->matchGeneration, MatchResult::Cancel);
}

Result RoomAuthority::ApplyAbortMatch(MemberId member, const Action& action, Table* table) {
	if (!IsParticipant(*table, member) || (table->phase != TablePhase::Playing && table->phase != TablePhase::Paused) || action.matchGeneration != table->matchGeneration) return Reject(RejectReason::Unauthorized);
	return EndMatch(table->id, table->matchGeneration, MatchResult::Abort);
}

Result RoomAuthority::Apply(MemberId member, const Action& action) {
	if (!Find(member)) return Reject(RejectReason::UnknownMember);
	const auto prior = lastAcceptedActions_.find(member);
	const bool receiptScopedAction = action.kind == ActionKind::AcknowledgeTerminal ||
		action.kind == ActionKind::RecordResult || action.kind == ActionKind::MatchFinished;
	if (action.actionId != 0 && prior != lastAcceptedActions_.end()) {
		if (prior->second == action.actionId && !receiptScopedAction) {
			Result repeat;
			repeat.accepted = true;
			repeat.snapshot = snapshot_;
			return repeat;
		}
		// Terminal acknowledgements and match-lifecycle reports carry exact
		// authenticated table/generation identity. A lost reply may be retried
		// after another action advanced this generic watermark; only the exact
		// validation path below can accept it, and Accept() never lowers the newer
		// watermark.
		if (action.actionId < prior->second && !receiptScopedAction)
			return Reject(RejectReason::StaleRoom);
	}
	activeActionMember_ = member;
	activeActionId_ = action.actionId;
	if (action.protocolVersion != ProtocolVersion) return Reject(RejectReason::StaleRoom);
	if (snapshot_.closed && action.kind != ActionKind::Leave) return Reject(RejectReason::Closed);
	if (action.roomEpoch != snapshot_.roomEpoch) return Reject(RejectReason::StaleRoom);
	const bool roomAction = action.kind == ActionKind::SetCapacity || action.kind == ActionKind::Lock ||
		action.kind == ActionKind::Kick || action.kind == ActionKind::Close || action.kind == ActionKind::Rename ||
		action.kind == ActionKind::TransferHost;
	if (roomAction && action.revision != snapshot_.revision) return Reject(RejectReason::StaleRoom);
	const bool generationScopedUnwatch = action.kind == ActionKind::Unwatch && action.matchGeneration != 0;
	if (IsTableAction(action.kind) && action.kind != ActionKind::RecordResult && action.kind != ActionKind::MatchFinished && action.kind != ActionKind::CancelResult && action.kind != ActionKind::AbortMatch && action.kind != ActionKind::AcknowledgeTerminal && !generationScopedUnwatch) {
		const Table* table = FindTable(action.table);
		if (!table) return Reject(RejectReason::UnknownTable);
		if (action.tableRevision != table->revision) return Reject(RejectReason::StaleTable);
	}
	if (IsTableAction(action.kind) && (action.kind == ActionKind::RecordResult || action.kind == ActionKind::CancelResult)) {
		if (!FindTable(action.table)) return Reject(RejectReason::UnknownTable);
	}
	if (action.kind == ActionKind::Leave) return Leave(member);
	if (action.kind == ActionKind::Close) return ApplyClose(member);
	if (action.kind == ActionKind::TransferHost) return TransferHost(member, action.target);
	if (action.kind == ActionKind::SetCapacity) return ApplySetCapacity(member, action);
	if (action.kind == ActionKind::Lock) return ApplyLock(member, action);
	if (action.kind == ActionKind::Kick) return ApplyKick(member, action);
	if (action.kind == ActionKind::Chat) return ApplyChat(member, action);
	if (action.kind == ActionKind::Rename) return ApplyRename(member, action);
	Table* table = FindTable(action.table);
	if (!table) return Reject(RejectReason::UnknownTable);
	Member* item = Find(member);
	// Generic room effects may compact away the original MatchEnded/result
	// action. A retry from either frozen fighter is still authenticated by the
	// terminal receipt and must replay the committed outcome exactly once,
	// without rescoring or requiring the mutable table to remain Playing.
	if (action.matchGeneration != 0 &&
		(action.kind == ActionKind::RecordResult || action.kind == ActionKind::CancelResult || action.kind == ActionKind::AbortMatch)) {
		const bool validResult = action.kind != ActionKind::RecordResult ||
			(action.result == MatchResult::P1Win || action.result == MatchResult::P2Win || action.result == MatchResult::Draw);
		const MatchResult expected = action.kind == ActionKind::CancelResult ? MatchResult::Cancel :
			action.kind == ActionKind::AbortMatch ? MatchResult::Abort : action.result;
		if (validResult) {
			TerminalReceipt* receipt = nullptr;
			for (auto& candidate : terminalReceipts_) {
				const bool fighter = candidate.fighters[0] == member || candidate.fighters[1] == member;
				const bool authorizedCancel = action.kind == ActionKind::CancelResult && IsHost(member);
				if (candidate.table == action.table && candidate.generation == action.matchGeneration &&
					candidate.result == expected && (fighter || authorizedCancel)) {
					receipt = &candidate;
					break;
				}
			}
			if (receipt) {
				Result replay = Accept({Event{Event::Kind::MatchEnded, action.table, action.matchGeneration, 0, expected, true},
					Event{Event::Kind::SnapshotChanged, action.table, action.matchGeneration, 0, expected, true}});
				replay.terminalReplay = true;
				return replay;
			}
		}
	}
	if (action.kind == ActionKind::AcknowledgeTerminal) return ApplyAcknowledgeTerminal(member, action);
	if (generationScopedUnwatch && (table->phase != TablePhase::Playing && table->phase != TablePhase::Paused ||
		action.matchGeneration != table->matchGeneration)) return Reject(RejectReason::WrongGeneration);
	if (action.kind == ActionKind::SetRules) return ApplySetRules(member, action, table);
	if (action.kind == ActionKind::Queue) return ApplyQueue(member, table);
	if (action.kind == ActionKind::Unqueue) return ApplyUnqueue(member, table);
	if (action.kind == ActionKind::Watch) return ApplyWatch(member, table);
	if (action.kind == ActionKind::Unwatch) return ApplyUnwatch(member, table);
	if (action.kind == ActionKind::Ready || action.kind == ActionKind::Unready) return ApplyReadiness(member, action, table, item);
	if (action.kind == ActionKind::RecordResult) return ApplyRecordResult(member, action, table);
	if (action.kind == ActionKind::MatchFinished) return ApplyMatchFinished(member, action, table);
	if (action.kind == ActionKind::CancelResult) return ApplyCancelResult(member, action, table);
	if (action.kind == ActionKind::AbortMatch) return ApplyAbortMatch(member, action, table);
	return Reject(RejectReason::Unauthorized);
}

} }
