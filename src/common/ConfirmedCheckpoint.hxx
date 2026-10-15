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

// The state frame a simulation of input N reaches: N + 1. A player simulates
// the input of the state its engine holds (the frame GGPO last saved or a
// rollback loaded); a spectator the confirmed input it just consumed.
inline int StateFrameAfterInput(int input) {
    return input >= 0 && input < (std::numeric_limits<int>::max)() ? input + 1 : -1;
}

// The remote hashes waiting for the local checkpoint of their frame, compared
// in frame order once that checkpoint is confirmed here. The ring's slot order
// wraps, so frame order is what makes the first mismatch reported the
// earliest on both PCs. find(frame) gives the local checkpoint or null;
// report(local, remote) compares the two, and the remote hash is dropped.
template <class Pending, class Find, class Report>
void ReconcileRemoteHashes(Pending& pending, int lastConfirmedInput, Find find, Report report) {
    for (auto it = pending.begin(); it != pending.end();) {
        const auto* local = find(it->first);
        if (!local || !IsConfirmedCheckpoint(local->ggpoStateFrame, lastConfirmedInput)) {
            ++it;
            continue;
        }
        report(*local, it->second);
        it = pending.erase(it);
    }
}

} }
