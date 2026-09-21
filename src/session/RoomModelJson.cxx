// Room model persistence: checkpoints and the JSON wire form of room types.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"

namespace sf4e { namespace room {
using namespace detail;

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
	// Older owners kept a departed member's chat. Drop those lines rather than
	// reject the whole room state; the sender cannot be named anyway.
	value.chat.erase(std::remove_if(value.chat.begin(), value.chat.end(),
		[&](const ChatMessage& chat) { return !memberIds.count(chat.sender); }), value.chat.end());
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
