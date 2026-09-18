#pragma once
#include "IrohRoom.hxx"
#include "sf4e__SessionServer.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"

namespace sf4e { namespace session {
// The production bridge is also used by helper integration tests. It never
// constructs or replaces a gameplay session: only the private room replica
// and its locally committed effects pass through this boundary.
class RoomRecoveryRuntime {
public:
    bool Tick(SessionServer& server, IrohRoom& room) {
        room.Poll();
        const auto authority=room.Coordination();
        if(!authority.active) return true;
        bool deferredLocalCommit=false;
        IrohRoom::CommittedCheckpoint committed;
        while(room.TakeCommittedCheckpoint(committed)) {
            try {
                // IrohRoom decoded this commit, verified it against its transfer
                // identity and compacted it when it arrived; repeating that here
                // doubled every member's import.
                const auto& proposal=*committed.proposal;
                const auto pending=server.PendingProposal();
                bool applied=false;
                const bool matchingLocalCandidate=pending && pending->request==proposal.request &&
                    pending->term==proposal.term && pending->baseRevision==proposal.baseRevision;
                const bool localAuthorityReady=authority.term==proposal.term &&
                    authority.revision>=proposal.baseRevision && authority.writable &&
                    authority.leaderLocal && authority.rebound;
                // The commit stream and writable/rebound watch are independent.
                // If this is our exact proposal but the same-term watch is
                // temporarily unhealthy, keep the checkpoint at the room head
                // and retain the native candidate's private payloads. Importing
                // its replicated digest-only form would lose game_prepare.
                if(matchingLocalCandidate && authority.term==proposal.term && !localAuthorityReady) {
                    deferredLocalCommit=true;
                    break;
                }
                {
                    diag::ScopedTimer applyTimer(diag::OP_ROOM_IMPORT_APPLY);
                    if(matchingLocalCandidate && localAuthorityReady) {
                        // SetAuthority at the end of the paused pump deliberately
                        // kept the gate non-writable at the candidate base. Restore
                        // that exact local authority before ApplyCommit; otherwise
                        // the first healthy pump would reject the retained commit.
                        server.SetAuthority(proposal.term,proposal.baseRevision,true,
                            authority.writable && authority.rebound);
                        applied=server.ApplyCommit(proposal.request,proposal.term,committed.identity.revision,
                            proposal.checkpoint,proposal.effectsDigest);
                    } else {
                        server.DiscardProposal();
                        applied=server.RestoreRecoveryCheckpoint(proposal.checkpoint,*committed.journal,
                            AuthorityStamp{proposal.term,committed.identity.revision,false});
                        if(applied) { needsRebind_=true; }
                    }
                }
                if(!applied) throw std::runtime_error(matchingLocalCandidate
                    ? "local committed candidate rejected" : "replicated checkpoint import rejected");
                // A replicated import is still private until every stable
                // member has been rebound.  Activate the room journal only
                // after that native boundary succeeds; a missing peer leaves
                // this exact head queued for retry and cannot be overtaken.
                if(needsRebind_ && !Rebind(server,room))
                    throw std::runtime_error("replicated checkpoint rebind pending");
                // Activation retires the staged head, and with it `proposal`.
                // identity.transfer is the proposal's request, held by value.
                if(!room.ActivateCommittedCheckpoint(committed.identity))
                    throw std::runtime_error("committed checkpoint activation pending");
                const auto request=committed.identity.transfer;
                needsRebind_=false;
                if(request==UINT64_MAX) throw std::runtime_error("checkpoint request exhausted");
                nextRequest_=(std::max)(nextRequest_,request+1);
                appliedRevision_=committed.identity.revision;
                failed_=false;
                error_.clear();
            } catch(const std::exception& error) {
                error_=error.what();
                // A passive peer may receive the durable checkpoint before
                // its helper has rebound one endpoint.  Leave the exact head
                // queued and keep the runtime healthy-but-paused so the next
                // pump can retry without publishing a false import failure.
                const std::string retry=error.what();
                if(retry=="replicated checkpoint rebind pending" ||
                    retry=="committed checkpoint activation pending") failed_=false;
                else failed_=true;
                break;
            }
        }
        const auto pending=server.PendingProposal();
        // The state watch can precede bulk checkpoint delivery. Never replace
        // a private candidate's base revision with an unapplied watch value.
        const auto revision=pending && pending->term==authority.term ? pending->baseRevision : appliedRevision_;
        const bool writable=!failed_ && !needsRebind_ && authority.writable && authority.rebound && authority.leaderLocal &&
            (appliedRevision_==authority.revision || (pending && pending->term==authority.term));
        server.SetAuthority(authority.term,revision,writable,
            authority.writable && authority.rebound);
        if(writable && !server.HasRecoveryCandidate() && !server.PendingProposal())
            server.CancelInterruptedPreparations();
        if(writable && server.HasRecoveryCandidate() && !server.PendingProposal())
            server.ProposeCheckpoint(nextRequest_++,authority.term,appliedRevision_,nullptr);
        if(const auto next=server.PendingProposal()) {
            if(!deferredLocalCommit && !room.ProposalInFlight())
                room.ProposeCheckpointBytes(next->request,next->term,next->baseRevision,next->encoded);
        }
        return !failed_;
    }
    std::uint64_t AppliedRevision() const { return appliedRevision_; }
    const std::string& Error() const { return error_; }
    bool CaughtUp(const IrohRoom::CoordinationSnapshot& authority) const {
        return !failed_ && !needsRebind_ && appliedRevision_==authority.revision;
    }
private:
    static bool Rebind(SessionServer& server, const IrohRoom& room) {
        diag::ScopedTimer timer(diag::OP_ROOM_IMPORT_REBIND);
        const auto* snapshot=server.RoomSnapshot();
        if(!snapshot) return false;
        if(snapshot->members.empty()) return true;
        std::vector<SessionServer::StableRebind> bindings;
        for(const auto& member:snapshot->members) {
            const auto peer=server.roomPeerIdentities.find(member.id);
            if(peer==server.roomPeerIdentities.end()) return false;
            const auto connection=room.ConnectionForIdentity(peer->second);
            if(!connection) return false;
            const auto incarnation=server.roomIncarnations.count(member.id)?server.roomIncarnations.at(member.id):0;
            const auto authenticated=room.PeerIncarnation(connection);
            if(!incarnation || (authenticated && authenticated!=incarnation)) return false;
            bindings.emplace_back(member.id,connection,SessionProtocol::ConnectionID{member.connection.host,member.connection.user},
                incarnation);
        }
        return server.RebindMembers(bindings);
    }
    std::uint64_t nextRequest_=1, appliedRevision_=0;
    bool failed_=false;
    bool needsRebind_=false;
    std::string error_;
};
} }
