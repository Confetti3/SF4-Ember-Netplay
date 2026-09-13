#include "RoomModel.hxx"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sf4e { namespace room {
namespace {

template <typename T>
bool InRange(T value, T low, T high) { return value >= low && value <= high; }

bool IsValidUtf8(const std::string& text) {
	const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
	std::size_t i = 0;
	while (i < text.size()) {
		const unsigned char c = bytes[i++];
		if (c == 0) return false;
		if (c < 0x80) continue;
		std::size_t count = 0;
		std::uint32_t code = 0;
		std::uint32_t minimum = 0;
		if ((c & 0xe0) == 0xc0) { count = 1; code = c & 0x1f; minimum = 0x80; }
		else if ((c & 0xf0) == 0xe0) { count = 2; code = c & 0x0f; minimum = 0x800; }
		else if ((c & 0xf8) == 0xf0) { count = 3; code = c & 0x07; minimum = 0x10000; }
		else return false;
		if (i + count > text.size()) return false;
		for (std::size_t j = 0; j < count; ++j) {
			const unsigned char continuation = bytes[i++];
			if ((continuation & 0xc0) != 0x80) return false;
			code = (code << 6) | (continuation & 0x3f);
		}
		if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
	}
	return true;
}

bool IsSingleLineText(const std::string& text) {
	for (const unsigned char byte : text) if (byte < 0x20 || byte == 0x7f) return false;
	return true;
}

bool IsTableAction(ActionKind kind) {
	return kind == ActionKind::Queue || kind == ActionKind::Unqueue ||
		kind == ActionKind::Watch || kind == ActionKind::Unwatch ||
	kind == ActionKind::Ready || kind == ActionKind::Unready ||
	kind == ActionKind::SetRules || kind == ActionKind::RecordResult ||
		kind == ActionKind::MatchFinished || kind == ActionKind::CancelResult || kind == ActionKind::AbortMatch ||
		kind == ActionKind::AcknowledgeTerminal;
}

int ReadInt(const nlohmann::json& object, const char* key, int low, int high) {
	const auto& value = object.at(key);
	if (!value.is_number_integer()) throw std::invalid_argument("room integer field");
	if(value.is_number_unsigned()&&value.get<std::uint64_t>()>static_cast<std::uint64_t>(high))
		throw std::out_of_range("room integer field");
	const long long parsed = value.get<long long>();
	if (parsed < low || parsed > high) throw std::out_of_range("room integer field");
	return static_cast<int>(parsed);
}

std::uint64_t ReadU64(const nlohmann::json& object, const char* key) {
	const auto& value = object.at(key);
	if (!value.is_number_unsigned()) throw std::invalid_argument("room unsigned field");
	return value.get<std::uint64_t>();
}

std::uint64_t ReadU64Range(const nlohmann::json& object, const char* key, std::uint64_t maximum) {
	const auto value = ReadU64(object, key);
	if (value > maximum) throw std::out_of_range("room bounded unsigned field");
	return value;
}

std::string ReadText(const nlohmann::json& object, const char* key, std::size_t maximum, bool allowEmpty = true) {
	const auto& value = object.at(key);
	if (!value.is_string()) throw std::invalid_argument("room text field");
	const auto text = value.get<std::string>();
	if ((!allowEmpty && text.empty()) || text.size() > maximum || !IsValidUtf8(text) || !IsSingleLineText(text)) {
		throw std::invalid_argument("room text bounds");
	}
	return text;
}

template <typename T>
void ReadMemberList(const nlohmann::json& object, const char* key, std::vector<T>& value, std::size_t maximum) {
	const auto& input = object.at(key);
	if (!input.is_array() || input.size() > maximum) throw std::out_of_range("room member list bounds");
	input.get_to(value);
}

void ValidateMemberIds(const std::vector<MemberId>& ids) {
	std::set<MemberId> seen;
	for (const auto id : ids) if (id == 0 || !seen.insert(id).second) throw std::invalid_argument("room duplicate member");
}

} // namespace

RoomAuthority::RoomAuthority(std::string name, std::uint8_t capacity, std::uint64_t roomEpoch, Rules defaults) {
	snapshot_.protocolVersion = ProtocolVersion;
	snapshot_.roomEpoch = roomEpoch == 0 ? 1 : roomEpoch;
	snapshot_.name = (!name.empty() && name.size() <= MaximumRoomNameBytes && IsValidUtf8(name) && IsSingleLineText(name))
		? std::move(name) : std::string("Private room");
	snapshot_.capacity = InRange<std::uint8_t>(capacity, 2, static_cast<std::uint8_t>(MaximumMembers))
		? capacity : static_cast<std::uint8_t>(MaximumMembers);
	if (!ValidRules(defaults)) defaults = Rules();
	defaults.format = SetFormat::Unlimited;
	// Open-ended rooms never rotate a set. Keep the legacy field stable on the
	// wire so old clients cannot infer a queue rotation policy from it.
	defaults.rotation = RotationMode::WinnerStays;
	for (std::size_t i = 0; i < TableCount; ++i) { snapshot_.tables[i].id = static_cast<std::uint8_t>(i); snapshot_.tables[i].rules = defaults; }
}

Snapshot RoomAuthority::SnapshotFor(MemberId member) const {
	Snapshot result = snapshot_;
	result.localMember = Find(member) ? member : 0;
	result.terminalPending.fill(false);
	result.localTerminalPending = false;
	result.localTerminalGenerations.fill(0);
	for (const auto& receipt : terminalReceipts_) {
		if (receipt.acknowledged || receipt.table >= TableCount) continue;
		result.terminalPending[receipt.table] = true;
		if (std::any_of(receipt.recipients.begin(), receipt.recipients.end(),
			[&](const TerminalRecipient& recipient) { return recipient.member == member && !recipient.acknowledged; })) {
			result.localTerminalPending = true;
			result.localTerminalGenerations[receipt.table] = (std::max)(
				result.localTerminalGenerations[receipt.table], receipt.generation);
		}
	}
	return result;
}

bool RoomAuthority::SetMemberFighter(MemberId member,int fighter) {
    auto* value=Find(member);
    if(!value||fighter<0||fighter>=44||value->fighter==fighter)return false;
    value->fighter=fighter;TouchRoom();return true;
}

