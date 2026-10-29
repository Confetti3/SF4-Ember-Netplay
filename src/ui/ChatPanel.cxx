#include "ApplicationShell.hxx"
#include "V2.hxx"
#include <imgui_internal.h>
#include "Theme.hxx"
#include "RoomFeedback.hxx"
#include "RoomControls.hxx"
#include "RoomBoardLayout.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sf4e { namespace ui {
namespace {
// A line of what happened in the room, in the player's language now.
std::string EventText(const ChatLine& line) {
    const std::string name = line.name.empty() ? std::string(loc::T("room.player")) : line.name;
    switch (line.kind) {
    case ChatLine::Kind::Joined: return loc::Tf("chat.event.joined", name);
    case ChatLine::Kind::Left: return loc::Tf("chat.event.left", name);
    case ChatLine::Kind::NewHost: return loc::Tf("chat.event.host", name);
    case ChatLine::Kind::GameWon: return loc::Tf("chat.event.game_won", name, line.table + 1, SetScoreText(line.score));
    case ChatLine::Kind::SetWon: return loc::Tf("chat.event.set_won", name, line.table + 1, SetScoreText(line.score));
    case ChatLine::Kind::ReadyTimeout: return loc::Tf("chat.event.ready_timeout", name, line.table + 1);
    default: return {};
    }
}
const char* ChatGateName(ChatGate gate) {
    switch (gate) {
    case ChatGate::NotJoined: return "not joined";
    case ChatGate::ReadOnly: return "read only";
    case ChatGate::Recovering: return "recovering, rebind pending";
    case ChatGate::CatchingUp: return "catching up";
    default: return "writable";
    }
}

}
// Every frame, drawn or hidden (Background), so the transcript sees each room
// snapshot whatever the player has open: the room drops a departed member's
// messages, and a join, a leave or a game won shows only as a difference
// between two snapshots. Only the session's room state and the room notices
// are read from `v`.
void ApplicationShell::ObserveChat(const ShellView& v, const room::Snapshot& room) {
    const auto epoch = v.session.room == netplay::RoomState::Idle ? 0 : room.roomEpoch;
    if (roomEpoch_ != epoch) { ResetRoomChat(room); roomEpoch_ = epoch; chatComposer_.Release(menu_); }
    if (v.session.room == netplay::RoomState::Idle || !room.roomEpoch) { transcript_.Clear(); pendingChat_.reset(); queuedChat_.reset(); muted_.clear(); mutedEpoch_ = 0; return; }
    // Seen here whether the shell is drawn or hidden (Background), so a mute
    // never outlives its room, and the unread count never leaves out a
    // member of the next room who took a muted member's id.
    if (mutedEpoch_ != room.roomEpoch) { mutedEpoch_ = room.roomEpoch; muted_.clear(); }
    if (transcript_.Update(room, v.roomNotices)) { pendingChat_.reset(); queuedChat_.reset(); }
    // The draft goes once the room's chat has the message, and not before: a send the room refuses,
    // or one that never arrives, leaves what was typed where it was. One that arrives after its
    // 8 seconds (ChatInFlight) still takes it, so what was delivered is not left to be sent again.
    if (!pendingChat_) return;
    for (auto line = transcript_.Lines().rbegin(); line != transcript_.Lines().rend(); ++line) {
        if (line->kind != ChatLine::Kind::Message) continue;
        if (line->sequence <= pendingChat_->after) break;
        if (line->own && line->text == pendingChat_->text) {
            if (pendingChat_->text == chat_) chat_[0] = 0;
            // A retry of what has arrived would only repeat it.
            if (queuedChat_ && queuedChat_->text == pendingChat_->text) queuedChat_.reset();
            pendingChat_.reset();
            return;
        }
    }
    pendingChat_->after = transcript_.LastSequence();
}
// The drawn frame's, wherever the player is, so a message that arrives while
// they are on Home still counts. Only the Chat screen on view reads it.
void ApplicationShell::UpdateChat(const ShellView& v, const Submit& submit) {
    chatComposer_.ObserveScreen(menu_);
    const bool onChat = menu_.navigation.Screen() == "room-chat";
    if (!onChat) chatOpen_ = false;
    chatTyping_ = false;
    ObserveChat(v, v.room);

    NoteChatGate(ChatGateOf(v), ImGui::GetTime());
    FlushChat(v, submit);
}

