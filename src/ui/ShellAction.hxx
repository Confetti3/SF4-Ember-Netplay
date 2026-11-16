#pragma once
// What a press in the menu asks: a command for the runtime
// (netplay/RuntimeCommands.hxx), built where the player's choice is known, or
// one of the overlay's own steps, which nothing is sent for.
#include "../netplay/RuntimeCommands.hxx"
#include <optional>
#include <type_traits>
#include <utility>

namespace sf4e { namespace ui {

// Steps the chosen Ultra or color by delta (the table page's Ultra and
// Appearance rows, the post-match prompt); the overlay applies it to the pick.
struct SelectionStep {
    enum class Field { Ultra, Color } field = Field::Ultra;
    int delta = 0;
};
// Training select's Start: the overlay hands the game both picks and the stage.
struct StartTraining {};
// Training select's Hitboxes row: the game shows its boxes over Training, or
// not, and keeps the choice (ShellGame::ShowHitboxes).
struct ShowHitboxes { bool shown = false; };
// Settings' Background artwork row: the game shows its artwork behind the
// menu, or not, and keeps the choice (ShellGame::ShowMenuArtwork).
struct ShowMenuArtwork { bool shown = false; };
// Settings' Screen mode row: the game keeps the choice, 0 to kScreenModes - 1,
// for its next start (ShellGame::SetScreenMode).
struct SetScreenMode { int mode = 0; };

using ShellAction = RuntimeRequestWith<SelectionStep, StartTraining, ShowHitboxes, ShowMenuArtwork, SetScreenMode>;

// The overlay's own steps: the alternatives ShellAction adds to RuntimeRequest.
template <class Alternative>
inline constexpr bool IsOverlayStep = std::is_same_v<Alternative, SelectionStep> || std::is_same_v<Alternative, StartTraining> ||
    std::is_same_v<Alternative, ShowHitboxes> || std::is_same_v<Alternative, ShowMenuArtwork> || std::is_same_v<Alternative, SetScreenMode>;

// The runtime's command an action is; none for the overlay's own steps.
inline std::optional<RuntimeRequest> ForRuntime(ShellAction action) {
    return std::visit([](auto& alternative) -> std::optional<RuntimeRequest> {
        using Alternative = std::decay_t<decltype(alternative)>;
        if constexpr (IsOverlayStep<Alternative>) return std::nullopt;
        else return RuntimeRequest(std::move(alternative));
    }, action);
}

} }
