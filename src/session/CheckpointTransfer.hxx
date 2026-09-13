#pragma once
#include "../common/CoordinationBytes.hxx"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <optional>

namespace sf4e { namespace coordination {
constexpr std::size_t MaximumCheckpoint = 1024 * 1024;
constexpr std::size_t CheckpointChunk = 16 * 1024;
struct TransferIdentity {
    std::array<std::uint8_t,16> room{};
    std::uint64_t epoch=0, transfer=0, term=0, baseRevision=0, revision=0;
    std::size_t length=0;
    std::string digest;
    nlohmann::json Envelope(const char* type) const {
        return {{"type",type},{"epoch",epoch},{"room",room},{"transfer",transfer},
            {"term",term},{"base_revision",baseRevision},{"revision",revision}};
    }
    bool Matches(const nlohmann::json& message) const {
        return message.at("epoch")==epoch && message.at("room")==room && message.at("transfer")==transfer &&
            message.at("term")==term && message.at("base_revision")==baseRevision && message.at("revision")==revision;
    }
};
// Reassembly has no transport side effects. Only a validated End can publish
// bytes; the caller additionally requires a local committed-state receipt.
class CheckpointReceiver {
public:
    bool Begin(const nlohmann::json& message, std::uint64_t now) {
        try {
            TransferIdentity next;
            next.epoch=message.at("epoch"); next.room=message.at("room").get<decltype(next.room)>();
            next.transfer=message.at("transfer"); next.term=message.at("term");
            next.baseRevision=message.at("base_revision"); next.revision=message.at("revision");
            next.length=message.at("length"); next.digest=message.at("digest");
            if (!next.transfer || !next.term || !next.length || next.length>MaximumCheckpoint ||
                next.digest.size()!=64 || !std::all_of(next.digest.begin(),next.digest.end(),[](char c){
                    return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return false;
			if (active_) {
				// The sender restarts a timed-out transfer from offset zero. Only the
				// exact immutable identity may replace an incomplete body; a completed
				// body remains pinned until its committed marker is consumed.
				if (complete_ || identity_.epoch!=next.epoch || identity_.room!=next.room ||
					identity_.transfer!=next.transfer || identity_.term!=next.term ||
					identity_.baseRevision!=next.baseRevision || identity_.revision!=next.revision ||
					identity_.length!=next.length || identity_.digest!=next.digest) return false;
			}
            identity_=std::move(next); bytes_.clear(); bytes_.reserve(identity_.length);
            active_=true; complete_=false; began_=now; return true;
        } catch (const nlohmann::json::exception&) { return false; }
    }
    bool Chunk(const nlohmann::json& message) {
        try {
            if (!active_ || complete_ || !identity_.Matches(message) || message.at("offset")!=bytes_.size()) return false;
            std::string decoded;
            if (!Decode(message.at("data").get<std::string>(),decoded,CheckpointChunk) || decoded.empty() ||
                decoded.size()>identity_.length-bytes_.size()) return false;
            bytes_+=decoded; return true;
        } catch (const nlohmann::json::exception&) { return false; }
    }
    bool End(const nlohmann::json& message) {
        try {
            if (!active_ || complete_ || !identity_.Matches(message) || message.at("length")!=identity_.length ||
                message.at("digest")!=identity_.digest || bytes_.size()!=identity_.length || Sha256(bytes_)!=identity_.digest) return false;
            complete_=true; return true;
        } catch (const nlohmann::json::exception&) { return false; }
    }
    std::optional<std::size_t> CompleteReplayAcknowledgment(
        const nlohmann::json& message, const std::string& type) const {
        if (!active_ || !complete_) return std::nullopt;
        try {
            if (!identity_.Matches(message)) return std::nullopt;
            if (type == "checkpoint_chunk") {
                const auto offset = message.at("offset").get<std::size_t>();
                std::string decoded;
                if (!Decode(message.at("data").get<std::string>(), decoded, CheckpointChunk) || decoded.empty() ||
                    offset > bytes_.size() || decoded.size() > bytes_.size() - offset ||
                    bytes_.compare(offset, decoded.size(), decoded) != 0) return std::nullopt;
                return offset + decoded.size();
            }
            if (type == "checkpoint_end" && message.at("length") == identity_.length &&
                message.at("digest") == identity_.digest) return identity_.length;
        } catch (const nlohmann::json::exception&) {}
        return std::nullopt;
    }
    void Reset() { active_=complete_=false; bytes_.clear(); identity_={}; began_=0; }
    bool Expired(std::uint64_t now) const {
		// End has already authenticated this bounded body. The helper advances its
		// exported revision when it emits the separate committed marker, which may
		// still be queued behind native work; expiring the body in that interval
		// would make the later marker unusable with no export retry remaining.
		return active_ && !complete_ && now>=began_ && now-began_>=15000;
	}
    bool Active() const { return active_; }
    bool Complete() const { return complete_; }
    std::size_t Offset() const { return bytes_.size(); }
    const TransferIdentity& Identity() const { return identity_; }
    const std::string& Bytes() const { return bytes_; }
private:
    TransferIdentity identity_;
    std::string bytes_;
    bool active_=false, complete_=false;
    std::uint64_t began_=0;
};
} }
