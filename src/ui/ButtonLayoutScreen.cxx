#include "ButtonLayoutScreen.hxx"
#include "GameMenu.hxx"
#include "TextPill.hxx"

namespace sf4e { namespace ui {
using namespace gamepads;
std::string LayoutName(const std::optional<ButtonLayout>& layout) {
    const auto* preset = layout ? PresetOf(*layout) : nullptr;
    return loc::T(!layout ? "button_layout.game_layout" : preset ? preset->key : "button_layout.custom");
}
std::string AttackLongName(std::uint32_t mask) {
    for (const auto& function : LayoutFunctions) if (mask == function.mask) return loc::T(function.key);
    return FightName(mask);
}
namespace {
// The wizard's name for a bound input. A hat mask of more than one direction has none.
std::optional<RawInput> BoundInput(const SdlInput& input) {
    if (input.type == SdlBindType::Button) return RawInput{RawKind::Button, input.index};
    if (input.type == SdlBindType::Hat) {
        for (unsigned bit = 0; bit < 4; ++bit) if (input.hatMask == 1 << bit) return RawInput{static_cast<RawKind>(bit + 1), input.index};
        return {};
    }
    const int half = AxisHalf(input.min, input.max);
    return RawInput{!half ? RawKind::Trigger : half > 0 ? RawKind::AxisPlus : RawKind::AxisMinus, input.index};
}
}
FightNames LayoutButtonNames(const PadView& pad, ButtonPrompts prompts) {
    const auto brand = ResolvePrompts(prompts, input::PadXInput, pad.brand).brand;
    if (brand == Brand::PlayStation) return {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3"};
    if (brand == Brand::Nintendo) return {"B", "A", "Y", "X", "L", "R", "ZL", "ZR", "LS", "RS"};
    FightNames names = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS"};
    if (brand != Brand::Stick) return names;
    constexpr std::array<const char*, 10> targets = {"a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick", "rightstick"};
    for (const auto& binding : pad.bindings)
        for (unsigned i = 0; i < targets.size(); ++i) if (SameTarget(binding.output, SdlTarget(targets[i])))
            if (auto raw = BoundInput(binding.input)) { raw->numberedHat = pad.numberedHats; names[i] = RawInputName(*raw); }
    return names;
}
std::string ExtraButtonName(const PadView& pad, unsigned extra) {
    const auto brand = pad.brand.brand;
    switch (static_cast<Extra>(extra)) {
    case Extra::Touchpad: return loc::T("button_layout.extra.touchpad");
    case Extra::Home:
        if (brand == Brand::PlayStation) return "PS";
        if (brand == Brand::Xbox) return "Xbox";
        return loc::T("button_layout.extra.home");
    case Extra::Share:
        // SDL's misc1: Share on Xbox Series pads, the microphone button on a DualSense, Capture on Nintendo pads.
        if (brand == Brand::Xbox) return "Share";
        if (brand == Brand::PlayStation) return loc::T("button_layout.extra.mute");
        if (brand == Brand::Nintendo) return "Capture";
        return loc::T("button_layout.extra.misc");
    case Extra::RightPaddle1: return loc::Tf("button_layout.extra.right_paddle", "1");
    case Extra::LeftPaddle1: return loc::Tf("button_layout.extra.left_paddle", "1");
    case Extra::RightPaddle2: return loc::Tf("button_layout.extra.right_paddle", "2");
    case Extra::LeftPaddle2: return loc::Tf("button_layout.extra.left_paddle", "2");
    default: return ExtraIds[extra];
    }
}
namespace {
void Mark(std::vector<MenuEntry>& rows, const LayoutRowMarks& marks) {
    for (auto& row : rows) {
        if (!marks.waiting.empty()) {
            if (row.id == marks.waiting) { row.value = loc::T("button_layout.waiting"); row.valueHighlighted = true; }
            else { row.enabled = false; row.quiet = true; }
        }
        if (row.id == marks.flash) row.valueHighlighted = true;
    }
}
// An extra button's row: Off or the layout button it copies, chosen from the
// buttons the pad has, as Steam Input binds a touchpad or paddle.
MenuEntry ExtraButtonRow(const PadView& pad, unsigned extra, bool enabled, const std::string& unavailable) {
    const auto name = ExtraButtonName(pad, extra);
    auto row = Row(ExtraRowId(extra), name, enabled ? loc::Tf("button_layout.extra_detail", name) : unavailable, enabled);
    const auto copy = CopiedButton(pad.extras, extra, pad.has);
    row.value = copy ? loc::Tf("button_layout.extra_copy", pad.buttonNames[*copy]) : std::string(loc::T("common.off"));
    row.chosen = copy ? LayoutGlyphs[*copy] : "off";
    row.choices.push_back({"off", loc::T("common.off"), ""});
    for (unsigned i = 0; i < pad.has.size(); ++i) if (pad.has[i]) {
        const auto fight = FightName(pad.table[i]);
        row.choices.push_back({LayoutGlyphs[i], loc::Tf("button_layout.extra_copy", pad.buttonNames[i]),
            loc::Tf("button_layout.button_does", fight.empty() ? std::string(loc::T("button_layout.nothing")) : fight)});
    }
    row.valueHighlighted = std::find(pad.held.begin(), pad.held.end(), ExtraIds[extra]) != pad.held.end();
    row.detailText = DetailText::Name; row.quiet = !row.enabled;
    return row;
}
}
std::vector<MenuEntry> ButtonLayoutRows(const PadView& pad, bool editable, const LayoutRowMarks& marks) {
    std::vector<MenuEntry> rows;
    const bool absent = pad.state == RowState::Absent;
    const bool setup = pad.state == RowState::GameRead || pad.state == RowState::Unmapped;
    if (setup) {
        auto first = InfoRow("layout-setup-first", loc::T("button_layout.setup_first"), "", loc::Tf("button_layout.setup_first_detail", pad.name));
        first.detailText = DetailText::Name;
        return {std::move(first), Row("setup", loc::T("controllers.choice_setup"), loc::T(editable ? "controllers.choice_setup_detail" : "settings.leave_room_to_edit"), editable)};
    }
    const auto unavailable = absent ? loc::Tf("button_layout.absent_detail", pad.name) : std::string(loc::T("settings.leave_room_to_edit"));
    auto first = InfoRow(absent ? "layout-absent" : "test", loc::T(absent ? "button_layout.absent" : "button_layout.test"), "",
        absent ? unavailable : loc::Tf("button_layout.test_detail", pad.name));
    first.detailText = DetailText::Name; rows.push_back(std::move(first));
    const auto missing = MissingAttacks(pad.table, pad.has);
    for (const auto& function : LayoutFunctions) {
        auto detail = editable && !absent ? loc::Tf("button_layout.row_detail", loc::T(function.key)) : unavailable;
        if (editable && std::find(missing.begin(), missing.end(), function.mask) != missing.end()) detail += "\n\n" + std::string(loc::T("button_layout.row_missing"));
        auto row = Row(std::string("fn-") + function.id, loc::T(function.key), detail, editable && !absent);
        for (unsigned i = 0; i < pad.has.size(); ++i) if (pad.has[i] && pad.table[i] == function.mask) {
            if (!row.value.empty()) row.value += ", ";
            row.value += pad.buttonNames[i];
        }
        if (row.value.empty()) row.value = loc::T("button_layout.no_button");
        row.detailText = DetailText::Name; row.quiet = !row.enabled; rows.push_back(std::move(row));
    }
    // Ember's menu buttons: the player's choice, else the game's own (input::ResolveMenuButtons).
    const struct { const char* id; const char* label; const char* detail; std::uint32_t mask; const char* glyph; } menuRows[] = {
        {"menu-confirm", loc::T("button_layout.menu_confirm"), loc::T("button_layout.menu_confirm_detail"), pad.menuButtons.confirm, pad.menuButtons.confirmGlyph},
        {"menu-back", loc::T("button_layout.menu_back"), loc::T("button_layout.menu_back_detail"), pad.menuButtons.back, pad.menuButtons.backGlyph}};
    for (const auto& menu : menuRows) {
        auto row = Row(menu.id, menu.label, editable && !absent ? std::string(menu.detail) : unavailable, editable && !absent);
        row.value = MenuButtonName(pad, menu.mask, menu.glyph);
        row.detailText = DetailText::Name; row.quiet = !row.enabled; rows.push_back(std::move(row));
    }
    // Training's own buttons: one press records or plays, as F7 and F8 do.
    const struct { const char* id; const char* label; const char* detail; int button; } trainingRows[] = {
        {"training-record", loc::T("button_layout.training_record"), loc::T("button_layout.training_record_detail"), pad.training.record},
        {"training-play", loc::T("button_layout.training_play"), loc::T("button_layout.training_play_detail"), pad.training.play}};
    for (const auto& training : trainingRows) {
        auto row = Row(training.id, training.label, editable && !absent ? std::string(training.detail) : unavailable, editable && !absent);
        row.value = TrainingButtonName(pad, training.button);
        row.detailText = DetailText::Name; row.quiet = !row.enabled; rows.push_back(std::move(row));
    }
    if (editable && !absent && (pad.training.record >= 0 || pad.training.play >= 0))
        rows.push_back(ConfirmRow("training-clear", loc::T("button_layout.training_clear"), loc::T("button_layout.training_clear_detail")));
    for (unsigned extra = 0; extra < ExtraCount; ++extra)
        if (pad.hasExtras[extra]) rows.push_back(ExtraButtonRow(pad, extra, editable && !absent, unavailable));
    rows.push_back(Row("edit-button", loc::T("button_layout.edit_button"), editable && !absent ? loc::T("button_layout.edit_button_detail") : unavailable, editable && !absent));
    auto preset = Row("preset", loc::T("button_layout.preset"), editable && !absent ? loc::T("button_layout.preset_detail") : unavailable, editable && !absent);
    preset.value = LayoutName(pad.layout);
    if (pad.layout) if (const auto* current = PresetOf(*pad.layout)) preset.chosen = current->id;
    for (const auto& choice : LayoutPresets) preset.choices.push_back({choice.id, loc::T(choice.key), loc::T(choice.detailKey)});
    rows.push_back(std::move(preset));
    if (editable && !absent && pad.layout) rows.push_back(ConfirmRow("remove", loc::T("button_layout.remove"), loc::T("button_layout.remove_detail")));
    Mark(rows, marks);
    return rows;
}
std::vector<MenuEntry> ButtonEditorRows(const PadView& pad, unsigned button, bool editable) {
    std::vector<MenuEntry> rows;
    for (unsigned i = 0; i < 6; ++i) {
        const auto& function = LayoutFunctions[i];
        rows.push_back(Value(std::string("bit-") + function.id, loc::T(function.key), loc::T(pad.table[button] & function.mask ? "common.on" : "common.off"),
            loc::Tf("button_layout.bit_detail", loc::T(function.key)), editable));
    }
    rows.push_back(Row("clear", loc::T("button_layout.clear"), loc::T("button_layout.clear_detail"), editable && pad.table[button] != 0));
    return rows;
}
namespace {
bool Held(const PadView& pad, const char* glyph) { return std::find(pad.held.begin(), pad.held.end(), glyph) != pad.held.end(); }
void PictureName(const PadView& pad) {
    const float size = 64 * Scale(); const auto at = ImGui::GetCursorScreenPos();
    const auto picture = MenuArt() ? MenuArt()->InputPrompt(pad.picture) : SelectionImage{};
    if (picture.texture) ImGui::GetWindowDrawList()->AddImage(picture.texture, at, ImVec2(at.x + size, at.y + size), picture.uvMin, picture.uvMax);
    ImGui::Dummy(ImVec2(size, size)); ImGui::SameLine();
    NoteUserText(pad.name.c_str()); ImGui::TextWrapped("%s", pad.name.c_str());
}
void LayoutMap(const PadView& pad, PromptBrand brand) {
    const float s = Scale(), size = 40 * s;
    const auto names = NamesOf(pad.table);
    const std::vector<std::vector<unsigned>> lines = {{2, 3, 5, 4}, {0, 1, 7, 6}, {8, 9}};
    auto* draw = ImGui::GetWindowDrawList();
    for (const auto& line : lines) {
        auto at = ImGui::GetCursorScreenPos(); unsigned count = 0; float labelHeight = 0;
        for (auto i : line) if (pad.has[i]) {
            const bool held = Held(pad, LayoutGlyphs[i]);
            DrawPromptGlyph(draw, LayoutGlyphs[i], at, size, MenuArt(), s, 0, brand, held ? 1.f : .35f, &names);
            if (held) draw->AddRectFilled(ImVec2(at.x, at.y + size + 2 * s), ImVec2(at.x + size, at.y + size + 4 * s), palette::Ember);
            const auto label = names[i].empty() ? std::string(loc::T("button_layout.nothing")) : names[i];
            const auto measured = ImGui::GetFont()->CalcTextSizeA(12 * s, FLT_MAX, size, label.c_str());
            labelHeight = (std::max)(labelHeight, measured.y);
            draw->AddText(ImGui::GetFont(), 12 * s, ImVec2(at.x, at.y + size + 6 * s), palette::Muted, label.c_str(), nullptr, size);
            at.x += size + 16 * s; ++count;
        }
        if (count) ImGui::Dummy(ImVec2(0, size + labelHeight + 12 * s));
    }
}
void Attacks(const PadView& pad) {
    ImGui::TextUnformatted(loc::T("button_layout.attacks"));
    std::uint32_t active = 0;
    for (unsigned i = 0; i < pad.has.size(); ++i) if (pad.has[i] && Held(pad, LayoutGlyphs[i])) active |= pad.table[i];
    for (unsigned i = 0; i < ExtraCount; ++i)
        if (const auto copy = CopiedButton(pad.extras, i, pad.has)) if (Held(pad, ExtraIds[i])) active |= pad.table[*copy];
    const float s = Scale(), height = ImGui::GetTextLineHeight(), gap = 6 * s;
    auto at = ImGui::GetCursorScreenPos(); auto* draw = ImGui::GetWindowDrawList();
    for (unsigned i = 0; i < AttackMasks.size(); ++i) {
        const bool held = (active & AttackMasks[i]) != 0;
        const float width = ImGui::CalcTextSize(AttackNames[i]).x + 12 * s;
        DrawButtonPill(draw, AttackNames[i], at, width, height, s, held ? 1.f : .35f, held);
        at.x += width + gap;
    }
    ImGui::Dummy(ImVec2(0, height));
}
}
void DrawButtonLayoutDetail(const PadView& pad, ButtonPrompts prompts, bool capture) {
    PictureName(pad);
    if (pad.state == RowState::GameRead || pad.state == RowState::Unmapped) return;
    LayoutMap(pad, ResolvePrompts(prompts, input::PadXInput, pad.brand));
    if (!capture) Attacks(pad);
}
CaptureCard LayoutCaptureCard(const PadView& pad, const std::optional<std::uint32_t>& mask, double left) {
    return {mask ? loc::Tf("button_layout.press_for", AttackLongName(*mask)) : std::string(loc::T("button_layout.press_to_change")), pad.name, left};
}
CaptureCard MenuCaptureCard(const PadView& pad, bool back, double left) {
    return {loc::Tf("button_layout.press_for", loc::T(back ? "button_layout.menu_back" : "button_layout.menu_confirm")), pad.name, left};
}
CaptureCard TrainingCaptureCard(const PadView& pad, bool play, double left) {
    return {loc::Tf("button_layout.press_for", loc::T(play ? "button_layout.training_play" : "button_layout.training_record")), pad.name, left};
}
std::string TrainingButtonName(const PadView& pad, int button) {
    if (button < 0 || button >= static_cast<int>(pad.buttonNames.size())) return loc::T("common.off");
    const auto name = pad.buttonNames[button];
    return pad.table[button] ? loc::Tf("button_layout.training_busy_value", name) : name;
}
std::string MenuButtonName(const PadView& pad, std::uint32_t mask, const char* glyph) {
    for (unsigned i = 0; i < LayoutBits.size(); ++i) if (mask == std::uint32_t(1) << LayoutBits[i] && !pad.buttonNames[i].empty()) return pad.buttonNames[i];
    return glyph;
}
namespace {
// The prompt at 1.4x the body font, as the wizard's capture draws it (5.2).
float PromptFont() { return 1.4f * ImGui::GetFontSize(); }
float BarHeight() { return 4 * Scale(); }
}
float CaptureCardHeight(const CaptureCard& card, float width) {
    const float gap = 10 * Scale();
    return ImGui::GetFont()->CalcTextSizeA(PromptFont(), FLT_MAX, width, card.prompt.c_str()).y + gap +
        ImGui::CalcTextSize(card.pad.c_str(), nullptr, false, width).y + gap + BarHeight() + gap +
        ImGui::CalcTextSize(loc::T("button_layout.press_cancel"), nullptr, false, width).y;
}
void DrawCaptureCard(const CaptureCard& card) {
    const float s = Scale(), gap = 10 * s, width = ImGui::GetContentRegionAvail().x;
    auto* draw = ImGui::GetWindowDrawList();
    auto at = ImGui::GetCursorScreenPos();
    const auto prompt = ImGui::GetFont()->CalcTextSizeA(PromptFont(), FLT_MAX, width, card.prompt.c_str());
    draw->AddText(ImGui::GetFont(), PromptFont(), at, palette::Ivory, card.prompt.c_str(), nullptr, width);
    ImGui::Dummy(ImVec2(0, prompt.y + gap - ImGui::GetStyle().ItemSpacing.y));
    NoteUserText(card.pad);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette::Muted));
    ImGui::TextWrapped("%s", card.pad.c_str());
    ImGui::PopStyleColor();
    at = ImGui::GetCursorScreenPos(); at.y += gap - ImGui::GetStyle().ItemSpacing.y;
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + BarHeight()), IM_COL32(255, 135, 56, 60));
    draw->AddRectFilled(at, ImVec2(at.x + width * static_cast<float>(card.left), at.y + BarHeight()), palette::Ember);
    ImGui::Dummy(ImVec2(0, 2 * gap + BarHeight() - 2 * ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextWrapped("%s", loc::T("button_layout.press_cancel"));
}
void DrawButtonEditorDetail(const PadView& pad, ButtonPrompts prompts, unsigned button) {
    const float size = 64 * Scale(); const auto at = ImGui::GetCursorScreenPos(); const auto names = NamesOf(pad.table);
    DrawPromptGlyph(ImGui::GetWindowDrawList(), LayoutGlyphs[button], at, size, MenuArt(), Scale(), 0,
        ResolvePrompts(prompts, input::PadXInput, pad.brand), 1, &names);
    ImGui::Dummy(ImVec2(size, size));
    ImGui::TextUnformatted(pad.buttonNames[button].c_str());
    const auto name = FightName(pad.table[button]);
    ImGui::TextWrapped("%s", loc::Tf("button_layout.button_does", name.empty() ? loc::T("button_layout.nothing") : name).c_str());
    Attacks(pad);
}
} }
