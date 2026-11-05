#include "AppearanceGallery.hxx"
#include "AppearanceGrid.hxx"
#include "FighterSelector.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "RoomCards.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
namespace {
constexpr const char* CostumesScreen = "costumes";
constexpr const char* ColorsScreen = "colors";
// A card's picture is the fighter's whole figure, taller than wide.
constexpr float FigureAspect = 1.3f;
}
bool AppearanceGallery::Shows(const std::string& screen) { return screen == CostumesScreen || screen == ColorsScreen; }
const char* AppearanceGallery::Title(const std::string& screen) {
    return loc::T(screen == CostumesScreen ? "selection.costume_gallery_title" : "selection.color_gallery_title");
}
// A costume opens its colours; a colour saves.
const char* AppearanceGallery::StatusKey(const std::string& screen) {
    return screen == CostumesScreen ? "selection.status_browse" : "selection.status_editable";
}
void AppearanceGallery::Open(MenuNavigation& nav, Page page, const selection::Pick& pick) {
    costume_ = pick.costume;
    const bool costumes = page == Page::Costumes;
    nav.Push(costumes ? CostumesScreen : ColorsScreen);
    nav.Prefer(costumes ? "costume-" + std::to_string(pick.costume) : "color-" + std::to_string(pick.color));
}
void AppearanceGallery::Leave(MenuNavigation& nav) {
    while (Shows(nav.Screen()) && nav.Screen() != nav.Root()) nav.Return();
}
std::vector<MenuEntry> AppearanceGallery::Build(const std::string& screen, const selection::Pick& pick, const selection::Availability& availability,
                                                const selection::InstalledContent* installed, bool editable) {
    using namespace selection;
    pictures_.clear();
    page_ = screen == CostumesScreen ? Page::Costumes : Page::Colors;
    // A room can open the grid before the game has said what the player owns.
    if (!availability.ready) return {Row("waiting", loc::T("selection.loading_choices"), loc::T("selection.loading_choices.detail"), false)};
    std::vector<MenuEntry> rows;
    const auto card = [&](std::string id, std::string label, std::string detail, bool enabled, int key, FighterPicture picture) {
        auto row = Row(std::move(id), std::move(label), std::move(detail), enabled);
        row.key = static_cast<std::uint64_t>(key);
        rows.push_back(std::move(row));
        pictures_.push_back(picture);
    };
    const char* locked = loc::T("selection.locked_detail");
    if (page_ == Page::Costumes) {
        for (const int costume : AllowedCostumes(pick.fighter, availability, installed)) {
            auto option = pick; option.costume = costume;
            const auto colors = AllowedColors(pick.fighter, costume, availability, installed);
            card("costume-" + std::to_string(costume), ShortCostumeLabel(option, installed),
                 colors.empty() ? loc::T("selection.no_colors") : editable ? CostumeLabel(option, installed) : std::string(locked),
                 editable && !colors.empty(), costume, {pick.fighter, FighterArt::Character, costume, GalleryColor(colors, pick.color)});
        }
        return rows;
    }
    auto shown = pick; shown.costume = costume_;
    const std::string costumeName = CostumeLabel(shown, installed);
    for (const int color : AllowedColors(pick.fighter, costume_, availability, installed)) {
        // A custom colour is named by the setup program, else numbered like the rest.
        const std::string name = installed && IsCustomColor(color) ? installed->Name(pick.fighter, costume_, color) : std::string();
        card("color-" + std::to_string(color), name.empty() ? loc::Tf("selection.color", color + 1) : name,
             editable ? costumeName : std::string(locked), editable, color, {pick.fighter, FighterArt::Character, costume_, color});
        if (editable) rows.back().hint = loc::T("menu.hint.save_color");
    }
    if (rows.empty()) rows.push_back(Row("no-colors", loc::T("selection.no_colors"), costumeName, false));
    return rows;
}
void AppearanceGallery::DrawBody(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height, const selection::Pick& pick) {
    if (pictures_.empty() || pictures_.size() != rows.size()) {
        // No cards: the row that says why.
        for (const auto& e : rows) { ImGui::TextWrapped("%s", e.label.c_str()); ImGui::TextWrapped("%s", e.detail.c_str()); }
        return;
    }
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 8 * s;
    const float font = (std::min)(ImGui::GetFontSize(), 15 * s), caption = font + 8 * s;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const bool changed = nav.Focus() != focus_;
    focus_ = nav.Focus();
    const bool costumes = page_ == Page::Costumes;
    const auto grid = FitGallery(rows.size(), width, height, gap, FigureAspect, caption, 64 * s, 220 * s);
    const auto origin = ImGui::GetCursorScreenPos();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& e = rows[i];
        const auto& picture = pictures_[i];
        const auto at = grid.Cell(i, width, gap);
        const v2::Box b{ImVec2(origin.x + at.x, origin.y + at.y), ImVec2(origin.x + at.x + grid.width, origin.y + at.y + grid.height)};
        // The worn costume, and on its colours the worn colour.
        const bool worn = picture.costume == pick.costume && (costumes || picture.color == pick.color);
        const bool hovered = PressEntry(nav, rows, e, b, action, changed).hovered, focused = e.id == nav.Focus();
        v2::CardGround(d, b, focused, hovered, worn);
        DrawCharacterPortrait(picture, ImVec2(b.min.x + 3 * s, b.min.y + 3 * s), ImVec2(b.max.x - 3 * s, b.max.y - caption), IM_COL32(0, 0, 0, 0));
        if (!e.enabled) d->AddRectFilled(b.min, b.max, v2::color::Alpha(c.ink, .65f), 4 * s);
        d->AddRectFilled(ImVec2(b.min.x, b.max.y - caption), b.max, v2::color::Alpha(c.ink, .85f));
        std::string elided;
        DrawCardText(elided, ImVec2(b.min.x + 4 * s, b.max.y - caption + 4 * s), grid.width - 8 * s, e.label, font, worn ? c.accent : c.text, true);
        if (focused) v2::Focus(d, b.min, b.max, false);
    }
    // The grid's whole height, so a grid taller than the body scrolls.
    const auto lines = static_cast<float>(grid.Lines(rows.size()));
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, lines * (grid.height + gap) - gap));
}
AppearanceGallery::Choice AppearanceGallery::Choose(const std::string& id, MenuNavigation& nav, selection::Pick& pick,
                                                    const selection::Availability& availability, const selection::InstalledContent* installed) {
    if (id == "costume") { Open(nav, Page::Costumes, pick); return Choice::Opened; }
    if (id == "color") { Open(nav, Page::Colors, pick); return Choice::Opened; }
    if (const auto costume = RowKey<int>(id, "costume-")) {
        costume_ = *costume;
        nav.Push(ColorsScreen);
        nav.Prefer("color-" + std::to_string(GalleryColor(selection::AllowedColors(pick.fighter, costume_, availability, installed), pick.color)));
        return Choice::Opened;
    }
    if (const auto color = RowKey<int>(id, "color-")) {
        pick.costume = costume_; pick.color = *color;
        Leave(nav);
        return Choice::Saved;
    }
    return Choice::Unhandled;
}
} }
