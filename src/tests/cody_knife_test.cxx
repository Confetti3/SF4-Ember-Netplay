// The desync hash reads Cody's knife from the knife item at the offsets the
// engine's pickup and throw use (common/CodyKnife.hxx), and only once the
// round's fight has dropped it. These buffers are laid out at those offsets.
#include "../common/CodyKnife.hxx"
#include <cstdint>
#include <cstring>
#include <vector>

#include "test_support.hxx"

namespace knife = sf4e::knife;

static void Put(std::vector<uint8_t>& object, size_t offset, uint32_t word) {
    std::memcpy(object.data() + offset, &word, sizeof(word));
}

static uint32_t Bits(float value) {
    uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

static void CheckWords() {
    std::vector<uint8_t> actor(knife::kActorSavedEnd, 0xCD);
    std::vector<uint8_t> item(0x170, 0xCD);
    Put(actor, knife::kActorItemIndex, 3);
    Put(item, knife::kItemState, 2);
    Put(item, knife::kItemPosition, Bits(2.0f));

    CHECK(knife::ItemIndex(actor.data()) == 3);
    const knife::Words words = knife::Read(item.data());
    CHECK(words.state == 2);
    CHECK(words.x == Bits(2.0f));

    // A knife that moved is a different word; nothing else in the item is read.
    Put(item, knife::kItemPosition, Bits(2.5f));
    std::memset(item.data(), 0, knife::kItemPosition);
    CHECK(knife::Read(item.data()).x == Bits(2.5f));
    CHECK(knife::Read(item.data()).state == 2);

    // In his hand the knife's position is stale and does not count.
    Put(item, knife::kItemState, 0);
    CHECK(knife::Read(item.data()).x == 0);
    Put(item, knife::kItemState, 1);
    CHECK(knife::Read(item.data()).x == Bits(2.5f));
}

// No item at the actor's index (the collection is gone or shorter).
static void CheckMissingKnife() {
    const knife::Words words = knife::Read(nullptr);
    CHECK(words.state == 0);
    CHECK(words.x == 0);
}

// Only FIGHT counts, and only once a frame of it has passed, so the round
// start's drop has been applied; before that the item can hold another
// battle's values.
static void CheckRoundGate() {
    CHECK(!knife::RoundDropApplied(knife::kFlowFight, 0));
    CHECK(knife::RoundDropApplied(knife::kFlowFight, 1));
    CHECK(knife::RoundDropApplied(knife::kFlowFight, 5000));
    CHECK(!knife::RoundDropApplied(1, 30));  // READY
    CHECK(!knife::RoundDropApplied(14, 30)); // ROUND_START
    CHECK(!knife::RoundDropApplied(16, 30)); // IDLE, an in-place restart
    CHECK(!knife::RoundDropApplied(3, 30));  // FINISH
}

// The ranges the engine's mementos copy (0x53B350, 0x56A310); the header's
// static_asserts keep every word read inside them.
static_assert(knife::kActorSavedEnd - knife::kActorSavedBegin == 0x6CF0, "the actor memento's copy");
static_assert(knife::kItemSavedEnd - knife::kItemSavedBegin == 0x130, "the knife memento's copy");

int main() {
    CheckWords();
    CheckMissingKnife();
    CheckRoundGate();
    return 0;
}
