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

using ShellAction = RuntimeRequestWith<SelectionStep, StartTraining>;

// The runtime's command an action is; none for the overlay's own steps.
inline std::optional<RuntimeRequest> ForRuntime(ShellAction action) {
    return std::visit([](auto& alternative) -> std::optional<RuntimeRequest> {
        using Alternative = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<Alternative, SelectionStep> || std::is_same_v<Alternative, StartTraining>) return std::nullopt;
        else return RuntimeRequest(std::move(alternative));
    }, action);
}

} }
