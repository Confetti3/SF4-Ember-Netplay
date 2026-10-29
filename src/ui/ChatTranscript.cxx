#include "ChatTranscript.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
void ChatTranscript::Clear() {
    lines_.clear();
    epoch_ = lastSequence_ = readThrough_ = lastNotice_ = 0;
    host_ = 0;
    names_.clear();
    present_.clear();
    tables_ = {};
    ++linesRevision_;
}
void ChatTranscript::Append(ChatLine line) {
    lines_.push_back(std::move(line));
    while (lines_.size() > MaximumLines) lines_.pop_front();
    ++linesRevision_;
}
void ChatTranscript::MemberIds(const room::Snapshot& snapshot, std::vector<room::MemberId>& ids) {
    ids.clear();
    for (const auto& member : snapshot.members) ids.push_back(member.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}
ChatLine ChatTranscript::EventLine(ChatLine::Kind kind, room::MemberId who) const {
    ChatLine line;
    line.kind = kind;
    line.sender = who;
    const auto name = names_.find(who);
    if (name != names_.end()) line.name = name->second;
    return line;
}
bool ChatTranscript::Update(const room::Snapshot& snapshot, const std::vector<RoomNotice>& notices) {
    const bool fresh = snapshot.roomEpoch != epoch_;
    if (fresh) { Clear(); epoch_ = snapshot.roomEpoch; }
    if (!snapshot.roomEpoch) return fresh;
    for (const auto& member : snapshot.members) names_[member.id] = member.name;
    MemberIds(snapshot, now_);
    // What happened in the room comes first: a member who joined and spoke in
    // the same snapshot is announced before the words.
    if (!fresh) Diff(snapshot);
    // Notices already kept when this player arrived are another room's, or history.
    for (const auto& notice : notices) {
        if (notice.sequence <= lastNotice_) continue;
        lastNotice_ = notice.sequence;
        if (fresh || notice.event.kind != room::Event::Kind::ReadyTimeout) continue;
        auto line = EventLine(ChatLine::Kind::ReadyTimeout, notice.event.member);
        line.table = notice.event.table;
        Append(std::move(line));
    }
    // The room numbers its messages from 1 and keeps the newest hundred.
    for (const auto& message : snapshot.chat) {
        if (message.sequence <= lastSequence_) continue;
        ChatLine line;
        line.sequence = message.sequence;
        line.sender = message.sender;
        const auto name = names_.find(message.sender);
        if (name != names_.end()) line.name = name->second;
        line.text = message.text;
        line.own = snapshot.localMember && message.sender == snapshot.localMember;
        Append(std::move(line));
        lastSequence_ = message.sequence;
    }
    // What was already said when this player arrived is history, not news.
    if (fresh) readThrough_ = lastSequence_;
    host_ = snapshot.host;
    present_.swap(now_);
    for (std::size_t i = 0; i < snapshot.tables.size(); ++i) {
        const auto& table = snapshot.tables[i];
        auto& seen = tables_[i];
        seen.p1 = table.p1; seen.p2 = table.p2;
        seen.score[0] = table.score[0]; seen.score[1] = table.score[1];
        seen.setGeneration = table.lastSet.generation;
    }
    return fresh;
}
void ChatTranscript::Diff(const room::Snapshot& snapshot) {
    // now_ holds this snapshot's members (Update), present_ the last one's.
    const auto in = [](const std::vector<room::MemberId>& ids, room::MemberId id) { return std::binary_search(ids.begin(), ids.end(), id); };
    // A member who left and one who was removed read the same: the snapshot does not say which.
    for (const auto id : present_) if (!in(now_, id)) Append(EventLine(ChatLine::Kind::Left, id));
    for (const auto& member : snapshot.members) if (!in(present_, member.id)) Append(EventLine(ChatLine::Kind::Joined, member.id));
    // Only a member with the host's powers is announced, never a watcher left holding the field.
    const auto host = room::ActingHost(snapshot);
    if (host_ && host && host != host_) Append(EventLine(ChatLine::Kind::NewHost, host));
    for (std::size_t i = 0; i < snapshot.tables.size(); ++i) {
        const auto& table = snapshot.tables[i];
        const auto& seen = tables_[i];
        // A finished set names its winner and the set's score. Otherwise a game
        // won shows as one seat's score going up between two snapshots with the
        // same pair seated; a draw, a cancelled game or both seats moving say nothing.
        if (table.lastSet.generation && table.lastSet.generation != seen.setGeneration) {
            const auto winner = table.lastSet.Winner();
            if (!winner || !names_.count(winner)) continue;
            auto line = EventLine(ChatLine::Kind::SetWon, winner);
            line.table = static_cast<unsigned>(i);
            const int seat = table.lastSet.winnerSeat;
            line.score[0] = table.lastSet.score[seat]; line.score[1] = table.lastSet.score[1 - seat];
            Append(std::move(line));
        } else if (seen.p1 == table.p1 && seen.p2 == table.p2) {
            const bool first = table.score[0] > seen.score[0], second = table.score[1] > seen.score[1];
            const auto winner = first ? table.p1 : table.p2;
            if (first == second || !winner || !names_.count(winner)) continue;
            auto line = EventLine(ChatLine::Kind::GameWon, winner);
            line.table = static_cast<unsigned>(i);
            const int seat = first ? 0 : 1;
            line.score[0] = table.score[seat]; line.score[1] = table.score[1 - seat];
            Append(std::move(line));
        }
    }
}
bool ChatTranscript::IsUnread(const ChatLine& line,const std::set<room::MemberId>& muted) const {
    return line.kind==ChatLine::Kind::Message&&line.sequence>readThrough_&&!line.own&&!muted.count(line.sender);
}
std::uint64_t ChatTranscript::NewestUnreadSequence(const std::set<room::MemberId>& muted) const {
    for(auto line=lines_.rbegin();line!=lines_.rend();++line)if(IsUnread(*line,muted))return line->sequence;
    return 0;
}
unsigned ChatTranscript::Unread(const std::set<room::MemberId>& muted) const {
    if (unreadKnown_ && unreadRevision_ == linesRevision_ && unreadThrough_ == readThrough_ && unreadMuted_ == muted) return unread_;
    unsigned count = 0;
    for (const auto& line : lines_)
        if (IsUnread(line,muted)) ++count;
    unread_ = count; unreadKnown_ = true;
    unreadRevision_ = linesRevision_; unreadThrough_ = readThrough_;
    if (unreadMuted_ != muted) unreadMuted_ = muted;
    return count;
}
} }
