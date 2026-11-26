#include "../common/RenderStreamCheck.hxx"
#include "../common/BattleBufferEdit.hxx"

#include <cstring>
#include <cstdint>
#include <vector>

#include "test_support.hxx"
using sf4e::render::KeepSoundRanges;
using sf4e::render::KeptRanges;
using sf4e::render::Range;
using sf4e::render::RangeFault;
using sf4e::render::RangeTerminator;

// A stream and its range table as the game writes them.
struct Buffer {
    std::vector<std::uint32_t> stream = std::vector<std::uint32_t>(64);
    std::vector<Range> table;

    // A range of commands of the given lengths, ended by its terminator.
    void PushRange(std::vector<std::uint16_t> lengths, std::uint32_t key) {
        table.push_back({ static_cast<std::uint32_t>(written), key });
        for (std::uint16_t length : lengths) {
            stream[written] = (7u << 16) | length;
            written += length;
        }
        stream[written++] = RangeTerminator;
    }
    KeptRanges Keep() { return KeepSoundRanges(stream.data(), written, table.data(), table.size()); }
    std::size_t written = 0;
};

static void SoundRangesAreAllKept() {
    Buffer b;
    b.PushRange({ 3, 1 }, 1);
    b.PushRange({ 2 }, 2);
    const KeptRanges k = b.Keep();
    CHECK(k.kept == 2 && k.fault == RangeFault::None);
    CHECK(b.table[0].sortKey == 1 && b.table[1].sortKey == 2);
}

// A frame with no ranges, as a merge of nothing leaves it.
static void NoRangesKeepsNothing() {
    const KeptRanges k = KeepSoundRanges(nullptr, 0, nullptr, 0);
    CHECK(k.kept == 0 && k.fault == RangeFault::None);
}

// The last written dword can start a range; the dword after it cannot.
static void StartAtTheEdgeOfTheStream() {
    Buffer b;
    b.PushRange({ 2 }, 1);
    b.PushRange({}, 2);
    CHECK(b.Keep().kept == 2);
    b.table[1].firstDword = static_cast<std::uint32_t>(b.written);
    const KeptRanges k = b.Keep();
    CHECK(k.kept == 1 && k.fault == RangeFault::StartOutside && k.firstBad == 1 && k.badStart == b.written);
}

// What the field dumps held: float bits from the stream where an index was,
// in the middle of the table. Only that range goes; the order stays.
static void GarbageRangeInTheMiddleIsDropped() {
    Buffer b;
    b.PushRange({ 2 }, 1);
    b.PushRange({ 4 }, 2);
    b.PushRange({ 1 }, 3);
    b.PushRange({ 1, 1 }, 4);
    b.table[1].firstDword = 0x4288000F;
    const KeptRanges k = b.Keep();
    CHECK(k.kept == 3 && k.fault == RangeFault::StartOutside && k.firstBad == 1 && k.badStart == 0x4288000F);
    CHECK(b.table[0].sortKey == 1 && b.table[1].sortKey == 3 && b.table[2].sortKey == 4);
}

// A zero word inside the stream would hold the walk in place forever.
static void ZeroLengthIsDropped() {
    Buffer b;
    b.PushRange({ 2 }, 1);
    b.PushRange({ 2, 2 }, 2);
    b.stream[5] = 0;
    const KeptRanges k = b.Keep();
    CHECK(k.kept == 1 && k.fault == RangeFault::ZeroLength && k.firstBad == 1 && k.badStart == 3);
}

// A range whose terminator was overwritten walks out of the written stream.
// Only the whole word 0x10000 ends a range (0x770CFA, 0x770D2D): op 1 with a
// length is a command the walk steps over, to whatever follows it.
static void OnlyTheWholeWordEndsARange() {
    Buffer b;
    b.PushRange({ 2, 1 }, 1);
    b.stream[0] = (1u << 16) | 2;
    CHECK(b.Keep().kept == 1);
    b.stream[3] = (7u << 16) | 5;
    const KeptRanges k = b.Keep();
    CHECK(k.kept == 0 && k.fault == RangeFault::RunsPastEnd);
}

static void MissingTerminatorIsDropped() {
    Buffer b;
    b.PushRange({ 2 }, 1);
    b.stream[b.written - 1] = (7u << 16) | 1;
    const KeptRanges k = b.Keep();
    CHECK(k.kept == 0 && k.fault == RangeFault::RunsPastEnd);
}

namespace {
struct Pages {
    int refuse = -1, unlocks = 0, open = 0;
    bool Unlock(std::uint8_t*, std::uint8_t, unsigned long&) { if (unlocks == refuse) return false; ++unlocks; ++open; return true; }
    void Lock(std::uint8_t*, std::uint8_t, unsigned long) { --open; }
};
}

// The two pushes of EvaApp::onInit as this game build holds them, and as
// the widening leaves them.
const std::uint8_t NativeRanges[5] = { 0x68, 0x00, 0x0C, 0x00, 0x00 }, NativeDwords[5] = { 0x68, 0x00, 0x00, 0x05, 0x00 };
const std::uint8_t WideRanges[5] = { 0x68, 0x00, 0x18, 0x00, 0x00 }, WideDwords[5] = { 0x68, 0x00, 0x00, 0x0A, 0x00 };

struct Pushes {
    std::uint8_t ranges[5], dwords[5];
    Pushes() { std::memcpy(ranges, NativeRanges, 5); std::memcpy(dwords, NativeDwords, 5); }
    bool Widen(Pages& pages) { return sf4e::render::WidenBattleBuffer(ranges, dwords, pages); }
    bool Read(const std::uint8_t (&r)[5], const std::uint8_t (&d)[5]) const {
        return std::memcmp(ranges, r, 5) == 0 && std::memcmp(dwords, d, 5) == 0;
    }
};

static void NativePushesBecomeWide() {
    Pushes p;
    Pages pages;
    CHECK(p.Widen(pages) && pages.open == 0);
    CHECK(p.Read(WideRanges, WideDwords));
}

// Already wide, or another game build: neither place is written.
static void OtherBytesAreLeftAlone() {
    Pages pages;
    Pushes wide;
    std::memcpy(wide.ranges, WideRanges, 5); std::memcpy(wide.dwords, WideDwords, 5);
    CHECK(!wide.Widen(pages) && wide.Read(WideRanges, WideDwords));
    const std::uint8_t other[5] = { 0x68, 0x00, 0x00, 0x06, 0x00 };
    Pushes mixed;
    std::memcpy(mixed.dwords, other, 5);
    CHECK(!mixed.Widen(pages) && mixed.Read(NativeRanges, other));
}

// One place that cannot be made writable changes neither.
static void ARefusedPlaceChangesNeither() {
    Pushes p;
    Pages refusing; refusing.refuse = 1;
    CHECK(!p.Widen(refusing) && refusing.open == 0);
    CHECK(p.Read(NativeRanges, NativeDwords));
}

int main() {
    NativePushesBecomeWide();
    OtherBytesAreLeftAlone();
    ARefusedPlaceChangesNeither();
    SoundRangesAreAllKept();
    NoRangesKeepsNothing();
    StartAtTheEdgeOfTheStream();
    GarbageRangeInTheMiddleIsDropped();
    ZeroLengthIsDropped();
    OnlyTheWholeWordEndsARange();
    MissingTerminatorIsDropped();
    return 0;
}