// A message entered while the room could not take it waits here, and goes out once the room can. It is
// discarded with the draft if the room closes or is left first. Only a drawn frame has
// the room to send into, so one still waiting while Ember is hidden goes when it is shown again.
void ApplicationShell::FlushChat(const ShellView& v, const Submit& submit) {
    if (!queuedChat_) return;
    const ChatGate gate = ChatGateOf(v);
    if (gate == ChatGate::NotJoined || gate == ChatGate::ReadOnly) { queuedChat_.reset(); return; }
    const double now = ImGui::GetTime();
    // A clock that went back (the device was reset) does not hold a retry for longer than its wait.
    if (gate != ChatGate::Writable || (now < queuedChat_->retryAt && queuedChat_->retryAt - now <= MaximumChatRetrySeconds)) return;
    room::Action request;
    request.table = static_cast<std::uint8_t>(selectedTable_);
    request.kind = room::ActionKind::Chat;
    request.text = queuedChat_->text;
    if (SendRoom(std::move(request), v, submit)) {
        pendingChat_ = PendingChat{queuedChat_->text, transcript_.LastSequence(), now};
        queuedChat_.reset();
        return;
    }
    if (error_.empty()) error_ = loc::T("room.action_rejected");
    queuedChat_->retryAt = now + (std::min)(static_cast<double>(++queuedChat_->attempts), MaximumChatRetrySeconds);
}

// A room under many updates can flip the fence every few frames, so a change is written at most once
// a second, with how many changes it stands for. The state that last settled is still written.
void ApplicationShell::NoteChatGate(ChatGate gate, double now) {
    if (gate != chatGate_) { chatGate_ = gate; ++chatGateChanges_; }
    if (!chatGateChanges_ || (now >= chatGateLoggedAt_ && now - chatGateLoggedAt_ < 1)) return;
    if (chatLog_)
        chatLog_(std::string("Chat: sending is ") + ChatGateName(chatGate_) + " (was " + ChatGateName(chatGateLogged_) +
            "), changes since the last line: " + std::to_string(chatGateChanges_));
    chatGateLogged_ = chatGate_;
    chatGateLoggedAt_ = now;
    chatGateChanges_ = 0;
}

std::string ApplicationShell::ChatTypingText(const ShellView& v) const {
    const auto& room = v.room;
    std::vector<std::string> names;
    for (const auto& member : room.members)
        if (RoomTypingMs(member, v.roomElapsedMs) && member.id != room.localMember && !muted_.count(member.id))
            names.push_back(member.name.empty() ? std::string(loc::T("room.player")) : member.name);
    if (names.empty()) return {};
    if (names.size() == 1) return loc::Tf("chat.typing.one", names[0]);
    if (names.size() == 2) return loc::Tf("chat.typing.two", names[0], names[1]);
    return loc::T("chat.typing.many");
}

