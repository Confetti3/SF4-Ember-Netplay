#pragma once
#include <cstdint>
#include <limits>

namespace sf4e { namespace statehash {

// GGPO save frame N is the state before input N, hence it includes input N-1.
// No age heuristic can substitute for the backend's confirmation boundary.
inline bool IsConfirmedCheckpoint(int stateFrame, int lastConfirmedInput) {
    return stateFrame > 0 && lastConfirmedInput >= 0 && stateFrame - 1 <= lastConfirmedInput;
}

// The native engine's signed 16-bit frame field wraps at 32767/-32768.  The
// GGPO state frame is monotonic and is therefore the only safe identity for a
// checkpoint cadence, ring slot, confirmation boundary, or wire key.
constexpr int CheckpointInterval = 30;
constexpr int CheckpointRingSize = 64;

inline bool IsValidCheckpointStateFrame(int stateFrame) {
    return stateFrame > 0;
}

inline bool IsCheckpointCadenceFrame(int stateFrame) {
    return IsValidCheckpointStateFrame(stateFrame) && stateFrame % CheckpointInterval == 0;
}

inline int CheckpointRingIndex(int stateFrame) {
    return IsCheckpointCadenceFrame(stateFrame)
        ? (stateFrame / CheckpointInterval) % CheckpointRingSize : -1;
}

// The identity and publication gate are updated together by the production
// capture path.  In particular, a rollback recapture of the same GGPO state
// frame must reopen publication after its semantic hash is replaced.  Keeping
// this seam here lets focused tests exercise the real state transition without
// constructing the native battle object.
inline bool PrepareCheckpointIdentity(int& frameIdx, int& ggpoStateFrame,
    bool& sent, int stateFrame) {
    if (CheckpointRingIndex(stateFrame) < 0) return false;
    frameIdx = stateFrame;
    ggpoStateFrame = stateFrame;
    sent = false;
    return true;
}

inline int SpectatorCheckpointStateFrame(int lastConfirmedInput) {
    return lastConfirmedInput >= 0 && lastConfirmedInput < (std::numeric_limits<int>::max)()
        ? lastConfirmedInput + 1 : -1;
}

} }
