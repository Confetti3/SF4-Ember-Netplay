#pragma once
#include "../session/RoomModel.hxx"
#include "../netplay/SessionController.hxx"
#include <string>
#include <cstdint>

namespace sf4e { namespace discord {
// Copied values only: no native pointers or user-authored profile text.
struct PresenceInput {
    bool ready = false, offline = false, show = true, invites = true;
    netplay::Snapshot session;
    room::Snapshot room;
    std::string party, secret;
    std::uint64_t expires = 0, now = 0;
};
struct Presence {
    bool show = true;
    std::string activity = "Starting Ember", party, secret;
    int size = 0, capacity = 0;
    std::uint64_t expires = 0;
};
inline Presence Describe(const PresenceInput& in) {
    Presence out;
    out.show = in.show;
    if (!in.show) return out;
    out.activity = !in.ready ? "Starting Ember" : in.offline ? "Playing offline" : "In menus";
    const room::Member* local = nullptr;
    for (const auto& member : in.room.members)
        if (member.id == in.room.localMember) local = &member;
    const bool joined = local && !in.room.closed && in.session.room == netplay::RoomState::Joined;
    if (!joined) {
        if (in.session.match == netplay::MatchState::Playing)
            out.activity = local && local->status == room::MemberStatus::Watching ? "Spectating" : "Fighting";
        return out;
    }
    out.activity = "In a room";
    switch (local->status) {
    case room::MemberStatus::Queued: out.activity = "Queued"; break;
    case room::MemberStatus::Ready: out.activity = "Ready"; break;
    case room::MemberStatus::Playing: out.activity = "Fighting"; break;
    case room::MemberStatus::Watching: out.activity = "Spectating"; break;
    default: break;
    }
    out.party = in.party;
    out.size = static_cast<int>(in.room.members.size());
    out.capacity = in.room.capacity;
    if (in.invites && in.session.control == netplay::Health::Healthy && !in.room.locked &&
        out.size < out.capacity && !out.party.empty() && in.expires > in.now && in.secret.size() == 127) {
        out.secret = in.secret;
        out.expires = in.expires;
    }
    return out;
}

enum class InviteAction { None, Cancel, Switch };
enum class Next { None, Leave, Join, Expired };
// An accepted Discord invite is user intent, but cannot tear down a fight or
// silently replace a session. Generation checks also cover delayed UI actions.
class PendingInvite {
public:
    bool Offer(const std::string& secret, const std::string& party, std::uint64_t expires,
               std::uint64_t sequence, std::uint64_t generation, bool busy) {
        if (sequence <= sequence_) return false;
        sequence_ = sequence;
        if (secret == secret_) return false;
        revision_ = sequence;
        secret_ = secret; party_ = party; expires_ = expires; generation_ = generation;
        confirm_ = busy; switching_ = false;
        return true;
    }
    void Cancel() { secret_.clear(); party_.clear(); confirm_ = switching_ = false; }
    bool Matches(std::uint64_t revision) const { return Active() && revision == revision_; }
    bool Expired(std::uint64_t now) const { return Active() && expires_ <= now; }
    std::uint64_t Revision() const { return revision_; }
    bool Confirm(std::uint64_t generation, std::uint64_t revision) {
        if (!Matches(revision) || generation != generation_) return false;
        confirm_ = false; switching_ = true; return true;
    }
    Next Tick(std::uint64_t now, std::uint64_t generation, const std::string& currentParty,
              bool safeMenu, bool idle, bool canJoin) {
        if (secret_.empty()) return Next::None;
        if (expires_ <= now) { Cancel(); return Next::Expired; }
        if (!currentParty.empty() && currentParty == party_) { Cancel(); return Next::None; }
        if (generation != generation_) { Cancel(); return Next::None; }
        if (!safeMenu || confirm_) return Next::None;
        if (!idle) {
            if (switching_) return Next::Leave;
            return Next::None;
        }
        return canJoin ? Next::Join : Next::None;
    }
    void LeaveQueued() { switching_ = false; }
    bool Active() const { return !secret_.empty(); }
    bool NeedsConfirmation() const { return confirm_; }
    const std::string& Secret() const { return secret_; }
private:
    std::string secret_, party_;
    std::uint64_t expires_ = 0, sequence_ = 0, generation_ = 0, revision_ = 0;
    bool confirm_ = false, switching_ = false;
};
} }
