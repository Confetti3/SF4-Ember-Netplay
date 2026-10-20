#include "ControllersScreen.hxx"
#include "ButtonPromptSetting.hxx"
#include "ButtonLayoutScreen.hxx"
#include "GameMenu.hxx"
#include "../netplay/InputAssignment.hxx"

namespace sf4e { namespace ui {
using namespace gamepads;
std::string RawInputName(const RawInput& input) {
    const char* key = "pad_setup.input.button";
    switch (input.kind) {
    case RawKind::HatUp: key = "pad_setup.input.hat_up"; break;
    case RawKind::HatRight: key = "pad_setup.input.hat_right"; break;
    case RawKind::HatDown: key = "pad_setup.input.hat_down"; break;
    case RawKind::HatLeft: key = "pad_setup.input.hat_left"; break;
    case RawKind::AxisPlus: key = "pad_setup.input.axis_plus"; break;
    case RawKind::AxisMinus: key = "pad_setup.input.axis_minus"; break;
    case RawKind::Trigger: key = "pad_setup.input.trigger"; break;
    default: break;
    }
    const bool hat = input.kind >= RawKind::HatUp && input.kind <= RawKind::HatLeft;
    return hat ? std::string(loc::T(key)) + (input.numberedHat ? " " + std::to_string(input.index + 1) : "") :
        loc::Tf(key, input.index + 1);
}
std::string RawInputList(const std::vector<RawInput>& inputs) {
    std::string text;
    for (const auto& input : inputs) {
        if (!text.empty()) text += ", ";
        text += RawInputName(input);
    }
    return text;
}
MenuEntry ControllerRow(const PadView& pad, bool canChange, bool canEdit, Layer layer) {
    auto row = Row("pad-" + std::to_string(pad.id), pad.name, "");
    row.userText = true;
    row.detailText = DetailText::Name;
    switch (pad.state) {
    case RowState::Recognised:
        if (pad.gameplay) row.value = loc::T("controllers.value_yours");
        row.detail = loc::Tf(pad.gameplay ? "controllers.detail_yours" : "controllers.detail_other", pad.slot + 1);
        if (pad.customMapping) row.detail += "\n" + std::string(loc::T("controllers.detail_custom"));
        break;
    case RowState::Steam:
        row.value = loc::T("controllers.value_steam"); row.detail = loc::T("controllers.detail_steam"); break;
    case RowState::Unmapped:
        row.value = loc::T("controllers.value_unmapped");
        row.detail = loc::T(canEdit ? "controllers.detail_unmapped" : "settings.leave_room_to_edit");
        row.enabled = canEdit; return row;
    case RowState::NotInUse:
        row.value = loc::T("controllers.value_full"); row.detail = loc::T("controllers.detail_full"); row.info = true; return row;
    case RowState::Absent:
        row.value = loc::T("controllers.value_absent"); row.detail = loc::T("controllers.detail_absent"); row.info = true; return row;
    case RowState::GameRead:
        if (pad.gameplay) row.value = loc::T("controllers.value_yours");
        row.detail = loc::T("controllers.detail_game_read");
        if (layer == Layer::Active) row.choices = {{"layout", loc::T("controllers.choice_layout"), loc::T("controllers.choice_layout_setup"), false}, {"setup", loc::T("controllers.choice_setup"),
            loc::T(canEdit ? "controllers.choice_setup_detail" : "settings.leave_room_to_edit"), canEdit}};
        else row.info = true;
        return row;
    }
    if (pad.layout) row.detail += "\n" + loc::Tf("controllers.detail_layout", LayoutName(pad.layout));
    row.choices = {
        {"play", loc::T("controllers.choice_play"), loc::T(pad.gameplay ? "controllers.choice_play_current" :
            canChange ? "controllers.choice_play_detail" : "settings.leave_room_to_edit"), !pad.gameplay && canChange},
        {"layout", loc::T("controllers.choice_layout"), loc::T("controllers.choice_layout_detail")},
        {"setup", loc::T("controllers.choice_setup"), loc::T(!canEdit ? "settings.leave_room_to_edit" :
            pad.state == RowState::Steam ? "controllers.choice_setup_steam" : "controllers.choice_setup_detail"), canEdit && pad.state != RowState::Steam}
    };
    if (pad.customMapping) row.choices.push_back({"forget", loc::T("controllers.choice_forget"),
        loc::T(canEdit ? "controllers.choice_forget_detail" : "settings.leave_room_to_edit"), canEdit});
    return row;
}
std::vector<MenuEntry> ControllerRows(Layer layer, const std::vector<PadView>& pads, bool canChange, bool canEdit) {
    std::vector<MenuEntry> rows;
    if (layer == Layer::Failed) rows.push_back(InfoRow("pads-unavailable", loc::T("controllers.unavailable"), "", loc::T("controllers.unavailable_detail")));
    if (layer == Layer::GameReads) rows.push_back(InfoRow("pads-game", loc::T("controllers.game_reads"), "", loc::T("controllers.game_reads_detail")));
    for (const auto& pad : pads) {
        NoteUserText(pad.name.c_str());
        rows.push_back(ControllerRow(pad, canChange, canEdit, layer));
    }
    if (pads.empty()) rows.push_back(InfoRow("pads-none", loc::T("controllers.none"), "", loc::T("controllers.none_detail")));
    return rows;
}
std::optional<ControllerRequest> ControllerAction(const MenuAction& action, const std::vector<PadView>& pads) {
    const auto pad = std::find_if(pads.begin(), pads.end(), [&](const PadView& pad) { return action.id == "pad-" + std::to_string(pad.id); });
    if (pad == pads.end()) return {};
    using Kind = ControllerRequest::Kind;
    if (action.kind == MenuAction::Activate && pad->state == RowState::Unmapped) return ControllerRequest{Kind::Setup, pad->id};
    if (action.kind != MenuAction::Chosen) return {};
    if (action.text == "layout") return ControllerRequest{Kind::OpenLayout, pad->id};
    if (action.text == "play") return ControllerRequest{Kind::Play, pad->id};
    if (action.text == "setup") return ControllerRequest{Kind::Setup, pad->id};
    if (action.text == "forget") return ControllerRequest{Kind::Forget, pad->id};
    return {};
}
std::vector<MenuEntry> PlayerControllerRows(const netplay::PlayerPreferences& p, const std::string& controller,
    int deviceType, PromptBrand brand, const std::vector<PadView>& pads, bool pending, bool canChange, bool canEdit, bool idle, Layer layer) {
    NoteUserText(controller.c_str());
    const auto count = std::count_if(pads.begin(), pads.end(), [](const PadView& pad) { return pad.state != RowState::Absent; });
    auto controllers = Row("controllers", loc::T("player.controllers"), loc::Tf("player.controllers_detail", controller));
    controllers.value = loc::Tf("player.controllers_value", count);
    controllers.detailText = DetailText::Name;
    auto capture = Row("capture", loc::T("player.change_controller"), loc::Tf("player.change_controller_detail", controller), canChange);
    capture.detailText = DetailText::Name;
    auto promptDetail = std::string(loc::T(canEdit ? "player.button_prompts_detail" : "settings.leave_room_to_edit"));
    if (canEdit && p.buttonPrompts == ButtonPrompts::Automatic && deviceType == input::PadXInput)
        promptDetail += "\n" + loc::Tf("player.prompts_showing", PromptBrandWord(brand.brand));
    auto gameDetail = std::string(loc::T(canEdit ? "player.game_reads_detail" : "settings.leave_room_to_edit"));
    if (canEdit && pending) gameDetail += "\n\n" + std::string(loc::T("player.game_reads_pending"));
    const auto pad = std::find_if(pads.begin(), pads.end(), [](const PadView& pad) { return pad.gameplay; });
    const bool keyboard = deviceType == input::PadKeyboard;
    const bool covered = pad != pads.end() && (pad->state == RowState::Recognised || pad->state == RowState::Steam || pad->state == RowState::Absent);
    const char* detail = layer == Layer::GameReads ? "player.button_layout_game_reads" : layer == Layer::Failed ? "player.button_layout_failed" :
        keyboard ? "player.button_layout_keyboard" : covered ? "player.button_layout_detail" : "player.button_layout_setup";
    auto layout = Row("controls", loc::T("player.button_layout"), loc::Tf(detail, controller), layer == Layer::Active && !keyboard);
    layout.quiet = !layout.enabled; layout.detailText = DetailText::Name;
    if (covered && layer == Layer::Active) layout.value = LayoutName(pad->layout);
    return {TextRow("name", loc::T("profile.player_name"), p.displayName, 31, canEdit), std::move(controllers), std::move(capture),
        ConfirmRow("keyboard", loc::T("player.use_keyboard"), loc::T("player.use_keyboard_detail"), canChange),
        Value("button-prompts", loc::T("player.button_prompts"), ButtonPromptValue(p.buttonPrompts), promptDetail, canEdit),
        std::move(layout), Row("game-buttons", loc::T("player.game_buttons"), loc::T("player.game_buttons_detail"), idle),
        Value("background-play", loc::T("player.background_play"), loc::T(p.backgroundPlay ? "common.on" : "common.off"), loc::T("player.background_play_detail"), canEdit),
        Value("lower-input-lag", loc::T("player.lower_input_lag"), loc::T(p.lowerInputLag ? "common.on" : "common.off"), loc::T("player.lower_input_lag_detail"), canEdit),
        Value("lower-keyboard-lag", loc::T("player.lower_keyboard_lag"), loc::T(p.lowerKeyboardLag ? "common.on" : "common.off"), loc::T("player.lower_keyboard_lag_detail"), canEdit),
        Value("faster-display", loc::T("player.faster_display"), loc::T(p.fasterDisplay ? "common.on" : "common.off"), loc::T("player.faster_display_detail"), canEdit),
        Value("borderless", loc::T("player.borderless"), loc::T(p.borderless ? "common.on" : "common.off"), loc::T("player.borderless_detail"), canEdit),
        Value("game-reads-pads", loc::T("player.game_reads"), loc::T(p.controllerLayer ? "common.off" : "common.on"), gameDetail, canEdit)};
}
namespace {
void Picture(const PadView& pad, float pixels, float alpha = 1) {
    const float size = pixels * Scale();
    const auto picture = MenuArt() ? MenuArt()->InputPrompt(pad.picture) : SelectionImage{};
    if (picture.texture) {
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(picture.texture, at, ImVec2(at.x + size, at.y + size),
            picture.uvMin, picture.uvMax, ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)));
    }
    ImGui::Dummy(ImVec2(size, size));
}
void TestLine(const PadView& pad, PromptBrand brand, const std::vector<const char*>& ids) {
    auto* draw = ImGui::GetWindowDrawList();
    const float s = Scale(), size = 32 * s;
    auto at = ImGui::GetCursorScreenPos();
    const auto names = NamesOf(pad.table);
    for (const char* id : ids) {
        bool has = true;
        for (unsigned i = 0; i < LayoutGlyphs.size(); ++i) if (!std::strcmp(id, LayoutGlyphs[i])) has = pad.has[i];
        if (!has) continue;
        const bool held = std::find(pad.held.begin(), pad.held.end(), id) != pad.held.end();
        DrawPromptGlyph(draw, id, at, size, MenuArt(), s, 0, brand, held ? 1.f : .35f, &names);
        if (held) draw->AddRectFilled(ImVec2(at.x, at.y + size + 2 * s), ImVec2(at.x + size, at.y + size + 4 * s), palette::Ember);
        at.x += size + 8 * s;
    }
    ImGui::Dummy(ImVec2(ids.size() * (size + 8 * s), size + 8 * s));
}
}
void DrawControllerDetail(const PadView& pad, ButtonPrompts prompts) {
    // The game holds an unmapped pad, so Ember sees only whether it reads a press
    // (no input names): the picture lights while it does.
    Picture(pad, 64, pad.state == RowState::Unmapped && !pad.pressed ? .4f : 1.f);
    if (pad.state == RowState::Recognised || pad.state == RowState::Steam) {
        ImGui::TextUnformatted(loc::T("controllers.test_heading"));
        const auto brand = ResolvePrompts(prompts, input::PadXInput, pad.brand);
        TestLine(pad, brand, {"up", "down", "left", "right"});
        TestLine(pad, brand, {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "View", "Start"});
    } else if (pad.state == RowState::Unmapped) {
        ImGui::TextUnformatted(loc::T("controllers.test_heading"));
        // An empty line while pressed keeps the pane from moving.
        const auto text = !pad.raw.empty() ? loc::Tf("controllers.test_raw", RawInputList(pad.raw)) :
            pad.pressed ? std::string() : std::string(loc::T("controllers.test_nothing"));
        ImGui::TextWrapped("%s", text.c_str());
    }
}
void DrawCaptureDetail(input::Capture capture, const std::vector<PadView>& pads, unsigned candidate) {
    for (const auto& pad : pads) {
        if (pad.state == RowState::Absent) continue;
        if (capture == input::Capture::ReleaseSelected) {
            if (pad.id != candidate) continue;
            Picture(pad, 64);
            NoteUserText(pad.name.c_str());
            ImGui::TextWrapped("%s", loc::Tf("controller.release_to_use", pad.name).c_str());
        } else {
            Picture(pad, 32, pad.pressed ? 1.f : .4f);
            ImGui::SameLine();
            NoteUserText(pad.name.c_str());
            ImGui::TextUnformatted(pad.name.c_str());
        }
    }
}
} }
