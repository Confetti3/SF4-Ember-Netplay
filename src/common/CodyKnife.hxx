#pragma once

// Cody's knife as the simulation sees it, for the desync hash.
//
// The knife is an item of the chara unit's item collection
// (Chara::impl::ItemCDY_Knife, 0x170 bytes). Gameplay reads its state
// (item +0x114: 0 in his hand or not out, 1 flying, 2 on the ground) and its
// position (item +0x50, 0x56A4B0 and 0x56A4D0): the pickup needs it on the
// ground (0x56A490 from 0x557C10), within 0.5 (0.75 in some states) of
// Cody's x (0x542EF0). The item update (0x56AFF0, in the chara unit's pair
// job) turns the knife's request (item +0x110) into its state. Both words are
// inside the knife's memento (0x56A310), which Ember records and restores.
// Neither was in a hash, so a knife that parted between the PCs, for
// instance one landing somewhere else after a throw, stayed invisible until
// Cody next reached for it, then showed only as a different move.
//
// Hashed only once the round has dropped the knife. Before that its state
// and position can be left from an earlier battle in the same process, which
// two PCs need not share: the battle load resets the state (0x56A2E0, from the
// unit's 0x5635A0) but not the position, and no reset of either was found on
// an in-place restart (0x5DA220). Every round start drops it: SetBattleFlow (0x5DCE90)
// sends each new flow to the units, and the unit answers ROUND_START (14) by
// requesting the knife on the ground at a stage-fixed spot (0x563740 ->
// 0x563630, request 3 and position). FIGHT (2) is only ever entered from the
// ROUND_START handler (0x5DD970) or the READY substate it leads to (0x5DCFB0;
// READY is entered only from 0x5DD970); no other SetBattleFlow call or inline
// write of the flow (0xA64994) sets 2. One frame into FIGHT the item update
// has applied that request, so from then on state and position follow only
// from the round's inputs.
//
// Whether Cody holds it (actor +0x6E08) is left out: the READY reset
// (0x563B80, through unit vtable slot 19) clears it, but 0x5DD970 can go from
// ROUND_START straight to FIGHT without READY, and a knife in his hand already
// shows as state 0.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sf4e { namespace knife {

// Index of the actor's first item in the unit's item collection (impl::
// ItemCollection, vtable slot 8 0x56B3F0, null past the end).
constexpr size_t kActorItemIndex = 0x6E34;
// The actor memento copies object +0x2A4..+0x6F94 (0x53B350).
constexpr size_t kActorSavedBegin = 0x2A4;
constexpr size_t kActorSavedEnd = 0x6F94;
static_assert(kActorItemIndex >= kActorSavedBegin && kActorItemIndex + 4 <= kActorSavedEnd, "the item index rolls back with the actor");

// Knife object offsets.
constexpr size_t kItemPosition = 0x50;
constexpr size_t kItemState = 0x114;
// The knife memento copies object +0x20..+0x150 (0x56A310), up to its key.
constexpr size_t kItemSavedBegin = 0x20;
constexpr size_t kItemSavedEnd = 0x150;
static_assert(kItemPosition >= kItemSavedBegin && kItemPosition + 4 <= kItemSavedEnd, "the position rolls back with the knife");
static_assert(kItemState >= kItemSavedBegin && kItemState + 4 <= kItemSavedEnd, "the state rolls back with the knife");

constexpr uint32_t kFlowFight = 2; // System::BF__FIGHT

// Whether this round's drop has been applied: the battle flow is FIGHT and
// its frame counter (0xAA9264, reset by every flow change) has passed 1.
inline bool RoundDropApplied(uint32_t battleFlow, int flowFrames) {
    return battleFlow == kFlowFight && flowFrames >= 1;
}

// The words hashed for one fighter: zero for anyone but Cody, and outside a
// round's fight.
struct Words {
    uint32_t state = 0;
    uint32_t x = 0; // float bit pattern
};

inline uint32_t Word(const uint8_t* object, size_t offset) {
    uint32_t word;
    std::memcpy(&word, object + offset, sizeof(word));
    return word;
}

inline int ItemIndex(const uint8_t* actor) {
    return static_cast<int>(Word(actor, kActorItemIndex));
}

// `knife` is the item ItemIndex names, null when the collection has none.
// The position is read only while the knife is out (flying or on the ground),
// so x counts only then.
inline Words Read(const uint8_t* knife) {
    Words out;
    if (knife) {
        out.state = Word(knife, kItemState);
        out.x = out.state != 0 ? Word(knife, kItemPosition) : 0;
    }
    return out;
}

}}
