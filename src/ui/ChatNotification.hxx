#pragma once
#include "ChatTranscript.hxx"
#include "Theme.hxx"

namespace sf4e { namespace ui {
// The count on a Chat control: a rounded badge ending at right (screen x),
// its top at top, the height of a line of text. UnreadBadgeWidth is what
// it takes, for the text beside it to leave room.
float UnreadBadgeWidth(unsigned count);
void DrawUnreadBadge(float right,float top,unsigned count);
struct ChatNotificationView {
    unsigned unread = 0;
    bool expanded = false;
};
// An attention notice is independent of the read watermark. Observe every
// presentation frame, including while menus or a fighter's match hide it.
class ChatNotificationState {
public:
    static constexpr double Seconds = 4;
    ChatNotificationView Update(const ChatTranscript& transcript,const std::set<room::MemberId>& muted,double now);

private:
    std::uint64_t epoch_ = 0, sequence_ = 0;
    double until_ = 0, lastArrival_ = -Seconds;
};
void DrawChatNotification(const ChatNotificationView& view,const MatchStripView& match);
} }
