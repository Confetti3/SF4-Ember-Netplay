#include "ChatNotification.hxx"
#include "../common/Localization.hxx"
#include "TrainingPanel.hxx"
#include <imgui_internal.h>
#include <algorithm>
#include <array>
namespace sf4e { namespace ui {
namespace {
std::string BadgeText(unsigned count) { return count > 99 ? "99+" : std::to_string(count); }
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

ChatNotificationView ChatNotificationState::Update(const ChatTranscript& transcript, const std::set<room::MemberId>& muted, double now) {
    const unsigned unread = transcript.Unread(muted);
    if (epoch_ != transcript.Epoch()) {
        epoch_ = transcript.Epoch(); sequence_ = transcript.LastSequence();
        until_ = 0; lastArrival_ = -Seconds;
    } else {
        const bool arrived=transcript.NewestUnreadSequence(muted)>sequence_;
        sequence_ = transcript.LastSequence();
        if (arrived) {
            // One four-second notice per burst. More messages update the
            // count, but cannot extend the notice indefinitely. A quiet
            // interval arms the next burst; unmuting old chat never does.
            if (now - lastArrival_ >= Seconds) until_ = now + Seconds;
            lastArrival_ = now;
        }
    }
    if (!unread) { until_ = 0; lastArrival_ = -Seconds; }
    return {unread, now < until_};
}

// A passive room-chat indicator, outside the fight's connection/status line.
// Its window shrinks to a speech bubble and the shared count badge at expiry.
void DrawChatNotification(const ChatNotificationView& view,const MatchStripView& match) {
    if (!view.unread) return;
    const auto* vp = ImGui::GetMainViewport();
    const float s = Scale();
    const auto text=loc::Tf("training.room.unread",view.unread);
    const float wrap=(std::max)(1.f,vp->Size.x-72*s);
    const auto measured=view.expanded?ImGui::CalcTextSize(text.c_str(),nullptr,false,wrap):ImVec2(UnreadBadgeWidth(view.unread),18*s);
    const auto minimum=ImGui::GetStyle().WindowMinSize;
    const ImVec2 size((std::max)(minimum.x,measured.x+40*s),(std::max)(minimum.y,(std::max)(18*s,measured.y)+10*s));
    ImVec2 pos(vp->Pos.x+vp->Size.x-16*s-size.x,vp->Pos.y+vp->Size.y-24*s-size.y);
    // Use actual occupied telemetry/name bounds, including raised and split
    // layouts. Moving above an intersecting box also handles long locales.
    const auto bounds=MatchStripGeometry(match);
    std::array<MatchStripBox,5> occupied{{bounds.panel,bounds.state,bounds.names[0],bounds.names[1],{}}};
    // The meter was drawn earlier this frame. Its live rectangle includes
    // whichever number and bar rows are enabled; no guessed height here.
    if (const auto* meter=ImGui::FindWindowByName(FrameMeterWindow);meter&&meter->LastFrameActive==ImGui::GetFrameCount())
        occupied.back()={meter->Pos.x,meter->Pos.y,meter->Pos.x+meter->Size.x,meter->Pos.y+meter->Size.y,true};
    // Move only upward, processing the lowest obstacle first. Each obstacle
    // is visited once; a later move cannot re-enter an already cleared box.
    std::sort(occupied.begin(),occupied.end(),[](const MatchStripBox& a,const MatchStripBox& b){return a.y0>b.y0;});
    for(const auto& box:occupied)
        if(box.valid&&pos.x<box.x1&&pos.x+size.x>box.x0&&pos.y<box.y1&&pos.y+size.y>box.y0)
            pos.y=box.y0-8*s-size.y;
    ImGui::SetNextWindowPos(pos,ImGuiCond_Always);
    ImGui::SetNextWindowSize(size,ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8*s, 5*s));
    ImGui::Begin("Chat notification", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    const auto at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRect(ImVec2(at.x, at.y + 2*s), ImVec2(at.x + 16*s, at.y + 13*s), palette::Ember, 3*s);
    draw->AddLine(ImVec2(at.x + 4*s, at.y + 13*s), ImVec2(at.x + 4*s, at.y + 17*s), palette::Ember);
    draw->AddLine(ImVec2(at.x + 4*s, at.y + 17*s), ImVec2(at.x + 8*s, at.y + 13*s), palette::Ember);
    ImGui::Dummy(ImVec2(18*s, 18*s)); ImGui::SameLine(0, 6*s);
    if(view.expanded){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+wrap);ImGui::TextUnformatted(text.c_str());ImGui::PopTextWrapPos();}
    else {
        const auto count = ImGui::GetCursorScreenPos();
        const float width = UnreadBadgeWidth(view.unread);
        DrawUnreadBadge(count.x + width, count.y, view.unread);
        ImGui::Dummy(ImVec2(width, 18*s));
    }
    ImGui::End(); ImGui::PopStyleVar();
}

} }
