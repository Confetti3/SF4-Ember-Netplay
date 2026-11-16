#pragma once
#include "../session/RoomModel.hxx"
#include "../netplay/SessionController.hxx"
#include <algorithm>
#include <string>
#include <cstdint>

namespace sf4e { namespace discord {
// No native pointers or user-authored profile text. The session and room are
// read in place for the one Describe call, not copied.
struct PresenceInput {
    PresenceInput(const netplay::Snapshot& session, const room::Snapshot& room) : session(session), room(room) {}
    bool ready = false, offline = false, show = true, invites = true;
    const netplay::Snapshot& session;
    const room::Snapshot& room;
    std::string party, secret;
    std::uint64_t expires = 0, now = 0;
};
struct Presence {
    bool show = true;
    std::string activity = "Starting Ember", party, secret;
    int size = 0, capacity = 0;
    std::uint64_t expires = 0;
    // The same description, typed, for what is built from it besides the
    // companion's text (netplay/FriendPresence.hxx). `status` and `training`
    // are the local member's, in a room; `held` is what keeps that room from
    // being offered to friends (a bound match, unhealthy, locked, full), None when it can be.
    enum class Where : std::uint8_t { Starting, Menus, Offline, Room } where = Where::Starting;
    room::MemberStatus status = room::MemberStatus::Idle;
    enum class Held : std::uint8_t { None, Match, Unhealthy, Locked, Full } held = Held::None;
    bool training = false;
    bool Offerable() const { return where == Where::Room && held == Held::None; }
};
inline Presence Describe(const PresenceInput& in) {
    Presence out;
    out.show = in.show;
    if (!in.show) return out;
    out.activity = !in.ready ? "Starting Ember" : in.offline ? "Playing offline" : "In menus";
    out.where = !in.ready ? Presence::Where::Starting : in.offline ? Presence::Where::Offline : Presence::Where::Menus;
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
    out.where = Presence::Where::Room;
    out.status = local->status;
    out.training = local->training;
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
    // A bound match's room is never offered, whichever way the player came into it
    // (before it is bound, the runtime passes no secret).
    out.held = in.room.tournament.Active() ? Presence::Held::Match : in.session.control != netplay::Health::Healthy ? Presence::Held::Unhealthy :
        in.room.locked ? Presence::Held::Locked : out.size >= out.capacity ? Presence::Held::Full : Presence::Held::None;
    if (in.invites && out.Offerable() && !out.party.empty() && in.expires > in.now && in.secret.size() == 127) {
        out.secret = in.secret;
        out.expires = in.expires;
    }
    return out;
}

enum class InviteAction { None, Cancel, Switch };
// Where a room the player is asked to go to came from: a Discord invitation, an invite a friend sent, or the player's own Join on a
// friend's private room (which the player already answered by pressing it).
enum class InviteOrigin : std::uint8_t { Discord, Friend, OwnJoin };
// The catalog id of the words for an invitation that ran out before it was used.
inline const char* ExpiredWords(InviteOrigin origin) { return origin == InviteOrigin::Discord ? "discord.invitation_expired" : "room.invitation_expired"; }
enum class Next { None, Leave, Join, Expired };
// An accepted Discord invite is user intent, but cannot tear down a fight or
// silently replace a session. Generation checks also cover delayed UI actions.
class PendingInvite {
public:
    // The companion's invitation: `sequence` is its own count, and an older one is ignored.
    bool Offer(const std::string& secret, const std::string& party, std::uint64_t expires,
               std::uint64_t sequence, std::uint64_t generation, bool busy) {
        if (sequence <= sequence_) return false;
        sequence_ = sequence;
        return Accept(secret, party, expires, generation, busy, InviteOrigin::Discord);
    }
    // A friend's room (netplay/FriendVisits.hxx): an invite of theirs or the player's own Join, which the companion's count does not
    // order. Its revision follows the last one's, so a command made for an earlier invitation still misses.
    bool OfferFromFriend(const std::string& secret, const std::string& party, std::uint64_t expires,
                         std::uint64_t generation, bool busy, InviteOrigin origin) {
        return Accept(secret, party, expires, generation, busy, origin);
    }
    void Cancel() { secret_.clear(); party_.clear(); origin_ = InviteOrigin::Discord; confirm_ = switching_ = false; }
    InviteOrigin From() const { return origin_; }
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
    bool Accept(const std::string& secret, const std::string& party, std::uint64_t expires,
                std::uint64_t generation, bool busy, InviteOrigin origin) {
        if (secret == secret_) return false;
        revision_ = std::max(revision_ + 1, sequence_);
        secret_ = secret; party_ = party; expires_ = expires; generation_ = generation;
        origin_ = origin;
        confirm_ = busy; switching_ = false;
        return true;
    }
    std::string secret_, party_;
    InviteOrigin origin_ = InviteOrigin::Discord;
    std::uint64_t expires_ = 0, sequence_ = 0, generation_ = 0, revision_ = 0;
    bool confirm_ = false, switching_ = false;
};
} }
