// Background play edits USF4's three "window in front" checks: the two pad
// gates become short jumps past the gate, and the app frame's sound check
// reads the foreground window through Ember. All edits land together or none
// does, and every place made writable is locked again.
#include "../common/FocusGate.hxx"
#include "../sf4e/sf4e__BackgroundPlay.hxx"
#include <cstdint>
#include <cstring>

#include "test_support.hxx"

namespace gate = sf4e::focus_gate;

// Where a short jump at `at` lands.
static std::uint32_t Target(std::uint32_t at, std::uint8_t displacement) {
    return at + 2 + static_cast<std::int8_t>(displacement);
}

static void CheckTargets() {
    // Pad update: test ebx,ebx at 0x5121AD; 0x5121D5 is `cmp byte_D7D2E4, 0`.
    CHECK(Target(0x5121AD, gate::kUpdateSkip) == 0x5121D5);
    // Pad poll: test ebx,ebx at 0x6D8D8A; 0x6D8DBE is `cmp [esi+1F74h], 0`.
    CHECK(Target(0x6D8D8A, gate::kPollSkip) == 0x6D8DBE);
}

// The game's bytes: 0x5121AD (test ebx,ebx; jz short), 0x6D8D8A (test ebx,ebx;
// jz near) and 0x4042F8 (call dword ptr [GetForegroundWindow], slot 0x9312AC).
struct Code {
    std::uint8_t update[4] = { 0x85, 0xDB, 0x74, 0x36 };
    std::uint8_t poll[4] = { 0x85, 0xDB, 0x0F, 0x84 };
    std::uint8_t sound[8] = { 0xFF, 0x15, 0xAC, 0x12, 0x93, 0x00, 0x8B, 0xD8 };
};
static const Code native;
const std::uint32_t kImport = 0x009312AC, kEmber = 0x12345678;

static bool Same(const Code& a, const Code& b) { return std::memcmp(&a, &b, sizeof(Code)) == 0; }

struct Edits {
    gate::Edit list[3];
    explicit Edits(Code& code) : list{ gate::Gate(code.update, gate::kUpdateSkip), gate::Gate(code.poll, gate::kPollSkip),
        gate::CallThrough(code.sound, kImport, kEmber) } {}
};

// Refuses the unlock numbered `refuse` (from 0); counts what stays writable.
struct Pages {
    int refuse = -1, unlocks = 0, open = 0;
    bool Unlock(std::uint8_t*, std::uint8_t, unsigned long& saved) {
        if (unlocks++ == refuse) return false;
        saved = 0x20; // PAGE_EXECUTE_READ
        ++open;
        return true;
    }
    void Lock(std::uint8_t*, std::uint8_t, unsigned long saved) {
        CHECK(saved == 0x20);
        --open;
    }
};

static void CheckAllOrNone() {
    Code code;
    Edits edits(code);
    Pages pages;
    CHECK(gate::ApplyAll(edits.list, pages) && pages.unlocks == 3 && pages.open == 0);
    const std::uint8_t update[] = { 0xEB, gate::kUpdateSkip, 0x74, 0x36 };
    const std::uint8_t poll[] = { 0xEB, gate::kPollSkip, 0x0F, 0x84 };
    const std::uint8_t sound[] = { 0xFF, 0x15, 0x78, 0x56, 0x34, 0x12, 0x8B, 0xD8 };
    CHECK(std::memcmp(code.update, update, 4) == 0 && std::memcmp(code.poll, poll, 4) == 0 &&
        std::memcmp(code.sound, sound, 8) == 0);

    // Already edited: not the game's bytes, so nothing is unlocked or written.
    const Code edited = code;
    Pages again;
    CHECK(!gate::ApplyAll(edits.list, again) && again.unlocks == 0 && Same(code, edited));

    // Any place that cannot be made writable: all stay native, none stays writable.
    for (int refuse = 0; refuse < 3; ++refuse) {
        Code refused;
        Edits refusedEdits(refused);
        Pages refusing; refusing.refuse = refuse;
        CHECK(!gate::ApplyAll(refusedEdits.list, refusing) && Same(refused, native) && refusing.open == 0);
    }

    // Any place holding other bytes (another game build) leaves all of them alone.
    for (int place = 0; place < 3; ++place) {
        Code other;
        std::uint8_t* changed[] = { other.update + 1, other.poll, other.sound + 2 };
        *changed[place] ^= 0x01;
        const Code before = other;
        Edits otherEdits(other);
        Pages untouched;
        CHECK(!gate::ApplyAll(otherEdits.list, untouched) && untouched.unlocks == 0 && Same(other, before));
    }
}

// What the game keeps doing behind another window. An export is heard even
// minimized; a watched match runs on but stays muted; the setting alone keeps
// the sound and the pads but never runs a paused game on.
static void CheckPolicy() {
    using sf4e::BackgroundPlay::PolicyFor;
    const auto none = PolicyFor(false, false, false);
    CHECK(!none.backgroundInput && !none.keepSound && !none.keepRunning && !none.keepSoundMinimized);
    const auto setting = PolicyFor(true, false, false);
    CHECK(setting.backgroundInput && setting.keepSound && !setting.keepRunning && !setting.keepSoundMinimized);
    const auto exporting = PolicyFor(false, true, false);
    CHECK(!exporting.backgroundInput && exporting.keepSound && exporting.keepRunning && exporting.keepSoundMinimized);
    const auto watching = PolicyFor(false, false, true);
    CHECK(!watching.backgroundInput && !watching.keepSound && watching.keepRunning && !watching.keepSoundMinimized);
    const auto watchingWithSetting = PolicyFor(true, false, true);
    CHECK(watchingWithSetting.backgroundInput && watchingWithSetting.keepSound && watchingWithSetting.keepRunning &&
        !watchingWithSetting.keepSoundMinimized);
}

int main() {
    CheckTargets();
    CheckAllOrNone();
    CheckPolicy();
    return 0;
}