nlohmann::json RoomAuthority::Checkpoint() const {
    using nlohmann::json;
    // A checkpoint is taken between actions, never halfway through Accept().
    if (activeActionMember_ || activeActionId_) throw std::logic_error("room action in progress");
    json ages = json::array();
    for (std::size_t i = 0; i < TableCount; ++i) {
        const auto& table = snapshot_.tables[i];
        ages.push_back(!table.resultPending ? std::uint64_t(0) : recoveryPaused_
            ? resultPendingSince_[i] : nowMs_ >= resultPendingSince_[i] ? nowMs_ - resultPendingSince_[i] : 0);
    }
    std::map<MemberId, std::uint64_t> chatTimes = lastChatMs_;
    json terminalReceipts = json::array();
    for (const auto& receipt : terminalReceipts_) {
        json recipients = json::array();
        for (const auto& recipient : receipt.recipients)
            recipients.push_back({{"member", recipient.member}, {"endpoint", recipient.endpoint},
                {"incarnation", recipient.incarnation}, {"acknowledged", recipient.acknowledged}});
        terminalReceipts.push_back({{"table", receipt.table}, {"generation", receipt.generation},
            {"result", static_cast<int>(receipt.result)}, {"fighters", receipt.fighters},
            {"recipients", std::move(recipients)}, {"acknowledged", receipt.acknowledged}});
    }
    json terminalAckTombstones = json::array();
    for (const auto& tombstone : terminalAckTombstones_)
        terminalAckTombstones.push_back({{"table", tombstone.table}, {"generation", tombstone.generation},
            {"member", tombstone.member}, {"endpoint", tombstone.endpoint}, {"incarnation", tombstone.incarnation}});
    json activeRosters = json::array();
    for (const auto& roster : activeMatchRecipients_) {
        json entries = json::array();
        for (const auto& recipient : roster)
            entries.push_back({{"member", recipient.member}, {"endpoint", recipient.endpoint},
                {"incarnation", recipient.incarnation}, {"acknowledged", recipient.acknowledged}});
        activeRosters.push_back(std::move(entries));
    }
    // PauseForRecovery already converted these values to durations. An
    // imported paused replica has no source clock to subtract a second time.
    json state = {{"version", 2}, {"snapshot", snapshot_},
        {"next_member", nextMemberId_}, {"next_join", nextJoinOrder_},
        {"next_match", nextMatchGeneration_}, {"next_chat", nextChatSequence_},
        {"reporters", resultReporter_}, {"results", pendingResult_},
        {"result_since", recoveryPaused_ ? std::array<std::uint64_t, TableCount>{} : resultPendingSince_},
        {"result_age", ages}, {"recovery_paused", recoveryPaused_}, {"terminal_receipts", std::move(terminalReceipts)},
        {"terminal_ack_tombstones", std::move(terminalAckTombstones)},
        {"active_match_recipients", std::move(activeRosters)},
        {"chat_times", chatTimes}, {"actions", lastAcceptedActions_},
        {"kicked", kicked_}, {"time", recoveryPaused_ ? 0 : nowMs_}};
    state["snapshot"]["local_member"] = std::uint64_t(0);
    if (state.dump().size() > MaximumCheckpointBytes) throw std::length_error("room checkpoint too large");
    return state;
}