// The transcript's lines in the current child, oldest first.
void ApplicationShell::DrawChatLog(const ShellView& view, bool compact) {
    const float s = Scale();
    bool any = false;
    const auto newest = std::find_if(transcript_.Lines().rbegin(), transcript_.Lines().rend(), [&](const ChatLine& line) {
        return line.kind == ChatLine::Kind::Message && !muted_.count(line.sender);
    });
    // The sender of the message just above with nothing between, whose name is not repeated.
    room::MemberId above = 0;
    for (const auto& line : transcript_.Lines()) {
        if (line.kind != ChatLine::Kind::Message) {
            any = true; above = 0;
            const std::string text = EventText(line);
            const float room = (std::max)(1.f, ImGui::GetContentRegionAvail().x - 8 * s);
            const float width = (std::min)(room, ImGui::CalcTextSize(text.c_str(), nullptr, false, room).x);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.f, (room - width) * .5f));
            ImGui::PushStyleColor(ImGuiCol_Text, palette::Muted);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width + 2 * s);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
            if (ImGui::IsItemVisible()) NoteUserText(line.name);
            ImGui::Dummy(ImVec2(0, 3 * s));
            continue;
        }
        // A muted member's words are kept, so unmuting brings them back, but never drawn.
        if (muted_.count(line.sender)) continue;
        any = true;
        if (above && above != line.sender) ImGui::Dummy(ImVec2(0, 6 * s));
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        // The player's own messages sit on a tinted strip with a bar at its edge.
        if (line.own) { draw->ChannelsSplit(2); draw->ChannelsSetCurrent(1); }
        if (above != line.sender) {
            const std::string name = line.name.empty() ? std::string(loc::T("room.member_left")) : line.name;
            const char* you = loc::T("room.you");
            const float tag = 12 * s, tagWidth = ImGui::GetFont()->CalcTextSizeA(tag, FLT_MAX, 0, you).x + 10 * s;
            // V2: other people's names in the brand's accent.
            ImGui::PushStyleColor(ImGuiCol_Text, line.own ? ToneColor(Tone::Success) : v2::On() ? ImGui::ColorConvertU32ToFloat4(v2::Colors().accent) : ToneColor(Tone::Pending));
            ImGui::TextUnformatted(FitLabel(name, width - (line.own ? tagWidth + 6 * s : 0)).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemVisible()) NoteUserText(line.name);
            if (line.own) {
                ImGui::SameLine(0, 6 * s);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float lineHeight = ImGui::GetTextLineHeight();
                draw->AddRectFilled(ImVec2(at.x, at.y + 1 * s), ImVec2(at.x + tagWidth, at.y + lineHeight - 1 * s), palette::Ready, 3 * s);
                const auto measured = ImGui::GetFont()->CalcTextSizeA(tag, FLT_MAX, 0, you);
                draw->AddText(ImGui::GetFont(), tag, ImVec2(at.x + 5 * s, at.y + (lineHeight - measured.y) * .5f), IM_COL32(20, 19, 18, 255), you);
                ImGui::Dummy(ImVec2(tagWidth, lineHeight));
            }
        }
        ImGui::TextWrapped("%s", line.text.c_str());
        if (ImGui::IsItemVisible()) NoteUserText(line.text, UserTextRole::Chat);
        const auto bounds = ImGui::GetCurrentWindow()->ClipRect;
        const auto bottom = ImGui::GetItemRectMax();
        const bool attended = !compact || chatComposer_.Typing() || ImGui::IsWindowHovered() || menu_.navigation.Focus() == "inline-chat";
        if (newest != transcript_.Lines().rend() && line.sequence == newest->sequence && attended && view.focused && !ImGui::GetIO().AppFocusLost &&
            !menu_.NoticeOpen() && !menu_.navigation.Asking() && !ImGui::GetTopMostPopupModal() &&
            ImGui::IsItemVisible() && bottom.y <= bounds.Max.y && ImGui::GetItemRectMin().y >= bounds.Min.y)
            transcript_.MarkRead();
        if (line.own) {
            const ImVec2 end = ImGui::GetCursorScreenPos();
            draw->ChannelsSetCurrent(0);
            draw->AddRectFilled(ImVec2(start.x - 4 * s, start.y), ImVec2(start.x + width, end.y), IM_COL32(164, 206, 160, 26), 3 * s);
            draw->AddRectFilled(ImVec2(start.x - 4 * s, start.y), ImVec2(start.x - 2 * s, end.y), palette::Ready);
            draw->ChannelsMerge();
        }
        ImGui::Dummy(ImVec2(0, 2 * s));
        above = line.sender;
    }
    if (!any) ImGui::TextWrapped("%s", loc::T(compact ? "room.no_messages" : "chat.empty"));
}

