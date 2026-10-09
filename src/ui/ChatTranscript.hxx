#pragma once
// The room chat as this client keeps it: the messages in the order they
// arrived, with lines for what happened in the room between them. It is built
// only from the snapshots the client already receives (by message sequence
// number, and from snapshot-to-snapshot differences), so nothing is added to
// the room protocol and no other member sees it. A member's messages stay
// after they leave, under the name they had.
#include "../session/RoomModel.hxx"
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace sf4e { namespace ui {
struct ChatLine {
    enum class Kind { Message, Joined, Left, NewHost, GameWon, SetWon, ReadyTimeout } kind = Kind::Message;
    // Messages: the room's sequence number, who sent it and under which name.
    std::uint64_t sequence = 0;
    room::MemberId sender = 0;
    std::string name;
    std::string text;
    bool own = false;
    // Results: the table (from 0), and the score with the winner's first.
    unsigned table = 0;
    std::uint32_t score[2] = {0, 0};
};
class ChatTranscript {
public:
    static constexpr std::size_t MaximumLines = 200;
    // Reads one snapshot. A different room starts a new transcript from that
    // snapshot's chat, with no event lines and nothing unread. Returns true when
    // it did.
    bool Update(const room::Snapshot& snapshot);
    void Clear();
    const std::deque<ChatLine>& Lines() const { return lines_; }
    // The newest message sequence taken in, or 0 before any.
    std::uint64_t LastSequence() const { return lastSequence_; }
    // Messages from others, newer than the last MarkRead, that the player has
    // not muted.
    unsigned Unread(const std::set<room::MemberId>& muted) const;
    void MarkRead() { readThrough_ = lastSequence_; }
private:
    void Append(ChatLine line);
    void Diff(const room::Snapshot& snapshot);
    struct SeenTable { room::MemberId p1 = 0, p2 = 0; std::uint32_t score[2] = {0, 0}; std::uint64_t setGeneration = 0, readyTimeoutRevision = 0; };
    std::deque<ChatLine> lines_;
    std::uint64_t epoch_ = 0, lastSequence_ = 0, readThrough_ = 0;
    room::MemberId host_ = 0;
    // Every name seen in this room, so a result or a message can name a member who has since left.
    std::map<room::MemberId, std::string> names_;
    std::set<room::MemberId> present_;
    std::array<SeenTable, room::TableCount> tables_;
};
} }
