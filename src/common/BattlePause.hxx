#pragma once
#include <atomic>

namespace sf4e {
// The pause/hold bit of the battle's simulation flags, System::SSF_PAUSE_OR_HOLD;
// sf4e__Game__Battle__System.cxx checks that the two agree.
constexpr unsigned NativePauseOrHoldFlag = 0x1;
// The bit also holds native Training load-state input. Only a pausing player
// identifies a real menu; native online wait (0x4) and replay freeze (0x8) do not.
inline bool NativePauseMenuOpen(unsigned flags, int pausingPlayer) {
    return (flags & NativePauseOrHoldFlag) != 0 && pausingPlayer != -1;
}
// The game thread publishes the menu state; the overlay reads no native memory.
class BattlePauseState {
public:
    void Publish(bool paused) { paused_.store(paused); }
    bool Paused() const { return paused_.load(); }
    void StartBattle() { Publish(false); }
    void CloseBattle() { Publish(false); }
private:
    std::atomic<bool> paused_{false};
};
inline BattlePauseState battlePause;
// Render tests exercise visibility without constructing a native System.
inline void SetNativePauseForTest(bool paused) { battlePause.Publish(paused); }
}
