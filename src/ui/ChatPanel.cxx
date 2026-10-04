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
std::string BadgeText(unsigned count) { return count > 99 ? "99+" : std::to_string(count); }
// A line of what happened in the room, in the player's language now.
std::string EventText(const ChatLine& line) {
    const std::string name = line.name.empty() ? std::string(loc::T("room.player")) : line.name;
    switch (line.kind) {
    case ChatLine::Kind::Joined: return loc::Tf("chat.event.joined", name);
    case ChatLine::Kind::Left: return loc::Tf("chat.event.left", name);
    case ChatLine::Kind::NewHost: return loc::Tf("chat.event.host", name);
    case ChatLine::Kind::GameWon: return loc::Tf("chat.event.game_won", name, line.table + 1, SetScoreText(line.score));
    case ChatLine::Kind::SetWon: return loc::Tf("chat.event.set_won", name, line.table + 1, SetScoreText(line.score));
    default: return {};
    }
}
// ImGui selects the whole box when the keyboard focus lands on it, which would
// have the next letter replace a kept draft. The caret goes to the end instead.
int CaretToEnd(ImGuiInputTextCallbackData* data) {
    bool* fresh = static_cast<bool*>(data->UserData);
    if (*fresh) { *fresh = false; data->CursorPos = data->BufTextLen; data->SelectionStart = data->SelectionEnd = data->CursorPos; }
    return 0;
}
bool HasText(const char* text) {
    for (; *text; ++text) if (!std::isspace(static_cast<unsigned char>(*text))) return true;
    return false;
}
}
float UnreadBadgeWidth(unsigned count) {
    const float s = Scale(), height = 18 * s;
    return (std::max)(height, ImGui::GetFont()->CalcTextSizeA(13 * s, FLT_MAX, 0, BadgeText(count).c_str()).x + 12 * s);
}
void DrawUnreadBadge(float right, float top, unsigned count) {
    const float s = Scale(), height = 18 * s, size = 13 * s, width = UnreadBadgeWidth(count);
    const auto text = BadgeText(count);
    auto* d = ImGui::GetWindowDrawList();
    const ImVec2 min(right - width, top), max(right, top + height);
    d->AddRectFilled(min, max, palette::Ember, height * .5f);
    const auto measured = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    d->AddText(ImGui::GetFont(), size, ImVec2(min.x + (width - measured.x) * .5f, min.y + (height - measured.y) * .5f), IM_COL32(20, 19, 18, 255), text.c_str());
}

// Every frame, drawn or hidden (Background), so the transcript sees each room
// snapshot whatever the player has open: the room drops a departed member's
// messages, and a join, a leave or a game won shows only as a difference
// between two snapshots. Only the session's room state is read from `v`.
void ApplicationShell::ObserveChat(const ShellView& v, const room::Snapshot& room) {
    if (v.session.room == netplay::RoomState::Idle || !room.roomEpoch) { transcript_.Clear(); pendingChat_.reset(); return; }
    if (transcript_.Update(room)) pendingChat_.reset();
    // The draft goes once the room's chat has the message, and not before: a send the room refuses,
    // or one that never arrives, leaves what was typed where it was. One that arrives after its
    // 8 seconds (ChatInFlight) still takes it, so what was delivered is not left to be sent again.
    if (!pendingChat_) return;
    for (auto line = transcript_.Lines().rbegin(); line != transcript_.Lines().rend(); ++line) {
        if (line->kind != ChatLine::Kind::Message) continue;
        if (line->sequence <= pendingChat_->after) break;
        if (line->own && line->text == pendingChat_->text) {
            if (pendingChat_->text == chat_) chat_[0] = 0;
            pendingChat_.reset();
            return;
        }
    }
    pendingChat_->after = transcript_.LastSequence();
}
// The drawn frame's, wherever the player is, so a message that arrives while
// they are on Home still counts. Only the Chat screen on view reads it.
void ApplicationShell::UpdateChat(const ShellView& v) {
    const bool onChat = menu_.navigation.Screen() == "room-chat";
    if (!onChat) chatOpen_ = false;
    ObserveChat(v, v.room);
    if (onChat) transcript_.MarkRead();
}

