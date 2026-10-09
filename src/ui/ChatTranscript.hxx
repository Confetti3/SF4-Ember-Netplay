#pragma once
// The room chat as this client keeps it: the messages in the order they
// arrived, with lines for what happened in the room between them. Most of it
// is built from the snapshots the client already receives (by message
// sequence number, and from snapshot-to-snapshot differences), so no other
// member sees it. What a snapshot cannot show, a fighter the room took out of
// a seat for not readying, arrives as a room event the runtime keeps as a
// RoomNotice. A member's messages stay after they leave, under the name they
// had.
#include "../session/RoomModel.hxx"
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ChatLine {
    enum class Kind { Message, Joined, Left, NewHost, GameWon, SetWon, ReadyTimeout } kind = Kind::Message;
    // Messages: the room's sequence number, who sent it and under which name.
    std::uint64_t sequence = 0;
    room::MemberId sender = 0;
    std::string name;
    std::string text;
    bool own = false;
    // Results and Ready timeouts: the table (from 0). Results: the score with
    // the winner's first.
    unsigned table = 0;
    std::uint32_t score[2] = {0, 0};
};
// A room event for the chat (Event::Kind::ReadyTimeout), numbered by the
// runtime from 1 in the order it arrived.
struct RoomNotice {
    std::uint64_t sequence = 0;
    room::Event event;
};
class ChatTranscript {
public:
    static constexpr std::size_t MaximumLines = 200;
    // Reads one snapshot and the runtime's recent notices. A different room
    // starts a new transcript from that snapshot's chat, with no event lines
    // and nothing unread. Returns true when it did.
    bool Update(const room::Snapshot& snapshot, const std::vector<RoomNotice>& notices);
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
    ChatLine EventLine(ChatLine::Kind kind, room::MemberId who) const;
    void Diff(const room::Snapshot& snapshot);
    struct SeenTable { room::MemberId p1 = 0, p2 = 0; std::uint32_t score[2] = {0, 0}; std::uint64_t setGeneration = 0; };
    std::deque<ChatLine> lines_;
    std::uint64_t epoch_ = 0, lastSequence_ = 0, readThrough_ = 0, lastNotice_ = 0;
    room::MemberId host_ = 0;
    // Every name seen in this room, so a result or a message can name a member who has since left.
    std::map<room::MemberId, std::string> names_;
    std::set<room::MemberId> present_;
    std::array<SeenTable, room::TableCount> tables_;
};
} }
