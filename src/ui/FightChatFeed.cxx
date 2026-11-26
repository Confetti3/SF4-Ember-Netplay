#include "FightChatFeed.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>
#include <algorithm>

namespace sf4e { namespace ui {
FightChatAudience AudienceOf(const room::Snapshot& room, netplay::FightChatPreferences::Scope scope) {
    FightChatAudience audience;
    if (scope != netplay::FightChatPreferences::Scope::MyTable) return audience;
    const auto has = [](const std::vector<room::MemberId>& list, room::MemberId member) {
        return std::find(list.begin(), list.end(), member) != list.end();
    };
    const room::MemberId own = room.localMember;
    for (const auto& table : room.tables) {
        if (table.p1 != own && table.p2 != own && !has(table.queue, own) && !has(table.spectators, own) && !has(table.watchingNext, own)) continue;
        audience.everyone = false;
        for (const room::MemberId member : {table.p1, table.p2}) if (member) audience.members.insert(member);
        for (const auto* list : {&table.queue, &table.spectators, &table.watchingNext}) audience.members.insert(list->begin(), list->end());
        return audience;
    }
    return audience;
}

void FightChatFeed::Update(const ChatTranscript& transcript, bool fighting, double now) {
    if (fighting && !fighting_) { seen_ = transcript.LastSequence(); arrived_.clear(); }
    fighting_ = fighting;
    if (!fighting) return;
    for (auto line = transcript.Lines().rbegin(); line != transcript.Lines().rend(); ++line) {
        if (line->kind != ChatLine::Kind::Message) continue;
        if (line->sequence <= seen_) break;
        arrived_[line->sequence] = now;
    }
    seen_ = transcript.LastSequence();
    // Only the last few seconds' messages can still show.
    for (auto it = arrived_.begin(); it != arrived_.end();) it = now - it->second > ShownSeconds + FadeSeconds ? arrived_.erase(it) : std::next(it);
}

FightChatFeedView FightChatFeed::View(const ChatTranscript& transcript, const std::set<room::MemberId>& muted, const FightChatAudience& audience,
    bool filtered, double now) const {
    FightChatFeedView view;
    if (!fighting_) return view;
    for (auto line = transcript.Lines().rbegin(); line != transcript.Lines().rend() && view.entries.size() <= MaximumShown; ++line) {
        if (line->kind != ChatLine::Kind::Message) continue;
        // Stamps run with the sequence: a line with none is expired or from before the fight, and so is every older one.
        const auto arrived = arrived_.find(line->sequence);
        if (arrived == arrived_.end()) break;
        if (muted.count(line->sender) || !audience.Hears(line->sender)) continue;
        const double age = now - arrived->second;
        const float alpha = age <= ShownSeconds ? 1.f : static_cast<float>(1 - (age - ShownSeconds) / FadeSeconds);
        if (alpha <= 0) continue;
        const std::string text = ChatLineText(*line, filtered);
        // Newest first here: a line the same member just said again is folded into the newer one.
        if (!view.entries.empty() && view.entries.back().sender == line->sender && view.entries.back().text == text) { ++view.entries.back().repeats; continue; }
        FightChatEntry entry;
        entry.name = line->name; entry.text = text; entry.own = line->own; entry.alpha = alpha; entry.sender = line->sender;
        view.entries.push_back(std::move(entry));
    }
    if (view.entries.size() > MaximumShown) view.entries.resize(MaximumShown);
    std::reverse(view.entries.begin(), view.entries.end());
    return view;
}

namespace {
constexpr const char* ScopeRow = "fight-chat", *QuickRow = "quick-lines", *SlotRowPrefix = "quick-slot-";
constexpr const char* SlotKeys[quickchat::Slots] = {"settings.quick_slot_up", "settings.quick_slot_right", "settings.quick_slot_down", "settings.quick_slot_left"};
// The slot a row's id names, or -1.
int SlotOf(const std::string& id) {
    const std::string prefix = SlotRowPrefix;
    if (id.compare(0, prefix.size(), prefix) != 0 || id.size() != prefix.size() + 1) return -1;
    const int slot = id.back() - '0';
    return slot >= 0 && slot < quickchat::Slots ? slot : -1;
}
}

std::vector<MenuEntry> FightChatRows(const netplay::FightChatPreferences& chat, bool enabled) {
    std::vector<MenuEntry> rows{Value(ScopeRow, loc::T("settings.fight_chat"), loc::T(netplay::ScopeRowOf(chat.scope).key), loc::T("settings.fight_chat_detail"), enabled),
        Value(QuickRow, loc::T("settings.quick_lines"), loc::T(chat.quickLines ? "common.on" : "common.off"), loc::T("settings.quick_lines_detail"), enabled)};
    for (int slot = 0; slot < quickchat::Slots; ++slot)
        rows.push_back(Value(SlotRowPrefix + std::to_string(slot), loc::T(SlotKeys[slot]), loc::T(quickchat::RowOf(chat.slots[slot]).key),
            loc::T("settings.quick_slot_detail"), enabled && chat.quickLines));
    return rows;
}

bool AdjustFightChat(const std::string& id, int delta, netplay::FightChatPreferences& chat) {
    if (id == ScopeRow) chat.scope = netplay::SteppedScope(chat.scope, delta);
    else if (id == QuickRow) chat.quickLines = delta > 0;
    else if (const int slot = SlotOf(id); slot >= 0) {
        const int at = static_cast<int>(chat.slots[slot]) - 1;
        chat.slots[slot] = quickchat::Table[(at + (delta > 0 ? 1 : quickchat::Count - 1)) % quickchat::Count].line;
    } else return false;
    return true;
}

void DrawFightChatFeed(const FightChatFeedView& view, const MatchStripBox& strip) {
    if (view.entries.empty()) return;
    const auto* vp = ImGui::GetMainViewport();
    const float s = Scale(), width = vp->Size.x * .3f;
    // Under the life bar and portrait, clear of the timer. The strip's panel, where the strip placed it
    // (it may have moved to the bottom on a short screen), keeps its side: the feed goes to the other.
    const float left = vp->Pos.x + vp->Size.x * .03f, top = vp->Pos.y + vp->Size.y * .2f;
    const float reach = top + FightChatFeed::MaximumShown * (2 * ImGui::GetTextLineHeight() + 10 * s);
    const bool stripInTheWay = strip.valid && strip.x0 < left + width && strip.x1 > left && strip.y0 < reach && strip.y1 > top;
    const float x = stripInTheWay ? vp->Pos.x + vp->Size.x * .97f - width : left;
    ImGui::SetNextWindowPos(ImVec2(x, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(0);
    // Only each line's own dark pill shows: no frame, no padding around them.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Fight chat", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    auto* draw = ImGui::GetWindowDrawList();
    for (const auto& entry : view.entries) {
        const std::string said = entry.repeats > 1 ? entry.text + " x" + std::to_string(entry.repeats) : entry.text;
        const std::string line = entry.name + ": " + said;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::CalcTextSize(line.c_str(), nullptr, false, width - 12 * s);
        draw->AddRectFilled(at, ImVec2(at.x + size.x + 12 * s, at.y + size.y + 6 * s), IM_COL32(12, 11, 10, static_cast<int>(170 * entry.alpha)), 3 * s);
        ImGui::SetCursorScreenPos(ImVec2(at.x + 6 * s, at.y + 3 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, entry.alpha);
        ImGui::PushTextWrapPos(at.x + width - 6 * s);
        ImGui::PushStyleColor(ImGuiCol_Text, entry.own ? ImVec4(.71f, .66f, .61f, 1) : ImVec4(.93f, .9f, .86f, 1));
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor(); ImGui::PopTextWrapPos(); ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + size.y + 10 * s));
    }
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::End();
    ImGui::PopStyleVar(2);
}
} }
