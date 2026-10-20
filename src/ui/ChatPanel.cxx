#include "ApplicationShell.hxx"
#include <imgui_internal.h>
#include "Theme.hxx"
#include "RoomFeedback.hxx"
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
bool HasText(const char* text) {
    for (; *text; ++text) if (!std::isspace(static_cast<unsigned char>(*text))) return true;
    return false;
}
}
// Every frame, drawn or hidden (Background), so the transcript sees each room
// snapshot whatever the player has open: the room drops a departed member's
// messages, and a join, a leave or a game won shows only as a difference
// between two snapshots. Only the session's room state and the room notices
// are read from `v`.
void ApplicationShell::ObserveChat(const ShellView& v, const room::Snapshot& room) {
    const auto epoch = v.session.room == netplay::RoomState::Idle ? 0 : room.roomEpoch;
    if (roomEpoch_ != epoch) {
        chatComposer_.Release(menu_); roomEpoch_ = epoch; chatShown_ = 0; chat_[0] = 0;
        selectedTable_ = 0; rulesDirty_ = false; leaveAsk_ = -1; leaveAsked_ = false;
        roomUpdateUntil_ = 0; roomUpdateStarted_ = -1; roomDetails_.clear();
        std::snprintf(roomName_,sizeof(roomName_),"%s",room.name.c_str()); roomCapacity_ = room.capacity;
        const auto local=std::find_if(room.members.begin(),room.members.end(),[&](const room::Member& m){return m.id==room.localMember;});
        if (local!=room.members.end()&&local->table>=0&&local->table<static_cast<int>(room.tables.size())) selectedTable_=local->table;
    }
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
// they are on Home still counts. DrawChatLog acknowledges visible messages.
void ApplicationShell::UpdateChat(const ShellView& v, const Submit& submit) {
    chatComposer_.ObserveScreen(menu_);
    if (menu_.navigation.Screen() != "room-chat") chatOpen_ = false;
    ObserveChat(v, v.room);
    NoteChatGate(ChatGateOf(v), ImGui::GetTime());
    FlushChat(v, submit);
}


ChatComposer::Result ApplicationShell::DrawChatComposer(const ShellView& v,float width,bool inlineBox,bool activate) {
    const bool typable=ChatTypable(v)&&!menu_.NoticeOpen()&&!menu_.navigation.Asking();
    const bool pending=queuedChat_||(ChatInFlight(ImGui::GetTime())&&pendingChat_->text==chat_);
    return chatComposer_.Draw(menu_,chat_,v.room.roomEpoch,width,inlineBox,typable,pending,activate);
}

void ApplicationShell::DrawRoomChat(const ShellView& v, MenuAction& action, float height) {
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    const bool selected = action.kind == MenuAction::Activate && action.id == "inline-chat";
    if (selected) action = {};
    // On very short layouts the editor already identifies chat. Let the
    // heading yield before the transcript becomes too short to read a line.
    const bool heading=height>=2*ImGui::GetTextLineHeight()+ImGui::GetFrameHeight()+3*gap+12*Scale();
    if (heading) {
        ImGui::TextUnformatted(loc::T("room.chat_heading"));
        const auto unread=transcript_.Unread(muted_);
        if(unread)DrawUnreadBadge(ImGui::GetItemRectMax().x+8*Scale()+UnreadBadgeWidth(unread),ImGui::GetItemRectMin().y,unread);
    }
    const bool box=ChatGateOf(v)!=ChatGate::ReadOnly;
    const float logHeight=(std::max)(1.f,height-(box?ImGui::GetFrameHeight()+gap:0)-(heading?ImGui::GetTextLineHeight()+gap:0));
    ImGui::BeginChild("Recent chat", ImVec2(0, logHeight), 0, ImGuiWindowFlags_NoNavInputs);
    const bool bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1;
    DrawChatLog(v, true);
    if (bottom || chatShown_ == 0) ImGui::SetScrollHereY(1.f);
    chatShown_ = transcript_.Lines().size();
    ImGui::EndChild();
    if (box) switch (DrawChatComposer(v, ImGui::GetContentRegionAvail().x, true, selected)) {
    case ChatComposer::Result::Sent: action = {MenuAction::Activate, "compose"}; break;
    case ChatComposer::Result::Released: action = {}; break;
    default: break;
    }
}

// A message entered while the room could not take it waits here, and goes out once the room can. It is
// let go, with the draft kept until leaving the room (Idle) or changing its epoch, only if the room closes or is left first. Only a drawn frame has
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

// The transcript's lines in the current child, oldest first.
void ApplicationShell::DrawChatLog(const ShellView& v, bool compact) {
    const float s = Scale();
    bool any = false;
    // The sender of the message just above with nothing between, whose name is not repeated.
    room::MemberId above = 0;
    bool newestVisible = false;
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
            ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(line.own ? Tone::Success : Tone::Pending));
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
        // Check the newest unmuted message after drawing, not the previous
        // frame's scroll maximum. A newly appended message may still be below it.
        newestVisible = ImGui::IsItemVisible() &&
            ImGui::GetItemRectMax().y <= ImGui::GetCurrentWindow()->ClipRect.Max.y;
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
    // The compact preview needs attention (mouse or controller focus); merely
    // passing through the room board does not read chat. Both chat layouts use
    // the same visibility, focus and dialog gates.
    const bool attended = !compact || menu_.navigation.Focus() == "inline-chat" || ImGui::IsWindowHovered();
    if (newestVisible && attended && v.focused &&
        !menu_.NoticeOpen() && !menu_.navigation.Asking() && !ImGui::GetTopMostPopupModal())
        transcript_.MarkRead();
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
    const bool box = gate != ChatGate::ReadOnly;
    const bool typable = ChatTypable(v);
    const float available = ImGui::GetContentRegionAvail().x, width = (std::min)(available, 900 * s);
    const float x = ImGui::GetCursorPosX() + (available - width) * .5f;
    // What goes under the conversation: the box, then one wrapped line of help
    // (or why nothing can be sent) with the room left in the message at its right.
    const int left = static_cast<int>(room::MaximumChatBytes) - static_cast<int>(std::strlen(chat_));
    const std::string counter = loc::Tf("chat.remaining", left);
    const float counterWidth = box ? ImGui::CalcTextSize(counter.c_str()).x : 0.f;
    std::string note = loc::T("chat.detail");
    Tone noteTone = Tone::Neutral;
    // The box takes typing whatever the fence does. What it says follows the status line's held feedback
    // (roomUpdateVisible_), so a brief catch-up says nothing and a fence flickering open does not flicker it.
    const bool sending = queuedChat_ || ChatInFlight(ImGui::GetTime());
    const bool waiting = gate == ChatGate::Recovering || roomUpdateVisible_;
    if (!box) note = loc::T("room.tournament_watching");
    else if (!typable || (waiting && !sending)) { note = RoomWaitReason(v); noteTone = Tone::Pending; }
    else if (sending) { note = loc::T("chat.sending"); noteTone = Tone::Pending; }
    const float wrap = (std::max)(1.f, width - counterWidth - 12 * s);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float boxHeight = box ? ImGui::GetFrameHeight() : 0.f;
    const float rest = height - boxHeight - 2 * spacing - 2 * s;
    float noteHeight = (std::min)(4 * lineHeight, (std::max)(2 * lineHeight, (std::max)(
        ImGui::CalcTextSize(loc::T("chat.detail"), nullptr, false, wrap).y, ImGui::CalcTextSize(note.c_str(), nullptr, false, wrap).y)));
    // In a window too short for both, the help gives way to the conversation, down to the counter's line.
    if (rest - noteHeight < 90 * s) noteHeight = lineHeight;
    const float logHeight = (std::max)(1.f, rest - noteHeight);
    ReportMenuText("chat-remaining", lineHeight, lineHeight, counterWidth, width * .4f);
    const ImVec2 noteSize = ImGui::CalcTextSize(note.c_str(), nullptr, false, wrap);
    // A short window clips the help to its line by design, so only what is drawn is reported.
    ReportMenuText("chat-note", (std::min)(noteSize.y, noteHeight), noteHeight, noteSize.x, wrap);

    ImGui::SetCursorPosX(x);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 6 * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BoardPanelColor(v.SeeThrough()));
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

    ImGui::SetCursorPosX(x);
    if (box) switch (DrawChatComposer(v, width, false)) {
    case ChatComposer::Result::Sent: action = {MenuAction::Activate, "compose"}; break;
    case ChatComposer::Result::Released: action = menu_.navigation.Return(); break;
    default: break;
    }

    // The history screen keeps its explicit pad shortcuts. Keyboard letters
    // were already removed by GameMenu while the editor owned the keys.
    const unsigned shortcuts=menu_.TextPressed()&(MenuInput::Chat|MenuInput::Fighter|MenuInput::Options);
    if (shortcuts) {
        chatComposer_.Release(menu_);
        action={MenuAction::Shortcut};
        action.delta=shortcuts&MenuInput::Chat?MenuInput::Chat:shortcuts&MenuInput::Fighter?MenuInput::Fighter:MenuInput::Options;
    }

    ImGui::SetCursorPosX(x);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImU32 counterColor = left <= 0 ? ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Error)) :
        left <= 32 ? ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)) : palette::Muted;
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(at, ImVec2(at.x + width, at.y + noteHeight), true);
    if (box) draw->AddText(ImVec2(at.x + width - counterWidth, at.y), counterColor, counter.c_str());
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, ImGui::ColorConvertFloat4ToU32(ToneColor(noteTone)), note.c_str(), nullptr, wrap);
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(width, noteHeight));
}
} }
