#include "DisplayPanel.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <set>

namespace sf4e { namespace ui {
namespace {
std::string Size(int width, int height) { return std::to_string(width) + " x " + std::to_string(height); }
const char* ModeName(display::Mode mode) {
    switch (mode) {
    case display::Mode::Windowed: return loc::T("display.windowed");
    case display::Mode::Borderless: return loc::T("display.borderless");
    case display::Mode::Fullscreen: return loc::T("display.fullscreen");
    default: return loc::T("display.native");
    }
}
MenuEntry Choice(std::string id, const char* label, std::string value, const char* detail, bool enabled = true) {
    auto row = Row(std::move(id), label, detail, enabled);
    row.value = std::move(value); return row;
}
}
DisplayPanel::DisplayPanel(std::vector<display::Monitor> monitors, display::Preferences saved, Saver saver)
    : monitors_(std::move(monitors)), saved_(std::move(saved)), draft_(saved_), saver_(std::move(saver)) {}

std::vector<MenuEntry> DisplayPanel::Rows() const {
    using display::Mode;
    const bool custom = draft_.mode != Mode::Native;
    const auto* monitor = display::FindMonitor(monitors_, draft_.monitor);
    auto mode = Choice("display-mode", loc::T("display.mode"), ModeName(draft_.mode), loc::T("display.mode_detail"));
    for (int i = 0; i < 4; ++i) mode.choices.push_back({std::to_string(i), ModeName(static_cast<Mode>(i)), ""});
    mode.chosen = std::to_string(static_cast<int>(draft_.mode));
    auto screen = Choice("display-monitor", loc::T("display.monitor"), draft_.monitor.empty() ? loc::T("display.primary") :
        monitor && monitor->id == draft_.monitor ? monitor->name + " (" + Size(monitor->width, monitor->height) + ")" : loc::T("display.disconnected"),
        loc::T("display.monitor_detail"), custom && monitor);
    screen.choices.push_back({"", loc::T("display.primary"), ""});
    for (const auto& m : monitors_) screen.choices.push_back({m.id, m.name + " - " + Size(m.width, m.height) + " (" + m.id + ")", ""});
    screen.chosen = draft_.monitor;
    auto resolution = Choice("display-resolution", loc::T("display.resolution"), draft_.width ? Size(draft_.width, draft_.height) : loc::T("display.auto"),
        loc::T("display.resolution_detail"), custom && monitor);
    resolution.choices.push_back({"0x0", loc::T("display.auto"), ""});
    std::set<std::pair<int, int>> resolutions;
    if (monitor) {
        for (const auto& value : monitor->modes) resolutions.insert({value.width, value.height});
        if (draft_.mode != Mode::Fullscreen) {
            for (const auto& value : {std::pair<int,int>{1280,720}, {1600,900}, {1920,1080}, {2560,1440}, {3840,2160}})
                resolutions.insert(value);
            const auto fit = display::Fit16By9(monitor->width, monitor->height);
            if (fit.width >= 640 && fit.height >= 360) resolutions.insert({fit.width, fit.height});
        }
    }
    for (const auto& value : resolutions) resolution.choices.push_back({std::to_string(value.first) + "x" + std::to_string(value.second), Size(value.first, value.second), ""});
    resolution.chosen = std::to_string(draft_.width) + "x" + std::to_string(draft_.height);
    auto refresh = Choice("display-refresh", loc::T("display.refresh"), draft_.refresh ? std::to_string(draft_.refresh) + " Hz" : loc::T("display.auto"),
        loc::T("display.refresh_detail"), monitor && draft_.mode == Mode::Fullscreen);
    refresh.choices.push_back({"0", loc::T("display.auto"), ""});
    display::Selection selected;
    if (monitor) selected = display::Resolve(draft_, *monitor);
    std::set<int> rates;
    if (monitor) for (const auto& value : monitor->modes)
        if (value.width == selected.width && value.height == selected.height) rates.insert(value.refresh);
    for (int rate : rates) refresh.choices.push_back({std::to_string(rate), std::to_string(rate) + " Hz", ""});
    refresh.chosen = std::to_string(draft_.refresh);
    std::vector<MenuEntry> rows = {std::move(mode), std::move(screen), std::move(resolution), std::move(refresh)};
    if (custom && monitor) {
        const std::string result = Size(selected.width, selected.height) + " / " + Size(monitor->width, monitor->height);
        rows.push_back(InfoRow("display-output", loc::T("display.output"), result,
            selected.mode != draft_.mode ? loc::T("display.unsupported") : draft_.mode == Mode::Fullscreen
                ? loc::T("display.fullscreen_detail") : loc::T("display.proportions")));
    }
    rows.push_back(Row("display-save", loc::T("display.save"), loc::T("display.save_detail"), draft_.Valid() &&
        (!custom || (monitor && selected.mode == draft_.mode))));
    rows.push_back(ConfirmRow("display-reset", loc::T("display.reset"), loc::T("display.reset_detail")));
    rows.push_back(Row("display-refresh-monitors", loc::T("display.refresh_monitors"), loc::T("display.refresh_monitors_detail")));
    rows.push_back(Row("display-back", loc::T(draft_ != saved_ ? "display.back" : "common.back"), loc::T("display.back_detail")));
    return rows;
}
void DisplayPanel::Handle(const MenuAction& action) {
    // Revalidate against the current list, including enabled choices: an old
    // monitor popup cannot submit a now-missing mode after Refresh displays.
    const auto rows = Rows();
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& value) { return value.id == action.id; });
    if (row == rows.end() || !row->enabled) return;
    if (action.kind == MenuAction::Chosen) {
        const auto choice = std::find_if(row->choices.begin(), row->choices.end(), [&](const MenuChoice& value) { return value.id == action.text && value.enabled; });
        if (choice == row->choices.end()) return;
        if (action.id == "display-mode") { draft_.mode = static_cast<display::Mode>(std::stoi(action.text)); draft_.refresh = 0; }
        else if (action.id == "display-monitor") { draft_.monitor = action.text; draft_.width = draft_.height = draft_.refresh = 0; }
        else if (action.id == "display-resolution") {
            const auto split = action.text.find('x');
            draft_.width = std::stoi(action.text.substr(0, split)); draft_.height = std::stoi(action.text.substr(split + 1)); draft_.refresh = 0;
        } else if (action.id == "display-refresh") draft_.refresh = std::stoi(action.text);
        error_.clear(); savedNotice_ = false;
    } else if (action.kind == MenuAction::Activate) {
        if (action.id == "display-save" || action.id == "display-reset") {
            const auto value = action.id == "display-reset" ? display::Preferences{} : draft_;
            std::string diagnostic;
            if (!saver_(value, diagnostic)) { error_ = loc::T("display.save_failed"); savedNotice_ = false; }
            else { saved_ = draft_ = value; error_.clear(); savedNotice_ = true; }
        } else if (action.id == "display-refresh-monitors") refresh_ = true;
        else if (action.id == "display-back") Discard();
    }
}
void DisplayPanel::Discard() { draft_ = saved_; error_.clear(); savedNotice_ = false; }
void DisplayPanel::Draw(GameMenu& menu) {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos); ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("Display settings###EmberRecovery", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
    const auto status = !error_.empty() ? error_ : savedNotice_ ? std::string(loc::T("display.saved")) :
        draft_ != saved_ ? std::string(loc::T("display.unsaved")) : std::string(loc::T("display.restart"));
    menu.fitStatus = true;
    menu.compactDetailLines = 2;
    menu.backHint.clear();
    const auto action = menu.Draw(loc::T("display.title"), Rows(), status.c_str(), {}, 1, {}, {}, 0, 100, true,
        !error_.empty() ? Tone::Error : savedNotice_ ? Tone::Success : Tone::Neutral);
    Handle(action);
    if (action.kind == MenuAction::Returned || action.kind == MenuAction::Close || (action.kind == MenuAction::Activate && action.id == "display-back")) {
        Discard();
        if (menu.navigation.Screen() == "display") menu.navigation.Return();
    }
    ImGui::End();
}
} }