bool RoomAuthority::RestoreCheckpoint(const nlohmann::json& state) {
    try {
        using nlohmann::json;
        if (!state.is_object() || state.dump().size() > MaximumCheckpointBytes) return false;
        const auto version = ReadInt(state, "version", 1, 2);
        if (version != 1 && version != 2) return false;
        RoomAuthority restored("Recovering room");
        state.at("snapshot").get_to(restored.snapshot_);
        if (restored.snapshot_.localMember) return false;
        restored.nextMemberId_ = ReadU64(state, "next_member");
        restored.nextJoinOrder_ = ReadU64(state, "next_join");
        restored.nextMatchGeneration_ = ReadU64(state, "next_match");
        restored.nextChatSequence_ = ReadU64(state, "next_chat");
        restored.nowMs_ = ReadU64(state, "time");
        restored.recoveryPaused_ = version >= 2 && state.value("recovery_paused", false);
        if (!restored.nextMemberId_ || !restored.nextJoinOrder_ ||
            !restored.nextMatchGeneration_ || !restored.nextChatSequence_) return false;
        unsigned hosts = 0;
        for (const auto& member : restored.snapshot_.members) {
            if (member.id >= restored.nextMemberId_ || member.joinOrder >= restored.nextJoinOrder_) return false;
            if (member.host) { ++hosts; if (member.id != restored.snapshot_.host) return false; }
        }
        if (hosts != (restored.snapshot_.host ? 1u : 0u)) return false;
        for (const auto& chat : restored.snapshot_.chat) if (chat.sequence >= restored.nextChatSequence_) return false;
        for (const auto* key : {"reporters", "results", "result_since"})
            if (!state.at(key).is_array() || state.at(key).size() != TableCount) return false;
        for (std::size_t i = 0; i < TableCount; ++i) {
            const auto& table = restored.snapshot_.tables[i];
            if (table.matchGeneration >= restored.nextMatchGeneration_) return false;
            const auto reporter = state.at("reporters").at(i);
            const auto since = state.at("result_since").at(i);
            const auto result = state.at("results").at(i);
            if (!reporter.is_number_unsigned() || !since.is_number_unsigned() || !result.is_number_integer()) return false;
            restored.resultReporter_[i] = reporter.get<MemberId>();
            restored.resultPendingSince_[i] = since.get<std::uint64_t>();
            const auto parsedResult = result.get<long long>();
            if (parsedResult < 0 || parsedResult > static_cast<int>(MatchResult::Abort) ||
                restored.resultPendingSince_[i] > restored.nowMs_) return false;
            restored.pendingResult_[i] = static_cast<MatchResult>(parsedResult);
            if (version >= 2 && state.contains("result_age")) {
                const auto& age = state.at("result_age").at(i);
                if (!age.is_number_unsigned()) return false;
                if (restored.recoveryPaused_) restored.resultPendingSince_[i] = age.get<std::uint64_t>();
                else if (age.get<std::uint64_t>() > restored.nowMs_ && restored.resultPendingSince_[i] != 0) return false;
            }
            if (table.resultPending && restored.resultReporter_[i] &&
                !restored.IsParticipant(table, restored.resultReporter_[i])) return false;
        }
        const auto parseRecipient = [&](const json& row, TerminalRecipient& recipient, bool allowMissingMember) {
            if (!row.is_object() || !row.contains("member") || !row.contains("endpoint") || !row.contains("incarnation") ||
                !row.at("member").is_number_unsigned() || !row.at("incarnation").is_number_unsigned() ||
                (row.contains("acknowledged") && !row.at("acknowledged").is_boolean())) return false;
            recipient.member = row.at("member").get<MemberId>();
            recipient.endpoint = row.at("endpoint").get<ConnectionRef>();
            recipient.incarnation = row.at("incarnation").get<std::uint64_t>();
            recipient.acknowledged = row.value("acknowledged", false);
            if (!recipient.member || !recipient.incarnation) return false;
            const auto* current = restored.Find(recipient.member);
            if (!allowMissingMember && !current) return false;
            if (current && (!(current->connection == recipient.endpoint) || current->incarnation != recipient.incarnation)) return false;
            return true;
        };
        restored.activeMatchRecipients_ = {};
        const auto activeRosters = state.value("active_match_recipients", json::array());
        if (!activeRosters.is_array() && !activeRosters.is_null()) return false;
        if (activeRosters.is_array() && !activeRosters.empty() && activeRosters.size() != TableCount) return false;
        if (activeRosters.is_array()) {
            for (std::size_t i = 0; i < activeRosters.size(); ++i) {
                if (!activeRosters.at(i).is_array() || activeRosters.at(i).size() > MaximumMembers) return false;
                for (const auto& row : activeRosters.at(i)) {
                    TerminalRecipient recipient;
                    if (!parseRecipient(row, recipient, true) ||
                        std::any_of(restored.activeMatchRecipients_[i].begin(), restored.activeMatchRecipients_[i].end(),
                            [&](const TerminalRecipient& prior) { return prior.member == recipient.member; })) return false;
                    restored.activeMatchRecipients_[i].push_back(std::move(recipient));
                }
            }
        }
        restored.terminalReceipts_.clear();
        const auto receipts = state.value("terminal_receipts", json::array());
        if (!receipts.is_array() || receipts.size() > MaximumTerminalReceipts) return false;
        for (const auto& row : receipts) {
            TerminalReceipt receipt;
            receipt.table = static_cast<std::uint8_t>(ReadInt(row, "table", 0, static_cast<int>(TableCount - 1)));
            receipt.generation = ReadU64(row, "generation");
            if (!receipt.generation || receipt.generation >= restored.nextMatchGeneration_ ||
                restored.snapshot_.tables[receipt.table].matchGeneration < receipt.generation) return false;
            receipt.result = static_cast<MatchResult>(ReadInt(row, "result", 0, static_cast<int>(MatchResult::Abort)));
            if (row.contains("recipients")) {
                const auto& recipients = row.at("recipients");
                if (!recipients.is_array() || recipients.size() < 2 || recipients.size() > MaximumMembers) return false;
                for (const auto& recipientRow : recipients) {
                    TerminalRecipient recipient;
                    if (!parseRecipient(recipientRow, recipient, true) ||
                        std::any_of(receipt.recipients.begin(), receipt.recipients.end(),
                            [&](const TerminalRecipient& prior) { return prior.member == recipient.member; })) return false;
                    receipt.recipients.push_back(std::move(recipient));
                }
            } else {
                if (!row.at("fighters").is_array() || row.at("fighters").size() != 2 ||
                    !row.at("endpoints").is_array() || row.at("endpoints").size() != 2) return false;
                row.at("fighters").get_to(receipt.fighters);
                const auto endpoints = row.at("endpoints");
                for (std::size_t i = 0; i < 2; ++i) {
                    TerminalRecipient recipient;
                    recipient.member = receipt.fighters[i];
                    recipient.endpoint = endpoints.at(i).get<ConnectionRef>();
                    const auto* current = restored.Find(recipient.member);
                    if (!current || !(current->connection == recipient.endpoint)) return false;
                    recipient.incarnation = current->incarnation;
                    receipt.recipients.push_back(std::move(recipient));
                }
            }
            if (receipt.recipients.size() < 2) return false;
            receipt.fighters = {receipt.recipients[0].member, receipt.recipients[1].member};
            if (receipt.fighters[0] == receipt.fighters[1]) return false;
            // A completed fighter may have explicitly retired after native
            // teardown. Its frozen endpoint/incarnation remains the only
            // valid historical identity; an absent member is acceptable only
            // when Leave/retirement already acknowledged that recipient.
            for (const auto fighter : receipt.fighters) {
                const auto* current = restored.Find(fighter);
                const auto frozen = std::find_if(receipt.recipients.begin(), receipt.recipients.end(),
                    [&](const TerminalRecipient& recipient) { return recipient.member == fighter; });
                if (frozen == receipt.recipients.end() || !frozen->incarnation || frozen->endpoint.host.empty() || frozen->endpoint.user.empty()) return false;
                if (!current && !frozen->acknowledged) return false;
            }
            if (!restored.terminalReceipts_.empty()) {
                for (const auto& prior : restored.terminalReceipts_)
                    if (prior.table == receipt.table && prior.generation == receipt.generation) return false;
            }
            if (row.contains("acknowledged") && !row.at("acknowledged").is_boolean()) return false;
            receipt.acknowledged = std::all_of(receipt.recipients.begin(), receipt.recipients.end(),
                [](const TerminalRecipient& recipient) { return recipient.acknowledged; });
            restored.terminalReceipts_.push_back(std::move(receipt));
        }
        restored.terminalAckTombstones_.clear();
        const auto tombstones = state.value("terminal_ack_tombstones", json::array());
        if (!tombstones.is_array() || tombstones.size() > MaximumTerminalAckTombstones) return false;
        for (const auto& row : tombstones) {
            if (!row.is_object() || !row.contains("table") || !row.contains("generation") ||
                !row.contains("member") || !row.contains("endpoint") || !row.contains("incarnation") ||
                !row.at("table").is_number_unsigned() || !row.at("generation").is_number_unsigned() ||
                !row.at("member").is_number_unsigned() || !row.at("incarnation").is_number_unsigned()) return false;
            TerminalAckTombstone tombstone;
            const auto table = row.at("table").get<std::uint64_t>();
            tombstone.table = static_cast<std::uint8_t>(table);
            tombstone.generation = row.at("generation").get<std::uint64_t>();
            tombstone.member = row.at("member").get<MemberId>();
            tombstone.endpoint = row.at("endpoint").get<ConnectionRef>();
            tombstone.incarnation = row.at("incarnation").get<std::uint64_t>();
            if (table >= TableCount || !tombstone.generation || !tombstone.member ||
                tombstone.endpoint.host.empty() || tombstone.endpoint.user.empty() || !tombstone.incarnation ||
                std::any_of(restored.terminalAckTombstones_.begin(), restored.terminalAckTombstones_.end(),
                    [&](const TerminalAckTombstone& prior) {
                        return prior.table == tombstone.table && prior.generation == tombstone.generation &&
                            prior.member == tombstone.member && prior.endpoint == tombstone.endpoint &&
                            prior.incarnation == tombstone.incarnation;
                    })) return false;
            restored.terminalAckTombstones_.push_back(std::move(tombstone));
        }
        const auto readMap = [&](const char* key, std::map<MemberId, std::uint64_t>& values, bool time) {
            const auto& entries = state.at(key);
            // nlohmann encodes numeric-key maps as pairs, not JSON properties.
            if (!entries.is_array() || entries.size() > MaximumMembers) return false;
            for (const auto& entry : entries) {
                if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_unsigned() || !entry[1].is_number_unsigned()) return false;
                const auto member = entry[0].get<MemberId>(), value = entry[1].get<std::uint64_t>();
                if (!restored.Find(member) || (time && value > restored.nowMs_) || !values.emplace(member, value).second) return false;
            }
            return true;
        };
        if (!readMap("chat_times", restored.lastChatMs_, !restored.recoveryPaused_) || !readMap("actions", restored.lastAcceptedActions_, false)) return false;
        if (!state.at("kicked").is_array() || state.at("kicked").size() > MaximumCheckpointBytes / 8) return false;
        for (const auto& entry : state.at("kicked"))
            if (!restored.kicked_.insert(entry.get<ConnectionRef>()).second) return false;
        *this = std::move(restored);
        return true;
    } catch (const std::exception&) { return false; }
}

