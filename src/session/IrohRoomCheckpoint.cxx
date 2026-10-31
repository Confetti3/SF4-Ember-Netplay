// IrohRoom: committed checkpoint proposal, decode, staging and activation.
#include "IrohRoom.hxx"
#include "RoomMessageQueue.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "../common/JsonDump.hxx"
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
    if (!request) { sender_.status="invalid_request"; return false; }
    if (!room_.coordination.active) { sender_.status="coordination_inactive"; return false; }
    if (!room_.coordination.writable) { sender_.status="coordination_not_writable"; return false; }
    if (!room_.coordination.leaderLocal) { sender_.status="coordination_not_local_leader"; return false; }
    if (term != room_.coordination.term) { sender_.status="term_mismatch"; return false; }
    if (baseRevision != room_.coordination.revision) { sender_.status="base_revision_mismatch"; return false; }
    if (!sender_.bytes.empty()) { sender_.status="proposal_in_flight"; return false; }
    {
    // Digest only; PumpCheckpoint below times itself.
    diag::ScopedTimer timer(diag::OP_ROOM_PROPOSE);
    if (bytes.empty()) { sender_.status="empty_checkpoint"; return false; }
    if (bytes.size()>coordination::MaximumCheckpoint) { sender_.status="checkpoint_too_large"; return false; }
    if (baseRevision==UINT64_MAX) { sender_.status="base_revision_exhausted"; return false; }
    const auto digest=coordination::Sha256(bytes);
    if (digest.empty()) { sender_.status="checkpoint_digest_failed"; return false; }
    sender_.identity={room_.id,epoch_,request,term,baseRevision,baseRevision+1,bytes.size(),digest};
    sender_.bytes=std::move(bytes); sender_.sent=sender_.acknowledged=0;
    sender_.begun=sender_.ended=false; sender_.startedMs=GetTickCount64();
    sender_.status="in_flight";
    }
    PumpCheckpoint(); return true;
}
void IrohRoom::SubmitCommittedCheckpoint(const coordination::TransferIdentity& receiverIdentity) {
    const auto identity=receiverIdentity; // the receiver is reset below
    const auto ticket=nextDecodeTicket_++;
    // Claim the revision now so a replayed transfer of this same commit is
    // ignored while it decodes. A failed decode gives the revision back.
    replica_.decoding.push_back({ticket,identity,replica_.receivedRevision});
    replica_.receivedRevision=identity.revision;
    // Taken, not copied, and moved on to the worker: a commit's bytes exist
    // once from the receiver to the decode.
    std::string bytes=replica_.checkpointReceiver.TakeBytes();
    replica_.checkpointReceiver.Reset();
    if(decodeOffThread_ && decoder_.Submit(ticket,std::move(bytes))) return;
    // The worker is disabled or its thread could not start. Decode inline from
    // now on; this one can complete inline only if nothing older is still
    // decoding, because results are matched oldest first. Otherwise fail it
    // closed like an undecodable one.
    decodeOffThread_=false;
    if(replica_.decoding.size()==1) { FinishCommittedCheckpoint(CheckpointDecodeWorker::Decode(ticket,bytes)); return; }
    replica_.receivedRevision=replica_.decoding.back().previousReceivedRevision;
    replica_.decoding.pop_back();
    room_.error="invalid_checkpoint_proposal";
}
void IrohRoom::CompleteCommittedCheckpoints() {
    CheckpointDecodeWorker::Result decoded;
    while(decoder_.TryTake(decoded)) FinishCommittedCheckpoint(std::move(decoded));
}
void IrohRoom::FinishCommittedCheckpoint(CheckpointDecodeWorker::Result&& decoded) {
    // A result for a room that has since been reset has no pending entry.
    if(replica_.decoding.empty() || replica_.decoding.front().ticket!=decoded.ticket) return;
    const auto pending=replica_.decoding.front();
    replica_.decoding.pop_front();
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
    if(replica_.receivedRevision==identity.revision) replica_.receivedRevision=pending.previousReceivedRevision;
    room_.error="invalid_checkpoint_proposal";
    // Staging capacity came back without an activation; release a parked marker.
    if(!replica_.pendingCommittedMarker.is_null()) {
        auto marker=std::move(replica_.pendingCommittedMarker); replica_.pendingCommittedMarker=nullptr;
        ConsumeCoordinationEvent(marker,"checkpoint_committed");
    }
}
void IrohRoom::StageCommittedCheckpoint(const coordination::TransferIdentity& identity,
    CheckpointDecodeWorker::Result&& decoded) {
    auto& proposal=decoded.proposal;
    auto& journal=decoded.journal;
    std::map<room::MemberId,room::ConnectionRef> recipients;
    std::set<std::string> committedMembers;
    std::size_t liveMembers=0;
    for(const auto& member:proposal.checkpoint.at("members")) {
        const auto& data=member.at("data");
        committedMembers.insert(data.at("authenticatedEndpoint").get<std::string>());
        if(!member.value("frozen",false)) ++liveMembers;
        if(data.value("authenticatedEndpoint",std::string())==localIdentity_)
            recipients.emplace(member.at("member").get<room::MemberId>(),member.at("endpoint").get<room::ConnectionRef>());
    }
    // Keep the prior identity long enough to authenticate an accepted
    // neutral departure response from the removal's committed journal.
    for(const auto& prior:replica_.effectRecipients) {
        if(std::any_of(journal.begin(),journal.end(),[&](const EffectEnvelope& e){return e.recipient==prior.first;})) recipients.emplace(prior);
    }
    // A received commit is staged until the native recovery bridge has
    // imported it and rebound every endpoint.  This keeps
    // ClientAdapter from consuming effects for an unapplied room.
    for(const auto& staged:replica_.committedCheckpoints) {
        for(const auto& prior:staged.recipients) {
            if(std::any_of(journal.begin(),journal.end(),[&](const EffectEnvelope& e){return e.recipient==prior.first;}))
                recipients.emplace(prior);
        }
    }
    replica_.committedCheckpoints.push_back({identity,std::move(proposal),std::move(journal),
        std::move(recipients),std::move(committedMembers),liveMembers});
    if (identity.transfer==sender_.identity.transfer && identity.term==sender_.identity.term) {
        sender_.bytes.clear(); sender_.status="committed";
    }
}
bool IrohRoom::TakeCommittedCheckpoint(CommittedCheckpoint& checkpoint) {
    if (replica_.committedCheckpoints.empty()) return false;
    // Keep the head in place until the recovery bridge has imported it and
    // rebound every authenticated member.  A failed import must be retried in
    // order and must never expose the following commit.
    const auto& head=replica_.committedCheckpoints.front();
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
    if (!receipts.is_array() || receipts.size()>64 || !room_.coordination.active || !room_.coordination.rebound ||
        identity.term==0 || identity.revision==0 || localIdentity_.empty()) return false;
    if (!checkpoint.contains("members")) return false;
    RosterRow local;
    if (!ActiveRosterRow(checkpoint.at("members"),localIdentity_,local)) return false;
    if (!local.member) return true;
    const auto stagedRecipient=staged.recipients.find(local.member);
    if (stagedRecipient==staged.recipients.end() || !(stagedRecipient->second==local.endpoint)) return false;

    std::uint64_t roomEpoch=identity.epoch;
    if (roomState.contains("snapshot") && roomState.at("snapshot").is_object())
        roomEpoch=roomState.at("snapshot").value("room_epoch",roomEpoch);
    const auto append=[&](std::uint8_t table,std::uint64_t generation,const char* type,
        const json& payload,const TerminalReplayKey& key) {
        const auto alreadyPending=std::any_of(replica_.pendingTerminalReplayEffects.begin(),replica_.pendingTerminalReplayEffects.end(),
            [&](const TerminalReplayEffect& effect){return effect.key==key;});
        const auto alreadyInFlight=replica_.terminalReplayInFlight.find(key)!=replica_.terminalReplayInFlight.end();
        if (replica_.deliveredTerminalReplays.count(key) || alreadyPending || alreadyInFlight) return true;
        TerminalReplayEffect effect;
        effect.key=key; effect.roomEpoch=roomEpoch; effect.generation=generation;
        effect.activationRevision=identity.revision; effect.recipient=local.member;
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
            if (member!=local.member || recipient.value("acknowledged",false)) continue;
            if (!(endpoint==local.endpoint) || incarnation!=local.incarnation) return false;
            TerminalReplayKey roomKey{static_cast<std::uint8_t>(table),generation,local.member,local.endpoint,local.incarnation,"room_event"};
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
    if (replica_.committedCheckpoints.empty() || !SameTransfer(replica_.committedCheckpoints.front().identity,identity)) return false;
    std::vector<TerminalReplayEffect> terminalEffects;
    std::set<TerminalReplayKey> activeTerminalKeys;
    if (!BuildTerminalReplayEffects(replica_.committedCheckpoints.front(),identity,terminalEffects,activeTerminalKeys)) return false;
    auto staged=std::move(replica_.committedCheckpoints.front());
    replica_.committedCheckpoints.pop_front();
    replica_.effectRecipients=std::move(staged.recipients);
    replica_.appliedEffects=std::move(staged.effects);
    replica_.effectsRevision=identity.revision;
    replica_.committedMembers=std::move(staged.members);
    replica_.committedLiveMembers=staged.liveMembers;
    replica_.receivedRevision=(std::max)(replica_.receivedRevision,identity.revision);
    replica_.haveCommittedMembers=true;
    for(auto& peer:room_.peers) if(replica_.committedMembers.count(peer.second.identity)) peer.second.admitted=true;
    for(auto it=replica_.deliveredEffects.begin();it!=replica_.deliveredEffects.end();) {
        const bool retained=std::any_of(replica_.appliedEffects.begin(),replica_.appliedEffects.end(),[&](const EffectEnvelope& e){return *it==std::make_pair(e.term,e.sequence);});
        if(!retained) it=replica_.deliveredEffects.erase(it); else ++it;
    }
    replica_.pendingPublicEffects.clear();
    for(auto it=replica_.deliveredTerminalReplays.begin();it!=replica_.deliveredTerminalReplays.end();) {
        if(!activeTerminalKeys.count(*it)) it=replica_.deliveredTerminalReplays.erase(it); else ++it;
    }
    // A newer applied receipt supersedes queued replay messages for a
    // recipient that has now acknowledged.  Remove those local messages
    // before they can block the ClientAdapter at its queue head.
    for(auto it=replica_.pendingTerminalReplayEffects.begin();it!=replica_.pendingTerminalReplayEffects.end();) {
        if(!activeTerminalKeys.count(it->key)) it=replica_.pendingTerminalReplayEffects.erase(it); else ++it;
    }
    for(auto it=replica_.terminalReplayInFlight.begin();it!=replica_.terminalReplayInFlight.end();) {
        if(!activeTerminalKeys.count(it->first)) it=replica_.terminalReplayInFlight.erase(it); else ++it;
    }
    for(auto it=room_.clientMessages.begin();it!=room_.clientMessages.end();) {
        bool remove=false;
        // Only a terminal replay can be removed. Every edge into this queue
        // classifies what it queues, so other messages are not parsed here.
        if(it->tag.classified && !it->tag.terminalReplay) { ++it; continue; }
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
        if(remove) { room_.queuedBytes-=it->payload.size(); it=room_.clientMessages.erase(it); }
        else ++it;
    }
    for(const auto& effect:replica_.appliedEffects) {
        if(!effect.privatePayload && !effect.publicPayload.is_null() && replica_.effectRecipients.count(effect.recipient) &&
            !replica_.deliveredEffects.count({effect.term,effect.sequence})) replica_.pendingPublicEffects.push_back(effect);
    }
    for(auto& effect:terminalEffects) replica_.pendingTerminalReplayEffects.push_back(std::move(effect));
    if(!replica_.pendingCommittedMarker.is_null()) {
        auto marker=std::move(replica_.pendingCommittedMarker); replica_.pendingCommittedMarker=nullptr;
        ConsumeCoordinationEvent(marker,"checkpoint_committed");
    }
    return true;
}
bool IrohRoom::DiscardCommittedCheckpoint(const coordination::TransferIdentity& identity) {
    if (replica_.committedCheckpoints.empty() || !SameTransfer(replica_.committedCheckpoints.front().identity,identity)) return false;
    replica_.committedCheckpoints.pop_front();
    replica_.receivedRevision=(std::max)(replica_.receivedRevision,identity.revision);
    if(!replica_.pendingCommittedMarker.is_null()) {
        auto marker=std::move(replica_.pendingCommittedMarker); replica_.pendingCommittedMarker=nullptr;
        ConsumeCoordinationEvent(marker,"checkpoint_committed");
    }
    return true;
}
bool IrohRoom::ReadyForMatch() const {
    if(room_.state != State::Ready) return false;
    if(!room_.coordination.active) return true;
    return room_.coordination.writable && room_.coordination.rebound &&
        replica_.effectsRevision >= room_.coordination.revision && replica_.committedCheckpoints.empty() && replica_.decoding.empty();
}
void IrohRoom::PumpCheckpoint() {
    diag::ScopedTimer timer(diag::OP_ROOM_PROPOSE);
    const auto now=GetTickCount64();
    if (!replica_.pendingCheckpointAck.is_null() && helper_.Send(DumpJson(replica_.pendingCheckpointAck))) replica_.pendingCheckpointAck=nullptr;
    if (replica_.checkpointReceiver.Expired(now) && replica_.pendingCommittedMarker.is_null()) {
        ++replica_.checkpointTimeouts;
        replica_.checkpointReceiver.Reset();
    }
    if (sender_.bytes.empty()) return;
    if (sender_.identity.term!=room_.coordination.term || !room_.coordination.writable ||
        now-sender_.startedMs>=15000) {
        if(sender_.identity.term!=room_.coordination.term) sender_.status="term_changed";
        else if(!room_.coordination.writable) sender_.status="coordination_not_writable";
        else { ++sender_.timeouts; sender_.status="transfer_timeout"; }
        sender_.bytes.clear(); sender_.begun=sender_.ended=false; return;
    }
    if (!sender_.begun) {
        auto message=sender_.identity.Envelope("checkpoint_begin");
        message["length"]=sender_.identity.length; message["digest"]=sender_.identity.digest;
        if (!helper_.Send(DumpJson(message))) return;
        sender_.begun=true;
    }
    unsigned budget=4;
    while (sender_.sent<sender_.bytes.size() &&
        sender_.sent-sender_.acknowledged<4*coordination::CheckpointChunk && budget--) {
        auto message=sender_.identity.Envelope("checkpoint_chunk");
        // The chunk goes as the frame's raw body, copied once from the proposal.
        const auto size=(std::min)(coordination::CheckpointChunk,sender_.bytes.size()-sender_.sent);
        message["offset"]=sender_.sent;
        if (!helper_.Send(platform::helper_frame::EncodeRawBody(DumpJson(message),sender_.bytes.data()+sender_.sent,size))) return;
        sender_.sent+=size;
    }
    if (!sender_.ended && sender_.acknowledged==sender_.bytes.size()) {
        auto message=sender_.identity.Envelope("checkpoint_end");
        message["length"]=sender_.identity.length; message["digest"]=sender_.identity.digest;
        if (helper_.Send(DumpJson(message))) sender_.ended=true;
    }
}

} }
