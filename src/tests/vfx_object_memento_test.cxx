// The engine's Vfx::Object memento copied one dword past each 0x500-byte
// object. For the last object of an array that dword is the next heap block's
// header, so a rollback restore wrote a stale block size into a block the game
// had since reallocated (DB's v1.0.1 crash, 0xC0000374). ClampCopyCount turns
// the copy count in the game's `mov ecx, 0E4h` into 0xE3.
#include "../common/VfxObjectMemento.hxx"
#include <cstdint>
#include <cstring>
#include <vector>

#include "test_support.hxx"

namespace memento = sf4e::vfx_memento;

// Two objects as the container lays them out, then the next block's header.
struct Pool {
    std::uint8_t objects[2][memento::kObjectStride];
    std::uint32_t nextHeader[2];
};

// What `rep movsd` does in the restore at 0x5BBCB0.
static void Restore(Pool& pool, int index, const std::uint8_t* saved, std::uint32_t dwords) {
    std::memcpy(&pool.objects[index][memento::kCopyOffset], saved, dwords * 4);
}

static void CheckOverrunAndFix() {
    static_assert(sizeof(Pool) == 2 * memento::kObjectStride + 8, "no padding between objects and header");
    std::vector<std::uint8_t> saved(memento::kNativeDwords * 4, 0x5A);
    const std::uint32_t header[2] = { 0x2E00002E, 0x0D4B69CE };

    Pool pool;
    std::memset(&pool, 0, sizeof(pool));
    std::memcpy(pool.nextHeader, header, sizeof(header));
    Restore(pool, 1, saved.data(), memento::kNativeDwords);
    CHECK(pool.nextHeader[0] == 0x5A5A5A5A); // the native count reaches the header
    CHECK(pool.nextHeader[1] == header[1]);

    std::memset(&pool, 0, sizeof(pool));
    std::memcpy(pool.nextHeader, header, sizeof(header));
    Restore(pool, 1, saved.data(), memento::kFixedDwords);
    CHECK(std::memcmp(pool.nextHeader, header, sizeof(header)) == 0);
    CHECK(pool.objects[1][memento::kCopyOffset] == 0x5A);
    CHECK(pool.objects[1][memento::kObjectStride - 1] == 0x5A); // the object's last byte still restores

    // An object inside the array no longer touches its neighbour either.
    std::memset(&pool, 0, sizeof(pool));
    Restore(pool, 0, saved.data(), memento::kFixedDwords);
    CHECK(pool.objects[1][0] == 0);
}

static void CheckPatch() {
    std::uint8_t native[] = { 0xB9, 0xE4, 0x00, 0x00, 0x00 };
    CHECK(memento::ClampCopyCount(native) == memento::Patch::Applied);
    const std::uint8_t fixed[] = { 0xB9, 0xE3, 0x00, 0x00, 0x00 };
    CHECK(std::memcmp(native, fixed, sizeof(fixed)) == 0);
    CHECK(memento::ClampCopyCount(native) == memento::Patch::AlreadyApplied);
    CHECK(std::memcmp(native, fixed, sizeof(fixed)) == 0);

    std::uint8_t otherCount[] = { 0xB9, 0xE5, 0x00, 0x00, 0x00 };
    const std::uint8_t otherCopy[] = { 0xB9, 0xE5, 0x00, 0x00, 0x00 };
    CHECK(memento::ClampCopyCount(otherCount) == memento::Patch::Unexpected);
    CHECK(std::memcmp(otherCount, otherCopy, sizeof(otherCopy)) == 0);

    std::uint8_t otherInstruction[] = { 0xBA, 0xE4, 0x00, 0x00, 0x00 };
    const std::uint8_t instructionCopy[] = { 0xBA, 0xE4, 0x00, 0x00, 0x00 };
    CHECK(memento::ClampCopyCount(otherInstruction) == memento::Patch::Unexpected);
    CHECK(std::memcmp(otherInstruction, instructionCopy, sizeof(instructionCopy)) == 0);
}

int main() {
    CheckOverrunAndFix();
    CheckPatch();
    return 0;
}