// The transcript's lines in the current child, oldest first.
void ApplicationShell::DrawChatLog(const ShellView&, bool compact) {
    const float s = Scale();
    bool any = false;
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
void ApplicationShell::DrawChatScreen(const ShellView& v, const std::vector<MenuEntry>&, MenuNavigation&, MenuAction&, float height,
                                      const MenuVisualFeedback&) {
    const float s = Scale(), spacing = ImGui::GetStyle().ItemSpacing.y;
    const bool canSend = RoomActionsAvailable(v);
    const float available = ImGui::GetContentRegionAvail().x, width = (std::min)(available, 900 * s);
    const float x = ImGui::GetCursorPosX() + (available - width) * .5f;
    // What goes under the conversation: the box, then one wrapped line of help
    // (or why nothing can be sent) with the room left in the message at its right.
    const int left = static_cast<int>(room::MaximumChatBytes) - static_cast<int>(std::strlen(chat_));
    const std::string counter = loc::Tf("chat.remaining", left);
    const float counterWidth = ImGui::CalcTextSize(counter.c_str()).x;
    std::string note = loc::T("chat.detail");
    Tone noteTone = Tone::Neutral;
    if (!canSend) { note = RoomWaitReason(v); noteTone = Tone::Pending; }
    else if (ChatInFlight(ImGui::GetTime())) { note = loc::T("chat.sending"); noteTone = Tone::Pending; }
    const float wrap = (std::max)(1.f, width - counterWidth - 12 * s);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float boxHeight = ImGui::GetFrameHeight();
    const float rest = height - boxHeight - 2 * spacing - 2 * s;
    float noteHeight = (std::min)(4 * lineHeight, (std::max)(2 * lineHeight, (std::max)(
        ImGui::CalcTextSize(loc::T("chat.detail"), nullptr, false, wrap).y, ImGui::CalcTextSize(note.c_str(), nullptr, false, wrap).y)));
    // In a window too short for both, the help gives way to the conversation, down to the counter's line.
    if (rest - noteHeight < 90 * s) noteHeight = lineHeight;
    const float logHeight = (std::max)(1.f, rest - noteHeight);
    ReportMenuText("chat-remaining", lineHeight, lineHeight, counterWidth, width * .4f);

    ImGui::SetCursorPosX(x);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 6 * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.08f, .075f, .07f, .84f));
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

    // The message box keeps the keyboard whenever nothing else holds it. It edits
    // a copy, so Escape (which ImGui answers by restoring the text from when the
    // box was last entered) cannot take the draft back with it.
    ImGui::SetCursorPosX(x);
    ImGui::SetNextItemWidth(width);
    char buffer[sizeof(chat_)];
    std::memcpy(buffer, chat_, sizeof(buffer));
    bool fresh = false;
    if (chatBoxText_ != chat_)
        if (auto* state = ImGui::GetInputTextState(ImGui::GetID("##chat-draft"))) state->ReloadUserBufAndMoveToEnd();
    ImGui::BeginDisabled(!canSend);
    if (canSend && !menu_.NoticeOpen() && !ImGui::IsAnyItemActive()) { ImGui::SetKeyboardFocusHere(); fresh = true; }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * s);
    const bool edited = ImGui::InputTextWithHint("##chat-draft", loc::T("chat.hint"), buffer, sizeof(buffer), ImGuiInputTextFlags_CallbackAlways, CaretToEnd, &fresh);
    ImGui::PopStyleVar();
    const ImVec2 boxMin = ImGui::GetItemRectMin(), boxMax = ImGui::GetItemRectMax();
    if (ImGui::IsItemActive()) ImGui::GetWindowDrawList()->AddRect(boxMin, boxMax, palette::Ember, 4 * s, 0, 2 * s);
    ImGui::EndDisabled();
    ReportMenuCard("chat-box", boxMin, boxMax);
    if (edited && !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) std::snprintf(chat_, sizeof(chat_), "%s", buffer);
    chatBoxText_ = chat_;
    NoteUserText(chat_, UserTextRole::Draft);

    ImGui::SetCursorPosX(x);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImU32 counterColor = left <= 0 ? ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Error)) :
        left <= 32 ? ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)) : palette::Muted;
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(at, ImVec2(at.x + width, at.y + noteHeight), true);
    draw->AddText(ImVec2(at.x + width - counterWidth, at.y), counterColor, counter.c_str());
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, ImGui::ColorConvertFloat4ToU32(ToneColor(noteTone)), note.c_str(), nullptr, wrap);
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(width, noteHeight));
}
} }
