// IrohRoom: committed checkpoint proposal, decode, staging and activation.
#include "IrohRoom.hxx"
#include "RoomMessageQueue.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "../common/RoomLimits.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace sf4e { namespace session {
using nlohmann::json;
namespace {
bool SameTransfer(const coordination::TransferIdentity& left, const coordination::TransferIdentity& right) {
    return left.room==right.room && left.epoch==right.epoch && left.transfer==right.transfer &&
        left.term==right.term && left.baseRevision==right.baseRevision && left.revision==right.revision &&
        left.length==right.length && left.digest==right.digest;
}
}

bool IrohRoom::ProposeCheckpointBytes(std::uint64_t request, std::uint64_t term,
    std::uint64_t baseRevision, std::string bytes) {
    if (!request) { proposalStatus_="invalid_request"; return false; }
    if (!coordination_.active) { proposalStatus_="coordination_inactive"; return false; }
    if (!coordination_.writable) { proposalStatus_="coordination_not_writable"; return false; }
    if (!coordination_.leaderLocal) { proposalStatus_="coordination_not_local_leader"; return false; }
    if (term != coordination_.term) { proposalStatus_="term_mismatch"; return false; }
    if (baseRevision != coordination_.revision) { proposalStatus_="base_revision_mismatch"; return false; }
    if (!proposalBytes_.empty()) { proposalStatus_="proposal_in_flight"; return false; }
    {
    // Digest only; PumpCheckpoint below times itself.
    diag::ScopedTimer timer(diag::OP_ROOM_PROPOSE);
    if (bytes.empty()) { proposalStatus_="empty_checkpoint"; return false; }
    if (bytes.size()>coordination::MaximumCheckpoint) { proposalStatus_="checkpoint_too_large"; return false; }
    if (baseRevision==UINT64_MAX) { proposalStatus_="base_revision_exhausted"; return false; }
    const auto digest=coordination::Sha256(bytes);
    if (digest.empty()) { proposalStatus_="checkpoint_digest_failed"; return false; }
    proposalIdentity_={room_,epoch_,request,term,baseRevision,baseRevision+1,bytes.size(),digest};
    proposalBytes_=std::move(bytes); proposalSent_=proposalAcknowledged_=0;
    proposalBegun_=proposalEnded_=false; proposalStartedMs_=GetTickCount64();
    proposalStatus_="in_flight";
    }
    PumpCheckpoint(); return true;
}
void IrohRoom::SubmitCommittedCheckpoint(const coordination::TransferIdentity& receiverIdentity) {
    const auto identity=receiverIdentity; // the receiver is reset below
    const auto ticket=nextDecodeTicket_++;
    // Claim the revision now so a replayed transfer of this same commit is
    // ignored while it decodes. A failed decode gives the revision back.
    decoding_.push_back({ticket,identity,receivedRevision_});
    receivedRevision_=identity.revision;
    const std::string bytes=checkpointReceiver_.Bytes();
    checkpointReceiver_.Reset();
    if(decodeOffThread_ && decoder_.Submit(ticket,bytes)) return;
    // The worker is disabled or its thread could not start. Decode inline from
    // now on; this one can complete inline only if nothing older is still
    // decoding, because results are matched oldest first. Otherwise fail it
    // closed like an undecodable one.
    decodeOffThread_=false;
    if(decoding_.size()==1) { FinishCommittedCheckpoint(CheckpointDecodeWorker::Decode(ticket,bytes)); return; }
    receivedRevision_=decoding_.back().previousReceivedRevision;
    decoding_.pop_back();
    error_="invalid_checkpoint_proposal";
}
void IrohRoom::CompleteCommittedCheckpoints() {
    CheckpointDecodeWorker::Result decoded;
    while(decoder_.TryTake(decoded)) FinishCommittedCheckpoint(std::move(decoded));
}
void IrohRoom::FinishCommittedCheckpoint(CheckpointDecodeWorker::Result&& decoded) {
    // A result for a room that has since been reset has no pending entry.
    if(decoding_.empty() || decoding_.front().ticket!=decoded.ticket) return;
    const auto pending=decoding_.front();
    decoding_.pop_front();
    const auto& identity=pending.identity;
    bool staged=false;
    try {
        if(decoded.ok && decoded.proposal.term==identity.term && decoded.proposal.request==identity.transfer &&
            decoded.proposal.baseRevision==identity.baseRevision && identity.revision==decoded.proposal.baseRevision+1) {
            StageCommittedCheckpoint(identity,std::move(decoded));
            staged=true;
        }
    } catch(const std::exception&) {}
    if(staged) return;
    if(receivedRevision_==identity.revision) receivedRevision_=pending.previousReceivedRevision;
    error_="invalid_checkpoint_proposal";
    // Staging capacity came back without an activation; release a parked marker.
    if(!pendingCommittedMarker_.is_null()) {
        auto marker=std::move(pendingCommittedMarker_); pendingCommittedMarker_=nullptr;
        ConsumeCoordinationEvent(marker,"checkpoint_committed");
    }
}
void IrohRoom::StageCommittedCheckpoint(const coordination::TransferIdentity& identity,
    CheckpointDecodeWorker::Result&& decoded) {
    auto& proposal=decoded.proposal;
    auto& journal=decoded.journal;
    std::map<room::MemberId,room::ConnectionRef> recipients;
    std::set<std::string> committedMembers;
    for(const auto& member:proposal.checkpoint.at("members")) {
        const auto& data=member.at("data");
        committedMembers.insert(data.at("authenticatedEndpoint").get<std::string>());
        if(data.value("authenticatedEndpoint",std::string())==localIdentity_)
            recipients.emplace(member.at("member").get<room::MemberId>(),member.at("endpoint").get<room::ConnectionRef>());
    }
    // Keep the prior identity long enough to authenticate an accepted
    // neutral departure response from the removal's committed journal.
    for(const auto& prior:effectRecipients_) {
        if(std::any_of(journal.begin(),journal.end(),[&](const EffectEnvelope& e){return e.recipient==prior.first;})) recipients.emplace(prior);
    }
    // A received commit is staged until the native recovery bridge has
    // imported it and rebound every endpoint.  This keeps
    // ClientAdapter from consuming effects for an unapplied room.
    for(const auto& staged:committedCheckpoints_) {
        for(const auto& prior:staged.recipients) {
            if(std::any_of(journal.begin(),journal.end(),[&](const EffectEnvelope& e){return e.recipient==prior.first;}))
                recipients.emplace(prior);
        }
    }
    committedCheckpoints_.push_back({identity,std::move(proposal),std::move(journal),
        std::move(recipients),std::move(committedMembers)});
    if (identity.transfer==proposalIdentity_.transfer && identity.term==proposalIdentity_.term) {
        proposalBytes_.clear(); proposalStatus_="committed";
    }
}
bool IrohRoom::TakeCommittedCheckpoint(CommittedCheckpoint& checkpoint) {
    if (committedCheckpoints_.empty()) return false;
    // Keep the head in place until the recovery bridge has imported it and
    // rebound every authenticated member.  A failed import must be retried in
    // order and must never expose the following commit.
    const auto& head=committedCheckpoints_.front();
    checkpoint.identity=head.identity;
    checkpoint.proposal=&head.proposal;
    checkpoint.journal=&head.effects;
    return true;
}
bool IrohRoom::BuildTerminalReplayEffects(const StagedCheckpoint& staged,
    const coordination::TransferIdentity& identity,
    std::vector<TerminalReplayEffect>& effects,
    std::set<TerminalReplayKey>& activeKeys) const {
    effects.clear();
    activeKeys.clear();
    // Old checkpoints do not have the durable receipt ledger. They remain
    // valid and simply have no local terminal replay to synthesize.
    const auto& checkpoint=staged.proposal.checkpoint;
    const auto roomState=checkpoint.value("room",json::object());
    if (!roomState.is_object() || !roomState.contains("terminal_receipts")) return true;
    const auto receipts=roomState.at("terminal_receipts");
    if (!receipts.is_array() || receipts.size()>64 || !coordination_.active || !coordination_.rebound ||
        identity.term==0 || identity.revision==0 || localIdentity_.empty()) return false;
    if (!checkpoint.contains("members") || !checkpoint.at("members").is_array()) return false;

    struct LocalMember { room::MemberId id=0; room::ConnectionRef endpoint; std::uint64_t incarnation=0; };
    std::vector<LocalMember> localMembers;
    for (const auto& row : checkpoint.at("members")) {
        if (!row.is_object() || !row.contains("member") || !row.contains("endpoint") || !row.contains("data")) return false;
        const auto member=row.at("member").get<room::MemberId>();
        const auto endpoint=row.at("endpoint").get<room::ConnectionRef>();
        const auto data=row.at("data");
        const auto authenticated=data.value("authenticatedEndpoint",std::string());
        const auto incarnation=row.value("incarnation",data.value("incarnation",std::uint64_t(0)));
        if (!member || endpoint.host.empty() || endpoint.user.empty() || !incarnation) return false;
        if (authenticated==localIdentity_)
            localMembers.push_back({member,endpoint,incarnation});
    }
    if (localMembers.size()>1) return false;
    if (localMembers.empty()) return true;
    const auto local=localMembers.front();
    const auto stagedRecipient=staged.recipients.find(local.id);
    if (stagedRecipient==staged.recipients.end() || !(stagedRecipient->second==local.endpoint)) return false;

    std::uint64_t roomEpoch=identity.epoch;
    if (roomState.contains("snapshot") && roomState.at("snapshot").is_object())
        roomEpoch=roomState.at("snapshot").value("room_epoch",roomEpoch);
    const auto append=[&](std::uint8_t table,std::uint64_t generation,const char* type,
        const json& payload,const TerminalReplayKey& key) {
        const auto alreadyPending=std::any_of(pendingTerminalReplayEffects_.begin(),pendingTerminalReplayEffects_.end(),
            [&](const TerminalReplayEffect& effect){return effect.key==key;});
        const auto alreadyInFlight=terminalReplayInFlight_.find(key)!=terminalReplayInFlight_.end();
        if (deliveredTerminalReplays_.count(key) || alreadyPending || alreadyInFlight) return true;
        TerminalReplayEffect effect;
        effect.key=key; effect.roomEpoch=roomEpoch; effect.generation=generation;
        effect.activationRevision=identity.revision; effect.recipient=local.id;
        effect.endpoint=local.endpoint; effect.type=type; effect.payloadDigest=PayloadDigest(payload);
        effect.publicPayload=payload;
        effects.push_back(std::move(effect));
        return true;
    };
    for (const auto& receipt:receipts) {
        if (!receipt.is_object() || !receipt.contains("table") || !receipt.contains("generation") ||
            !receipt.contains("result") || !receipt.contains("recipients")) return false;
        if (!receipt.at("table").is_number_unsigned() || !receipt.at("generation").is_number_unsigned() ||
            !receipt.at("result").is_number_integer() || !receipt.at("recipients").is_array()) return false;
        const auto table=receipt.at("table").get<std::uint64_t>();
        const auto generation=receipt.at("generation").get<std::uint64_t>();
        const auto result=receipt.at("result").get<int>();
        const auto recipients=receipt.at("recipients");
        if (table>=room::TableCount || !generation || result<0 || result>static_cast<int>(room::MatchResult::Abort) ||
            recipients.size()<2 || recipients.size()>room::MaximumMembers) return false;
        std::set<room::MemberId> seen;
        for (const auto& recipient:recipients) {
            if (!recipient.is_object() || !recipient.contains("member") || !recipient.contains("endpoint") ||
                !recipient.contains("incarnation") || !recipient.at("member").is_number_unsigned() ||
                !recipient.at("incarnation").is_number_unsigned() ||
                (recipient.contains("acknowledged") && !recipient.at("acknowledged").is_boolean())) return false;
            const auto member=recipient.at("member").get<room::MemberId>();
            const auto endpoint=recipient.at("endpoint").get<room::ConnectionRef>();
            const auto incarnation=recipient.at("incarnation").get<std::uint64_t>();
            if (!member || !incarnation || endpoint.host.empty() || endpoint.user.empty() || !seen.insert(member).second) return false;
            if (member!=local.id || recipient.value("acknowledged",false)) continue;
            if (!(endpoint==local.endpoint) || incarnation!=local.incarnation) return false;
            TerminalReplayKey roomKey{static_cast<std::uint8_t>(table),generation,local.id,local.endpoint,local.incarnation,"room_event"};
            TerminalReplayKey gameKey=roomKey; gameKey.type="game_end";
            activeKeys.insert(roomKey); activeKeys.insert(gameKey);
            SessionProtocol::RoomEventMessage event;
            event.event=room::Event{room::Event::Kind::MatchEnded,static_cast<std::uint8_t>(table),generation,0,
                static_cast<room::MatchResult>(result),true};
            if (!append(static_cast<std::uint8_t>(table),generation,"room_event",json(event),roomKey)) return false;
            if (!append(static_cast<std::uint8_t>(table),generation,"game_end",
                json{{"type","game_end"},{"generation",generation}},gameKey)) return false;
        }
    }
    return true;
}
bool IrohRoom::ActivateCommittedCheckpoint(const coordination::TransferIdentity& identity) {
    if (committedCheckpoints_.empty() || !SameTransfer(committedCheckpoints_.front().identity,identity)) return false;
    std::vector<TerminalReplayEffect> terminalEffects;
    std::set<TerminalReplayKey> activeTerminalKeys;
    if (!BuildTerminalReplayEffects(committedCheckpoints_.front(),identity,terminalEffects,activeTerminalKeys)) return false;
    auto staged=std::move(committedCheckpoints_.front());
    committedCheckpoints_.pop_front();
    effectRecipients_=std::move(staged.recipients);
    appliedEffects_=std::move(staged.effects);
    effectsRevision_=identity.revision;
    committedMembers_=std::move(staged.members);
    receivedRevision_=(std::max)(receivedRevision_,identity.revision);
    haveCommittedMembers_=true;
    for(auto& peer:peers_) if(committedMembers_.count(peer.second.identity)) peer.second.admitted=true;
    for(auto it=deliveredEffects_.begin();it!=deliveredEffects_.end();) {
        const bool retained=std::any_of(appliedEffects_.begin(),appliedEffects_.end(),[&](const EffectEnvelope& e){return *it==std::make_pair(e.term,e.sequence);});
        if(!retained) it=deliveredEffects_.erase(it); else ++it;
    }
    pendingPublicEffects_.clear();
    for(auto it=deliveredTerminalReplays_.begin();it!=deliveredTerminalReplays_.end();) {
        if(!activeTerminalKeys.count(*it)) it=deliveredTerminalReplays_.erase(it); else ++it;
    }
    // A newer applied receipt supersedes queued replay messages for a
    // recipient that has now acknowledged.  Remove those local messages
    // before they can block the ClientAdapter at its queue head.
    for(auto it=pendingTerminalReplayEffects_.begin();it!=pendingTerminalReplayEffects_.end();) {
        if(!activeTerminalKeys.count(it->key)) it=pendingTerminalReplayEffects_.erase(it); else ++it;
    }
    for(auto it=terminalReplayInFlight_.begin();it!=terminalReplayInFlight_.end();) {
        if(!activeTerminalKeys.count(it->first)) it=terminalReplayInFlight_.erase(it); else ++it;
    }
    for(auto it=clientMessages_.begin();it!=clientMessages_.end();) {
        bool remove=false;
        try {
            const auto queued=json::parse(it->payload);
            if(queued.contains("_terminal_replay")) {
                const auto marker=queued.at("_terminal_replay");
                const auto key=TerminalReplayKey{
                    marker.at("table").get<std::uint8_t>(), marker.at("generation").get<std::uint64_t>(),
                    marker.at("member").get<room::MemberId>(), marker.at("endpoint").get<room::ConnectionRef>(),
                    marker.at("incarnation").get<std::uint64_t>(), marker.at("type").get<std::string>()};
                remove=!activeTerminalKeys.count(key);
            }
        } catch(const std::exception&) {}
        if(remove) { queuedBytes_-=it->payload.size(); it=clientMessages_.erase(it); }
        else ++it;
    }
    for(const auto& effect:appliedEffects_) {
        if(!effect.privatePayload && !effect.publicPayload.is_null() && effectRecipients_.count(effect.recipient) &&
            !deliveredEffects_.count({effect.term,effect.sequence})) pendingPublicEffects_.push_back(effect);
    }
    for(auto& effect:terminalEffects) pendingTerminalReplayEffects_.push_back(std::move(effect));
    if(!pendingCommittedMarker_.is_null()) {
        auto marker=std::move(pendingCommittedMarker_); pendingCommittedMarker_=nullptr;
        ConsumeCoordinationEvent(marker,"checkpoint_committed");
    }
    return true;
}
bool IrohRoom::ReadyForMatch() const {
    if(state_ != State::Ready) return false;
    if(!coordination_.active) return true;
    return coordination_.writable && coordination_.rebound &&
        effectsRevision_ >= coordination_.revision && committedCheckpoints_.empty() && decoding_.empty();
}
void IrohRoom::PumpCheckpoint() {
    diag::ScopedTimer timer(diag::OP_ROOM_PROPOSE);
    const auto now=GetTickCount64();
    if (!pendingCheckpointAck_.is_null() && helper_.Send(pendingCheckpointAck_.dump())) pendingCheckpointAck_=nullptr;
    if (checkpointReceiver_.Expired(now) && pendingCommittedMarker_.is_null()) {
        ++checkpointTimeouts_;
        checkpointReceiver_.Reset();
    }
    if (proposalBytes_.empty()) return;
    if (proposalIdentity_.term!=coordination_.term || !coordination_.writable ||
        now-proposalStartedMs_>=15000) {
        if(proposalIdentity_.term!=coordination_.term) proposalStatus_="term_changed";
        else if(!coordination_.writable) proposalStatus_="coordination_not_writable";
        else { ++proposalTimeouts_; proposalStatus_="transfer_timeout"; }
        proposalBytes_.clear(); proposalBegun_=proposalEnded_=false; return;
    }
    if (!proposalBegun_) {
        auto message=proposalIdentity_.Envelope("checkpoint_begin");
        message["length"]=proposalIdentity_.length; message["digest"]=proposalIdentity_.digest;
        if (!helper_.Send(message.dump())) return;
        proposalBegun_=true;
    }
    unsigned budget=4;
    while (proposalSent_<proposalBytes_.size() &&
        proposalSent_-proposalAcknowledged_<4*coordination::CheckpointChunk && budget--) {
        auto message=proposalIdentity_.Envelope("checkpoint_chunk");
        const auto bytes=proposalBytes_.substr(proposalSent_,coordination::CheckpointChunk);
        message["offset"]=proposalSent_; message["data"]=coordination::Encode(bytes);
        if (!helper_.Send(message.dump())) return;
        proposalSent_+=bytes.size();
    }
    if (!proposalEnded_ && proposalAcknowledged_==proposalBytes_.size()) {
        auto message=proposalIdentity_.Envelope("checkpoint_end");
        message["length"]=proposalIdentity_.length; message["digest"]=proposalIdentity_.digest;
        if (helper_.Send(message.dump())) proposalEnded_=true;
    }
}

} }