void RoomAuthority::PauseForRecovery() {
    if (recoveryPaused_) return;
    for (std::size_t i = 0; i < TableCount; ++i) {
        if (snapshot_.tables[i].resultPending)
            resultPendingSince_[i] = nowMs_ >= resultPendingSince_[i] ? nowMs_ - resultPendingSince_[i] : 0;
    }
    for (auto& entry : lastChatMs_)
        entry.second = nowMs_ >= entry.second ? nowMs_ - entry.second : 0;
    recoveryPaused_ = true;
}

void RoomAuthority::AdvancePausedTimers(std::uint64_t elapsedMs) {
	if (!recoveryPaused_ || !elapsedMs) return;
	const auto add = [elapsedMs](std::uint64_t value, std::uint64_t maximum) {
		return value >= maximum || elapsedMs >= maximum - value ? maximum : value + elapsedMs;
	};
	for (std::size_t i = 0; i < TableCount; ++i)
		if (snapshot_.tables[i].resultPending) resultPendingSince_[i] = add(resultPendingSince_[i], 30000);
	for (auto& entry : lastChatMs_) entry.second = add(entry.second, 1000);
}

void RoomAuthority::ResumeRecovery(std::uint64_t nowMs) {
    if (!recoveryPaused_) {
        if (nowMs >= nowMs_) nowMs_ = nowMs;
        return;
    }
	std::uint64_t rebasedNow = nowMs;
	for (std::size_t i = 0; i < TableCount; ++i) {
		if (snapshot_.tables[i].resultPending) {
			const auto age = resultPendingSince_[i];
			rebasedNow = (std::max)(rebasedNow, age);
		}
	}
	for (auto& entry : lastChatMs_) {
		const auto age = entry.second;
		rebasedNow = (std::max)(rebasedNow, age);
	}
	for (std::size_t i = 0; i < TableCount; ++i)
		if (snapshot_.tables[i].resultPending) resultPendingSince_[i] = rebasedNow - resultPendingSince_[i];
	for (auto& entry : lastChatMs_) entry.second = rebasedNow - entry.second;
	nowMs_ = rebasedNow;
    recoveryPaused_ = false;
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

bool RoomAuthority::HasOutstandingTerminalReceipt(std::uint8_t table) const {
	return std::any_of(terminalReceipts_.begin(), terminalReceipts_.end(),
		[&](const TerminalReceipt& receipt) { return receipt.table == table && !receipt.acknowledged; });
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

void RoomAuthority::SetLocalMember(MemberId member) { snapshot_.localMember = Find(member) ? member : 0; }

void RoomAuthority::SetMemberIncarnation(MemberId member, std::uint64_t incarnation) {
	if (!incarnation) return;
	if (auto* value = Find(member)) {
		if (value->incarnation != incarnation) {
			value->incarnation = incarnation;
			TouchRoom();
		}
	}
}

void RoomAuthority::SetRoomEpoch(std::uint64_t epoch) {
	if (epoch == 0 || epoch == snapshot_.roomEpoch) return;
	snapshot_.roomEpoch = epoch;
	TouchRoom();
}

Result RoomAuthority::Join(const std::string& name, const ConnectionRef& connection, bool host, int mainFighter) {
	if(mainFighter < -1 || mainFighter >= 44) return Reject(RejectReason::Unauthorized);
	if (snapshot_.closed) return Reject(RejectReason::Closed);
	if (snapshot_.members.size() >= snapshot_.capacity) return Reject(RejectReason::RoomFull);
	if (snapshot_.locked && !host) return Reject(RejectReason::AdmissionLocked);
	if (snapshot_.host == 0 && !host) return Reject(RejectReason::Unauthorized);
	if (name.empty() || name.size() > 32 || !IsValidUtf8(name) || !IsSingleLineText(name)) return Reject(RejectReason::NameTaken);
	if (kicked_.count(connection)) return Reject(RejectReason::MemberKicked);
	for (const auto& member : snapshot_.members) {
		if (member.name == name) return Reject(RejectReason::NameTaken);
		if (member.connection == connection) return Reject(RejectReason::UnknownMember);
	}
	if (host && snapshot_.host != 0) return Reject(RejectReason::Unauthorized);
	if (nextMemberId_ == 0 || nextMemberId_ == (std::numeric_limits<MemberId>::max)()) return Reject(RejectReason::RoomFull);
	Member member;
	member.id = nextMemberId_++;
	member.name = name;
	member.mainFighter = mainFighter;
	member.connection = connection;
	member.host = snapshot_.host == 0 || host;
	member.joinOrder = nextJoinOrder_++;
	if (member.host) snapshot_.host = member.id;
	snapshot_.members.push_back(member);
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
			item->table = static_cast<std::int8_t>(i);
			item->status = table.phase == TablePhase::Playing ? MemberStatus::Watching : MemberStatus::WatchingNext;
			return;
		}
	}
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
			ancillaryChanged = ancillaryChanged || oldSpectatorSize != table.spectators.size() || oldNextSize != table.watchingNext.size();
		}
		if (changed) {
			table.ready[0] = table.ready[1] = false;
			table.resultPending = false;
			resultReporter_[table.id] = 0;
			resultPendingSince_[table.id] = 0;
			table.score[0] = table.score[1] = 0;
			// A departed seat always leaves a vacancy. Clear the transient phase
			// before filling it so Ready/Playing tables cannot remain Paused with
			// one stale fighter after a disconnect or explicit Leave.
			table.phase = TablePhase::Idle;
			Touch(table);
			SeatQueued(table);
		} else if (ancillaryChanged) Touch(table);
	}
	NormalizeMemberStatus(member);
}

