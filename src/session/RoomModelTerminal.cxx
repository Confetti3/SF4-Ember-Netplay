// Room model terminal receipts: who must acknowledge a finished game, and the
// rosters of the games under way.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"
#include <limits>

namespace sf4e { namespace room {
using namespace detail;

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

} }
