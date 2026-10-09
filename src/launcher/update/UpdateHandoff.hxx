#pragma once
#include <atomic>

namespace sf4e { namespace launcher {
// Preparation may finish work after its last check of `cancel`, and nothing
// can cancel the installation once another process runs it. Ask once more
// before handing it over.
template<class Spawn>
bool HandoffPreparedUpdate(const std::atomic<bool>& cancel, Spawn&& spawn) {
    if(cancel) return false;
    spawn();
    return true;
}
} }
