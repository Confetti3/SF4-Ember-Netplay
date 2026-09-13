#pragma once
#include "RoomModel.hxx"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace sf4e { namespace room {
// Game-thread proposal owner. Transport adapters submit the exported candidate
// to consensus and return its committed token; UI/native effects stay withheld.
class RoomCommit {
public:
    struct Token {
        std::uint64_t term = 0, request = 0, baseRevision = 0;
        bool operator==(const Token& rhs) const {
            return term == rhs.term && request == rhs.request && baseRevision == rhs.baseRevision;
        }
    };
    explicit RoomCommit(RoomAuthority& authority) : authority_(authority) {}
    void SetAuthority(std::uint64_t term, bool writable) {
        if (term < term_) return;
        if (term != term_ || !writable) pending_.reset();
        term_ = term; writable_ = writable && term != 0;
    }
    bool Prepare(std::uint64_t request, const std::function<Result(RoomAuthority&)>& apply) {
        if (!writable_ || !request || pending_) return false;
        auto proposal = std::make_unique<Pending>(authority_);
        proposal->token = {term_, request, authority_.SnapshotView().revision};
        proposal->result = apply(proposal->authority);
        if (!proposal->result.accepted) return false;
        try { proposal->checkpoint = proposal->authority.Checkpoint(); }
        catch (const std::exception&) { return false; }
        pending_ = std::move(proposal); return true;
    }
    bool PendingProposal() const { return bool(pending_); }
    Token PendingToken() const { return pending_ ? pending_->token : Token{}; }
    const nlohmann::json* Candidate() const { return pending_ ? &pending_->checkpoint : nullptr; }
    bool Commit(Token token, Result& effects) {
        if (!writable_ || !pending_ || !(pending_->token == token) || token.term != term_ ||
            token.baseRevision != authority_.SnapshotView().revision) return false;
        authority_ = std::move(pending_->authority);
        effects = std::move(pending_->result);
        pending_.reset(); return true;
    }
    void Discard() { pending_.reset(); }
private:
    struct Pending {
        explicit Pending(const RoomAuthority& source) : authority(source) {}
        RoomAuthority authority;
        Token token;
        Result result;
        nlohmann::json checkpoint;
    };
    RoomAuthority& authority_;
    std::uint64_t term_ = 0;
    bool writable_ = false;
    std::unique_ptr<Pending> pending_;
};
} }
