#pragma once
#include <cstdint>
#include <string>

namespace sf4e { namespace quickrematch {
enum class Choice { None, Rematch, Lobby };
// Immutable presentation data copied by the game thread. offer identifies
// one result decision in a continuous set.
// accepted means Rematch was requested; Lobby remains available while the
// native owner still publishes this offer, until it commits or withdraws it.
struct View {
    std::uint64_t offer = 0;
    bool visible = false, accepted = false, canRematch = false;
    std::string status;
};
inline bool Shown(const View& view) { return view.visible && view.offer != 0; }

} }