// The Chat screen: the whole conversation, oldest at the top, and the message
// box under it. Typing goes straight into the box and Enter sends (the
// compose row's Select); Up and Down scroll the conversation with the keys or
// the pad, as the mouse wheel does.
void ApplicationShell::DrawChatScreen(const ShellView& v, const std::vector<MenuEntry>&, MenuNavigation&, MenuAction& action, float height,
                                      const MenuVisualFeedback&) {
    const float s = Scale(), spacing = ImGui::GetStyle().ItemSpacing.y;
    const ChatGate gate = ChatGateOf(v);
    // A tournament match's watcher only reads: no box and no counter, and the note says why.
    const bool box = gate != ChatGate::ReadOnly;
    const float available = ImGui::GetContentRegionAvail().x, width = (std::min)(available, 900 * s);
    const float x = ImGui::GetCursorPosX() + (available - width) * .5f;
    // What goes under the conversation: the box, then one wrapped line of help
    // (or why nothing can be sent) with the room left in the message at its right.
    const float counterWidth = box ? ImGui::CalcTextSize(loc::Tf("chat.remaining", ChatBytesLeft()).c_str()).x + 12 * s : 0.f;
    // The box sits under a line kept for who is typing, so it never moves when that shows.
    const bool showTyping = height >= RoomChatMinimumHeight() + 4 * ImGui::GetTextLineHeightWithSpacing();
    const float boxHeight = box ? ImGui::GetFrameHeight() + (showTyping ? ImGui::GetTextLineHeight() + spacing : 0.f) : 0.f;
    std::string note = loc::T("chat.detail");
    Tone noteTone = Tone::Neutral;
    // The box takes typing whatever the fence does. What it says follows the status line's held feedback
    // (roomUpdateVisible_), so a brief catch-up says nothing and a fence flickering open does not flicker it.
    const bool sending = queuedChat_ || ChatInFlight(ImGui::GetTime());
    const bool waiting = gate == ChatGate::Recovering || roomUpdateVisible_;
    if (!box) note = loc::T("room.tournament_watching");
    else if (gate == ChatGate::NotJoined || (waiting && !sending)) { note = RoomWaitReason(v); noteTone = Tone::Pending; }
    else if (sending) { note = loc::T("chat.sending"); noteTone = Tone::Pending; }
    const float wrap = (std::max)(1.f, width - counterWidth);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float rest = height - boxHeight - 2 * spacing - 2 * s;
    float noteHeight = (std::min)(4 * lineHeight, (std::max)(2 * lineHeight, (std::max)(
        ImGui::CalcTextSize(loc::T("chat.detail"), nullptr, false, wrap).y, ImGui::CalcTextSize(note.c_str(), nullptr, false, wrap).y)));
    // In a window too short for both, the help gives way to the conversation, down to the counter's line.
    if (rest - noteHeight < 90 * s) noteHeight = lineHeight;
    const float logHeight = (std::max)(1.f, rest - noteHeight);
    const ImVec2 noteSize = ImGui::CalcTextSize(note.c_str(), nullptr, false, wrap);
    // A short window clips the help to its line by design, so only what is drawn is reported.
    ReportMenuText("chat-note", (std::min)(noteSize.y, noteHeight), noteHeight, noteSize.x, wrap);

    ImGui::SetCursorPosX(x);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 6 * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v2::BoardGround(v.SeeThrough()));
    ImGui::BeginChild("Chat transcript", ImVec2(width, logHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNavInputs);
    ReportMenuCard("chat-transcript", ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    // Opening the screen shows the newest message; after that the view follows new ones only while it is at the bottom.
    const bool first = !chatOpen_;
    chatOpen_ = true;
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1;
    DrawChatLog(v, false);
    ImGui::Dummy(ImVec2(0, 2 * s));
    const unsigned held = menu_.Held();
    const float step = ImGui::GetTextLineHeightWithSpacing() * 14 * ImGui::GetIO().DeltaTime;
    bool up = held & MenuInput::Up;
    if (up) ImGui::SetScrollY(ImGui::GetScrollY() - step);
    if (held & MenuInput::Down) ImGui::SetScrollY(ImGui::GetScrollY() + step);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) { up = true; ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetWindowHeight() * .9f); }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(ImGui::GetScrollY() + ImGui::GetWindowHeight() * .9f);
    if (!up && (first || atBottom)) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    ImGui::PopStyleColor(); ImGui::PopStyleVar();

    if (box) {
        if (showTyping) {
            const std::string typing = FitLabel(ChatTypingText(v), width);
            ImGui::SetCursorPosX(x);
            ImGui::PushStyleColor(ImGuiCol_Text, palette::Muted);
            ImGui::TextUnformatted(typing.c_str());
            ImGui::PopStyleColor();
            if (!typing.empty()) NoteUserText(typing);
        }
        ImGui::SetCursorPosX(x);
        DrawComposer(v, action, width, false);
        DrawChatBox(x, width, gate != ChatGate::NotJoined, noteHeight);
    }
    ImGui::SetCursorPosX(x);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(at, ImVec2(at.x + width, at.y + noteHeight), true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, ImGui::ColorConvertFloat4ToU32(ToneColor(noteTone)), note.c_str(), nullptr, wrap);
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(width, noteHeight));
}

