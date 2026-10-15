#pragma once

// The last frames a battle simulated, one row per GGPO state frame, so a desync
// report can show where two PCs parted between two checkpoints (30 frames
// apart). A rollback re-simulates frames; each pass replaces the frame's row
// and is counted, so the row a confirmed frame keeps is the timeline both PCs
// agreed on and `passes` says whether it was ever re-simulated. The trace is
// logged once a battle, for the first mismatch: later ones follow from it.

#include <array>

namespace sf4e { namespace statehash {

template <class Row, int Size>
class FrameTrace {
public:
    struct Entry {
        int stateFrame = -1;
        int passes = 0;
        Row row = {};
    };

    // The entry for stateFrame, its row cleared. A frame already held there
    // counts one more pass; any other frame in the slot is dropped.
    Entry& Begin(int stateFrame) {
        Entry& entry = entries_[Slot(stateFrame)];
        entry.passes = entry.stateFrame == stateFrame ? entry.passes + 1 : 1;
        entry.stateFrame = stateFrame;
        entry.row = Row();
        return entry;
    }

    // Null when the frame was never traced or its slot has moved on.
    const Entry* Find(int stateFrame) const {
        if (stateFrame < 0) return nullptr;
        const Entry& entry = entries_[Slot(stateFrame)];
        return entry.stateFrame == stateFrame ? &entry : nullptr;
    }

    // True the first time a battle asks to log the trace, false after.
    bool TakeDump() {
        if (dumped_) return false;
        dumped_ = true;
        return true;
    }

    void Clear() {
        entries_ = {};
        dumped_ = false;
    }

private:
    static int Slot(int stateFrame) { return (stateFrame % Size + Size) % Size; }
    std::array<Entry, Size> entries_ = {};
    bool dumped_ = false;
};

} }