void RoomAuthority::ResetTable(Table& table, bool clearScore) {
	table.ready[0] = table.ready[1] = false;
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
	const bool wasHost = item->host || member == snapshot_.host;
	std::vector<Event> departureEvents;
	for (const auto& table : snapshot_.tables) {
		if ((table.p1 == member || table.p2 == member) &&
			(table.phase == TablePhase::Ready || table.phase == TablePhase::Playing || table.phase == TablePhase::Paused)) {
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
	snapshot_.members.erase(std::remove_if(snapshot_.members.begin(), snapshot_.members.end(), [member](const Member& value) { return value.id == member; }), snapshot_.members.end());
	TouchRoom();
	if (wasHost && snapshot_.members.empty()) {
		snapshot_.closed = true;
		snapshot_.locked = true;
		snapshot_.host = 0;
		for (auto& table : snapshot_.tables) { table.phase = TablePhase::Closed; table.ready[0] = table.ready[1] = false; Touch(table); }
		return Accept({Event{Event::Kind::RoomClosed, 0, 0, member, MatchResult::Abort}});
	}
	departureEvents.push_back(Event{Event::Kind::MemberRemoved, 0, 0, member, MatchResult::Abort});
	departureEvents.push_back(Event{Event::Kind::SnapshotChanged, 0, 0, member, MatchResult::Abort});
	return Accept(std::move(departureEvents));
}

Result RoomAuthority::BeginMatch(std::uint8_t tableId, MemberId p1, MemberId p2) {
	Table* table = FindTable(tableId);
	if (!table || table->p1 != p1 || table->p2 != p2 || p1 == 0 || p2 == 0 ||
		table->phase != TablePhase::Ready || table->resultPending || HasOutstandingTerminalReceipt(tableId) ||
		nextMatchGeneration_ == (std::numeric_limits<std::uint64_t>::max)()) return Reject(table ? RejectReason::WrongPhase : RejectReason::UnknownTable);
	table->matchGeneration = nextMatchGeneration_++;
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
	for (const auto spectator : table->spectators) freezeRecipient(spectator);
	Touch(*table);
	NormalizeMemberStatus(p1); NormalizeMemberStatus(p2);
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
	activeMatchRecipients_[tableId].clear();
	if (result == MatchResult::P1Win) ++table->score[0];
	else if (result == MatchResult::P2Win) ++table->score[1];
	const auto generationValue = table->matchGeneration;
	for (const auto watcher : table->watchingNext) table->spectators.push_back(watcher);
	table->watchingNext.clear();
	// Rooms are open-ended rematch tables. A result closes only the current
	// game; the same fighter pair remains seated until one explicitly leaves.
	// Draw/cancel/abort therefore preserve prior wins and never award a point.
	table->phase = table->p1 != 0 && table->p2 != 0 ? TablePhase::Waiting : TablePhase::Idle;
	table->ready[0] = table->ready[1] = false;
	if (auto* first = Find(table->p1)) first->delayLocked = false;
	if (auto* second = Find(table->p2)) second->delayLocked = false;
	table->resultPending = false;
	resultReporter_[tableId] = 0;
	resultPendingSince_[tableId] = 0;
	if (table->phase == TablePhase::Waiting || table->phase == TablePhase::Idle) SeatQueued(*table);
	Touch(*table);
	NormalizeMemberStatus(table->p1); NormalizeMemberStatus(table->p2);
	return Accept({Event{Event::Kind::MatchEnded, tableId, generationValue, 0, result, true}, Event{Event::Kind::SnapshotChanged, tableId, generationValue, 0, result, true}});
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
	if (action.kind == ActionKind::Close) {
		if (!IsHost(member)) return Reject(RejectReason::NotHost);
		snapshot_.closed = true; snapshot_.locked = true;
		for (auto& table : snapshot_.tables) { table.phase = TablePhase::Closed; Touch(table); }
		TouchRoom();
		return Accept({Event{Event::Kind::RoomClosed, 0, 0, member, MatchResult::Abort}});
	}
	if (action.kind == ActionKind::TransferHost) return TransferHost(member, action.target);
	if (action.kind == ActionKind::SetCapacity) {
		if (!IsHost(member)) return Reject(RejectReason::NotHost);
		if (!InRange<std::uint8_t>(action.capacity, 2, static_cast<std::uint8_t>(MaximumMembers)) || action.capacity < snapshot_.members.size()) return Reject(RejectReason::InvalidCapacity);
		snapshot_.capacity = action.capacity; TouchRoom(); return Accept();
	}
	if (action.kind == ActionKind::Lock) {
		if (!IsHost(member)) return Reject(RejectReason::NotHost);
		snapshot_.locked = action.locked; TouchRoom(); return Accept();
	}
	if (action.kind == ActionKind::Kick) {
		if (!IsHost(member) || action.target == 0 || action.target == snapshot_.host) return Reject(RejectReason::Unauthorized);
		const auto* targetMember = Find(action.target);
		if (!targetMember) return Reject(RejectReason::UnknownMember);
		kicked_.insert(targetMember->connection);
		auto result = Leave(action.target);
		if (result.accepted && !snapshot_.closed) result.events.push_back(Event{Event::Kind::MemberRemoved, 0, 0, action.target, MatchResult::Abort});
		return result;
	}
	if (action.kind == ActionKind::Chat) {
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
	if (action.kind == ActionKind::Rename) {
		if (!IsHost(member) || action.text.empty() || action.text.size() > MaximumRoomNameBytes || !IsValidUtf8(action.text) || !IsSingleLineText(action.text)) return Reject(RejectReason::Unauthorized);
		snapshot_.name = action.text; TouchRoom(); return Accept();
	}
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
	if (action.kind == ActionKind::AcknowledgeTerminal) {
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
	if (generationScopedUnwatch && (table->phase != TablePhase::Playing && table->phase != TablePhase::Paused ||
		action.matchGeneration != table->matchGeneration)) return Reject(RejectReason::WrongGeneration);
	if (action.kind == ActionKind::SetRules) {
		if (!CanEditRules(member)) return Reject(RejectReason::NotHost);
		if (table->phase == TablePhase::Playing || table->phase == TablePhase::Ready || table->phase == TablePhase::Paused || !ValidRules(action.rules)) return Reject(RejectReason::InvalidRules);
		Rules normalized = action.rules;
		normalized.format = SetFormat::Unlimited;
		normalized.rotation = RotationMode::WinnerStays;
		table->rules = normalized; ResetTable(*table, false); return Accept();
	}
	if (action.kind == ActionKind::Queue) {
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
	if (action.kind == ActionKind::Unqueue) {
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
	if (action.kind == ActionKind::Watch) {
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
	if (action.kind == ActionKind::Unwatch) {
		auto found = std::find(table->spectators.begin(), table->spectators.end(), member);
		if (found != table->spectators.end()) table->spectators.erase(found);
		else {
			auto next = std::find(table->watchingNext.begin(), table->watchingNext.end(), member);
			if (next == table->watchingNext.end()) return Reject(RejectReason::NotWatching);
			table->watchingNext.erase(next);
		}
		Touch(*table); NormalizeMemberStatus(member); return Accept();
	}
	if (action.kind == ActionKind::Ready || action.kind == ActionKind::Unready) {
		if (!IsParticipant(*table, member)) return Reject(RejectReason::NotSeated);
		if (action.kind == ActionKind::Ready && HasOutstandingTerminalReceiptForMember(member)) return Reject(RejectReason::TerminalLedgerFull);
		if (table->phase != TablePhase::Waiting && table->phase != TablePhase::Ready) return Reject(RejectReason::WrongPhase);
		const int seat = table->p1 == member ? 0 : 1;
		if (action.inputDelay > 10) return Reject(RejectReason::Unauthorized);
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
	if (action.kind == ActionKind::RecordResult) {
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
	if (action.kind == ActionKind::MatchFinished) {
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
	if (action.kind == ActionKind::CancelResult) {
		if (!IsHost(member) || (table->phase != TablePhase::Paused && table->phase != TablePhase::Playing) ||
			action.matchGeneration != table->matchGeneration) return Reject(RejectReason::Unauthorized);
		return EndMatch(table->id, table->matchGeneration, MatchResult::Cancel);
	}
	if (action.kind == ActionKind::AbortMatch) {
		if (!IsParticipant(*table, member) || (table->phase != TablePhase::Playing && table->phase != TablePhase::Paused) || action.matchGeneration != table->matchGeneration) return Reject(RejectReason::Unauthorized);
		return EndMatch(table->id, table->matchGeneration, MatchResult::Abort);
	}
	return Reject(RejectReason::Unauthorized);
}

std::vector<Event> RoomAuthority::AdvanceTime(std::uint64_t nowMs) {
	if (recoveryPaused_) {
		ResumeRecovery(nowMs);
	}
	nowMs = (std::max)(nowMs, nowMs_);
	nowMs_ = nowMs;
	std::vector<Event> events;
	for (std::size_t i = 0; i < TableCount; ++i) {
		Table& table = snapshot_.tables[i];
		if (!table.resultPending || table.phase != TablePhase::Playing ||
			nowMs_ - resultPendingSince_[i] < 30000) continue;
		table.phase = TablePhase::Paused;
		Touch(table);
		events.push_back(Event{Event::Kind::ResultDisputed, static_cast<std::uint8_t>(i), table.matchGeneration, resultReporter_[i], pendingResult_[i]});
	}
	return events;
}

// nlohmann serialization uses numeric enum values. The wire protocol wraps
// these values with its message type and version, so unknown action values are
// rejected by the authority rather than silently selecting a default.
void to_json(nlohmann::json& json, const Rules& value) { json = nlohmann::json{{"format", static_cast<int>(value.format)}, {"rotation", static_cast<int>(value.rotation)}, {"edition_select", value.editionSelect}, {"round_count", value.roundCount}, {"round_time", value.roundTime}}; }
void from_json(const nlohmann::json& json, Rules& value) {
	const auto format = ReadInt(json, "format", 0, 5);
	if (format != 0 && format != 1 && format != 2 && format != 3 && format != 5) throw std::invalid_argument("room set format");
	value.format = static_cast<SetFormat>(format);
	value.rotation = static_cast<RotationMode>(ReadInt(json, "rotation", 0, 2));
	if (!json.at("edition_select").is_boolean()) throw std::invalid_argument("room edition field");
	value.editionSelect = json.at("edition_select").get<bool>();
	const auto rounds = ReadInt(json, "round_count", 1, 99);
	if (rounds != 1 && rounds != 3 && rounds != 5 && rounds != 7 && rounds != 15 && rounds != 99) throw std::invalid_argument("room round count");
	value.roundCount = static_cast<std::uint8_t>(rounds);
	const auto time = ReadInt(json, "round_time", 30, 9999);
	if (time != 30 && time != 60 && time != 99 && time != 300 && time != 9999) throw std::invalid_argument("room round time");
	value.roundTime = static_cast<std::uint16_t>(time);
}
void to_json(nlohmann::json& json, const ConnectionRef& value) { json = nlohmann::json{{"host", value.host}, {"user", value.user}}; }
void from_json(const nlohmann::json& json, ConnectionRef& value) { value.host = ReadText(json, "host", 256); value.user = ReadText(json, "user", 256); }
void to_json(nlohmann::json& json, const Member& value) { json = nlohmann::json{{"id", value.id}, {"name", value.name}, {"connection", value.connection}, {"host", value.host}, {"status", static_cast<int>(value.status)}, {"table", value.table}, {"seat", value.seat}, {"join_order", value.joinOrder}, {"incarnation", value.incarnation},{"fighter",value.fighter},{"main_fighter",value.mainFighter}, {"selected_delay", value.selectedDelay}, {"frozen_delay", value.frozenDelay}, {"delay_locked", value.delayLocked}}; }
void from_json(const nlohmann::json& json, Member& value) {
	value.id = ReadU64(json, "id"); if (!value.id) throw std::invalid_argument("room member id");
	value.name = ReadText(json, "name", 32, false); json.at("connection").get_to(value.connection);
	if (!json.at("host").is_boolean()) throw std::invalid_argument("room host field"); value.host = json.at("host").get<bool>();
	value.status = static_cast<MemberStatus>(ReadInt(json, "status", 0, static_cast<int>(MemberStatus::WatchingNext)));
	value.table = static_cast<std::int8_t>(ReadInt(json, "table", -1, static_cast<int>(TableCount - 1)));
	value.seat = static_cast<std::int8_t>(ReadInt(json, "seat", -1, 1));
    value.joinOrder = ReadU64(json, "join_order");
    value.incarnation = json.contains("incarnation") ? ReadU64(json, "incarnation") : 1;
    if (!value.incarnation) throw std::invalid_argument("room member incarnation");
    value.fighter=json.contains("fighter")?ReadInt(json,"fighter",-1,43):-1;
    value.mainFighter=json.contains("main_fighter")?ReadInt(json,"main_fighter",-1,43):-1;
    value.selectedDelay=json.contains("selected_delay")?static_cast<std::uint8_t>(ReadInt(json,"selected_delay",0,10)):2;
    value.frozenDelay=json.contains("frozen_delay")?static_cast<std::uint8_t>(ReadInt(json,"frozen_delay",0,10)):value.selectedDelay;
    value.delayLocked=json.contains("delay_locked")?json.at("delay_locked").get<bool>():false;
    if (value.delayLocked && value.frozenDelay > 10) throw std::invalid_argument("room delay bounds");
}
void to_json(nlohmann::json& json, const Table& value) { json = nlohmann::json{{"id", value.id}, {"rules", value.rules}, {"phase", static_cast<int>(value.phase)}, {"revision", value.revision}, {"match_generation", value.matchGeneration}, {"p1", value.p1}, {"p2", value.p2}, {"queue", value.queue}, {"spectators", value.spectators}, {"watching_next", value.watchingNext}, {"ready", {value.ready[0], value.ready[1]}}, {"input_delay", {value.inputDelay[0], value.inputDelay[1]}}, {"score", {value.score[0], value.score[1]}}, {"result_pending", value.resultPending}}; }
void from_json(const nlohmann::json& json, Table& value) {
	value.id = static_cast<std::uint8_t>(ReadInt(json, "id", 0, static_cast<int>(TableCount - 1)));
	json.at("rules").get_to(value.rules);
	value.phase = static_cast<TablePhase>(ReadInt(json, "phase", 0, static_cast<int>(TablePhase::Closed)));
	value.revision = ReadU64(json, "revision"); value.matchGeneration = ReadU64(json, "match_generation");
	value.p1 = ReadU64(json, "p1"); value.p2 = ReadU64(json, "p2");
	if (value.p1 && value.p1 == value.p2) throw std::invalid_argument("room duplicate fighter");
	ReadMemberList(json, "queue", value.queue, MaxSpectators); ValidateMemberIds(value.queue);
	ReadMemberList(json, "spectators", value.spectators, MaxSpectators); ValidateMemberIds(value.spectators);
	ReadMemberList(json, "watching_next", value.watchingNext, MaxSpectators); ValidateMemberIds(value.watchingNext);
	const auto ready = json.at("ready"); if (!ready.is_array() || ready.size() != 2 || !ready.at(0).is_boolean() || !ready.at(1).is_boolean()) throw std::invalid_argument("room readiness");
	value.ready[0] = ready.at(0).get<bool>(); value.ready[1] = ready.at(1).get<bool>();
	if (json.contains("input_delay")) {
		const auto delay = json.at("input_delay");
		if (!delay.is_array() || delay.size() != 2 || !delay.at(0).is_number_integer() || !delay.at(1).is_number_integer()) throw std::invalid_argument("room input delay");
		const auto first = delay.at(0).get<long long>();
		if (first < 0 || first > 10) throw std::out_of_range("room input delay");
		value.inputDelay[0] = static_cast<std::uint8_t>(first);
		const auto second = delay.at(1).get<long long>(); if (second < 0 || second > 10) throw std::out_of_range("room input delay");
		value.inputDelay[1] = static_cast<std::uint8_t>(second);
	}
	const auto score = json.at("score"); if (!score.is_array() || score.size() != 2) throw std::invalid_argument("room score");
	const auto readScore = [](const nlohmann::json& number) -> std::uint32_t {
		if (!number.is_number_unsigned()) throw std::invalid_argument("room score type");
		const auto scoreValue = number.get<std::uint64_t>();
		if (scoreValue > (std::numeric_limits<std::uint32_t>::max)()) throw std::out_of_range("room score bounds");
		return static_cast<std::uint32_t>(scoreValue);
	};
	value.score[0] = readScore(score.at(0)); value.score[1] = readScore(score.at(1));
	if (!json.at("result_pending").is_boolean()) throw std::invalid_argument("room result pending"); value.resultPending = json.at("result_pending").get<bool>();
}
void to_json(nlohmann::json& json, const ChatMessage& value) { json = nlohmann::json{{"sequence", value.sequence}, {"sender", value.sender}, {"text", value.text}}; }
void from_json(const nlohmann::json& json, ChatMessage& value) { value.sequence = ReadU64(json, "sequence"); value.sender = ReadU64(json, "sender"); if (!value.sender) throw std::invalid_argument("room chat sender"); value.text = ReadText(json, "text", MaximumChatBytes, false); }
void to_json(nlohmann::json& json, const Snapshot& value) { json = nlohmann::json{{"protocol_version", value.protocolVersion}, {"room_epoch", value.roomEpoch}, {"revision", value.revision}, {"name", value.name}, {"capacity", value.capacity}, {"locked", value.locked}, {"closed", value.closed}, {"host", value.host}, {"local_member", value.localMember}, {"members", value.members}, {"tables", value.tables}, {"terminal_pending", value.terminalPending}, {"local_terminal_pending", value.localTerminalPending}, {"local_terminal_generations", value.localTerminalGenerations}, {"chat", value.chat}}; }
void from_json(const nlohmann::json& json, Snapshot& value) {
	value.protocolVersion = static_cast<std::uint32_t>(ReadInt(json, "protocol_version", 0, 100));
	if (value.protocolVersion != ProtocolVersion) throw std::invalid_argument("room protocol version");
	value.roomEpoch = ReadU64(json, "room_epoch"); if (!value.roomEpoch) throw std::invalid_argument("room epoch"); value.revision = ReadU64(json, "revision");
	value.name = ReadText(json, "name", MaximumRoomNameBytes, false); const auto capacity = ReadInt(json, "capacity", 2, static_cast<int>(MaximumMembers)); value.capacity = static_cast<std::uint8_t>(capacity);
	if (!json.at("locked").is_boolean() || !json.at("closed").is_boolean()) throw std::invalid_argument("room state field"); value.locked = json.at("locked").get<bool>(); value.closed = json.at("closed").get<bool>();
	value.host = ReadU64(json, "host"); value.localMember = ReadU64(json, "local_member");
	value.terminalPending.fill(false);
	if (json.contains("terminal_pending")) {
		const auto& pending = json.at("terminal_pending");
		if (!pending.is_array() || pending.size() != TableCount) throw std::invalid_argument("room terminal pending");
		for (std::size_t i = 0; i < TableCount; ++i) {
			if (!pending.at(i).is_boolean()) throw std::invalid_argument("room terminal pending");
			value.terminalPending[i] = pending.at(i).get<bool>();
		}
	}
	if (json.contains("local_terminal_pending") && !json.at("local_terminal_pending").is_boolean()) throw std::invalid_argument("room local terminal pending");
	value.localTerminalPending = json.value("local_terminal_pending", false);
	value.localTerminalGenerations.fill(0);
	if (json.contains("local_terminal_generations")) {
		const auto& generations = json.at("local_terminal_generations");
		if (!generations.is_array() || generations.size() != TableCount) throw std::invalid_argument("room local terminal generations");
		for (std::size_t i = 0; i < TableCount; ++i) {
			if (!generations.at(i).is_number_unsigned()) throw std::invalid_argument("room local terminal generation");
			value.localTerminalGenerations[i] = generations.at(i).get<std::uint64_t>();
		}
	}
	ReadMemberList(json, "members", value.members, MaximumMembers); std::set<MemberId> memberIds; for (const auto& member : value.members) if (!memberIds.insert(member.id).second) throw std::invalid_argument("room duplicate member");
	const auto& tables = json.at("tables"); if (!tables.is_array() || tables.size() != TableCount) throw std::invalid_argument("room table count"); tables.get_to(value.tables);
	for (std::size_t i = 0; i < TableCount; ++i) {
		const auto& table = value.tables[i];
		if (table.id != i) throw std::invalid_argument("room table id");
		const auto validMember = [&](MemberId id) { return id == 0 || memberIds.count(id) != 0; };
		if (!validMember(table.p1) || !validMember(table.p2)) throw std::invalid_argument("room fighter member");
		const auto validateList = [&](const std::vector<MemberId>& ids) {
			std::set<MemberId> seen;
			for (const auto id : ids) {
				if (!id || !memberIds.count(id) || !seen.insert(id).second) throw std::invalid_argument("room table member");
				if (id == table.p1 || id == table.p2) throw std::invalid_argument("room fighter spectator");
			}
		};
		validateList(table.queue); validateList(table.spectators); validateList(table.watchingNext);
		std::set<MemberId> spectators(table.spectators.begin(), table.spectators.end());
		for (const auto id : table.watchingNext) if (spectators.count(id)) throw std::invalid_argument("room duplicate watcher");
	}
	ReadMemberList(json, "chat", value.chat, MaximumChatMessages);
	if (value.host && !memberIds.count(value.host)) throw std::invalid_argument("room host member"); if (value.localMember && !memberIds.count(value.localMember)) throw std::invalid_argument("room local member");
	for (const auto& chat : value.chat) if (!memberIds.count(chat.sender)) throw std::invalid_argument("room chat member");
}
void to_json(nlohmann::json& json, const Action& value) { json = nlohmann::json{{"kind", static_cast<int>(value.kind)}, {"protocol_version", value.protocolVersion}, {"room_epoch", value.roomEpoch}, {"revision", value.revision}, {"table_revision", value.tableRevision}, {"action_id", value.actionId}, {"table", value.table}, {"seat", value.seat}, {"target", value.target}, {"rules", value.rules}, {"capacity", value.capacity}, {"locked", value.locked}, {"result", static_cast<int>(value.result)}, {"match_generation", value.matchGeneration}, {"input_delay", value.inputDelay}, {"text", value.text}}; }
  void from_json(const nlohmann::json& json, Action& value) { value.kind = static_cast<ActionKind>(ReadInt(json, "kind", 0, static_cast<int>(ActionKind::AcknowledgeTerminal))); value.protocolVersion = static_cast<std::uint32_t>(ReadInt(json, "protocol_version", 0, 100)); value.roomEpoch = ReadU64(json, "room_epoch"); value.revision = ReadU64(json, "revision"); value.tableRevision = ReadU64(json, "table_revision"); value.actionId = ReadU64(json, "action_id"); value.table = static_cast<std::uint8_t>(ReadInt(json, "table", 0, static_cast<int>(TableCount - 1))); value.seat = static_cast<std::int8_t>(ReadInt(json, "seat", -1, 1)); value.target = ReadU64(json, "target"); json.at("rules").get_to(value.rules); value.capacity = static_cast<std::uint8_t>(ReadInt(json, "capacity", 0, static_cast<int>(MaximumMembers))); if (!json.at("locked").is_boolean()) throw std::invalid_argument("room lock field"); value.locked = json.at("locked").get<bool>(); value.result = static_cast<MatchResult>(ReadInt(json, "result", 0, static_cast<int>(MatchResult::Abort))); value.matchGeneration = ReadU64(json, "match_generation"); value.inputDelay = static_cast<std::uint8_t>(json.contains("input_delay") ? ReadInt(json, "input_delay", 0, 10) : 2); value.text = ReadText(json, "text", MaximumChatBytes); }
  void to_json(nlohmann::json& json, const Event& value) { json = nlohmann::json{{"kind", static_cast<int>(value.kind)}, {"table", value.table}, {"match_generation", value.matchGeneration}, {"member", value.member}, {"result", static_cast<int>(value.result)}, {"terminal_replay", value.terminalReplay}}; }
  void from_json(const nlohmann::json& json, Event& value) { value.kind = static_cast<Event::Kind>(ReadInt(json, "kind", 0, static_cast<int>(Event::Kind::ChatMessage))); value.table = static_cast<std::uint8_t>(ReadInt(json, "table", 0, static_cast<int>(TableCount - 1))); value.matchGeneration = ReadU64(json, "match_generation"); value.member = ReadU64(json, "member"); value.result = static_cast<MatchResult>(ReadInt(json, "result", 0, static_cast<int>(MatchResult::Abort))); value.terminalReplay = json.contains("terminal_replay") ? json.at("terminal_replay").get<bool>() : false; }
  void to_json(nlohmann::json& json, const Result& value) { json = nlohmann::json{{"accepted", value.accepted}, {"reason", static_cast<int>(value.reason)}, {"terminal_replay", value.terminalReplay}, {"snapshot", value.snapshot}, {"events", value.events}}; }
void from_json(const nlohmann::json& json, Result& value) {
	if (!json.at("accepted").is_boolean()) throw std::invalid_argument("room result accepted");
	value.accepted = json.at("accepted").get<bool>();
	value.reason = static_cast<RejectReason>(ReadInt(json, "reason", 0, static_cast<int>(RejectReason::TerminalLedgerFull)));
	value.terminalReplay = json.contains("terminal_replay") ? json.at("terminal_replay").get<bool>() : false;
	json.at("snapshot").get_to(value.snapshot);
	const auto& events = json.at("events"); if (!events.is_array() || events.size() > 64) throw std::out_of_range("room result events");
	events.get_to(value.events);
}

} }