int ApplicationShell::ChatBytesLeft() const {
    return static_cast<int>(room::MaximumChatBytes) - static_cast<int>(std::strlen(chat_));
}

void ApplicationShell::DrawChatBox(float x, float width, bool typable, float noteHeight) {
    const float s = Scale(), lineHeight = ImGui::GetTextLineHeight();
    // Counted before the box takes this frame's typing, as the screen's layout was.
    const int left = ChatBytesLeft();
    const std::string counter = loc::Tf("chat.remaining", left);
    const float counterWidth = ImGui::CalcTextSize(counter.c_str()).x;
    ReportMenuText("chat-remaining", lineHeight, lineHeight, counterWidth, width * .4f);
    // The counter, at the right of the note line under the box; the cursor stays at that line.
    ImGui::SetCursorPosX(x);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImU32 counterColor = left <= 0 ? ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Error)) :
        left <= 32 ? ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)) : palette::Muted;
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(at, ImVec2(at.x + width, at.y + noteHeight), true);
    draw->AddText(ImVec2(at.x + width - counterWidth, at.y), counterColor, counter.c_str());
    draw->PopClipRect();
}
bool ApplicationShell::ChatSendPending() const {
    return queuedChat_ || (ChatInFlight(ImGui::GetTime()) && pendingChat_->text == chat_);
}
void ApplicationShell::DrawComposer(const ShellView& v, MenuAction& action, float width, bool inlineBox) {
    const std::string before = chat_;
    const bool activate = action.kind == MenuAction::Activate && action.id == "inline-chat";
    if (activate) action = {};
    const auto result = chatComposer_.Draw(menu_, chat_, v.room.roomEpoch, width,
        inlineBox ? "inline-chat" : "chat-box", inlineBox, ChatTypable(v), ChatSendPending(), activate);
    if (before != chat_) chatEditedAt_ = ImGui::GetTime();
    chatTyping_ = chatComposer_.Typing() && result != ChatComposer::Result::Sent && ChatTypable(v) && room_controls::HasChatText(chat_) && !ChatSendPending() && ImGui::GetTime() - chatEditedAt_ < 4;
    if (result == ChatComposer::Result::Sent) action = {MenuAction::Activate, "compose"};
    else if (result == ChatComposer::Result::Released) action = inlineBox ? MenuAction{} : MenuAction{MenuAction::Back};
}
void ApplicationShell::DrawRoomChat(const ShellView& v, MenuAction& action, float height) {
    const bool box = ChatGateOf(v) != ChatGate::ReadOnly;
    const float reserve = box ? RoomChatComposerHeight() : 0;
    ImGui::BeginChild("Inline transcript", ImVec2(0, (std::max)(1.f, height - reserve)), 0, ImGuiWindowFlags_NoNavInputs);
    const bool follow = !chatShown_ || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1;
    DrawChatLog(v, true);
    if (follow) ImGui::SetScrollHereY(1.f);
    chatShown_ = transcript_.Lines().size();
    ImGui::EndChild();
    if (box) {
        ImGui::TextUnformatted(FitLabel(ChatTypingText(v), ImGui::GetContentRegionAvail().x).c_str());
        DrawComposer(v, action, ImGui::GetContentRegionAvail().x, true);
    }
}
} }
