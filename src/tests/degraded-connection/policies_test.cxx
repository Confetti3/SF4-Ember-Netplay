#include "../../common/sf4e__PacingController.hxx"
#include "../../../vcpkg-ports/ggpo/input-repair.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

// Deliberately independent of assert/NDEBUG: Release builds must test too.
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
    std::exit(1); } } while (0)

using sf4e::pacing::PacingController;
using sf4e::netplay::InputRepair;

static PacingController Pacer() {
    PacingController p;
    p.InitDefaults();
    return p;
}
static InputRepair Repair() {
    InputRepair r;
    r.SetEnabled(true);
    return r;
}

static void RiftDirection() {
    auto ahead = Pacer();
    for (int i = 0; i < 600; ++i) ahead.OnRiftSample(-4, 4);
    CHECK(ahead.outstandingMs > 0 && ahead.riftFramesEma == 4);
    // Behind the peer: the other side waits, this side never does.
    auto behind = Pacer();
    for (int i = 0; i < 600; ++i) behind.OnRiftSample(4, -4);
    CHECK(behind.outstandingMs == 0 && behind.riftFramesEma == -4);
    auto disabled = Pacer();
    disabled.enabled = false;
    for (int i = 0; i < 600; ++i) disabled.OnRiftSample(-4, 4);
    CHECK(disabled.outstandingMs == 0);
}

static void RiftDeadZoneAndCap() {
    auto p = Pacer();
    // Half a frame ahead is inside the dead zone.
    for (int i = 0; i < 600; ++i) p.OnRiftSample(0, 1);
    CHECK(p.outstandingMs == 0);
    // Blocked waits cannot grow a lump beyond two slices.
    for (int i = 0; i < 6000; ++i) p.OnRiftSample(-9, 9);
    CHECK(p.outstandingMs > 0 && p.outstandingMs <= 2 * p.maxStepMs);
    CHECK(p.maxOutstandingMs <= 2 * p.maxStepMs);
}

static void RiftReset() {
    auto p = Pacer();
    for (int i = 0; i < 100; ++i) p.OnRiftSample(-4, 4);
    CHECK(p.hasRift && p.outstandingMs > 0);
    p.Reset();
    CHECK(!p.hasRift && p.riftFramesEma == 0 && p.outstandingMs == 0);
    // One sample after a reset starts the average again; it is not diluted.
    p.OnRiftSample(-2, 2);
    CHECK(p.riftFramesEma == 2);
}

static void RiftHoldsAfterStall() {
    auto p = Pacer();
    p.OnRiftSample(0, 0);
    p.OnPredictionStall();
    // Samples taken in the aftermath of a stall are garbage and are dropped.
    for (int i = 0; i < p.riftHoldTicks; ++i) p.OnRiftSample(-14, 14);
    CHECK(p.riftFramesEma == 0 && p.outstandingMs == 0);
    p.OnRiftSample(-14, 14);
    CHECK(p.riftFramesEma > 0);
    p.OnPredictionStall();
    p.Reset();
    CHECK(p.riftHoldRemaining == 0);
}

// Closed loop with integer stats and feedback delayed by a fifth of a second,
// as the real quality reports are. The rift must settle near the dead zone
// without ever crossing to the other side.
static void RiftConvergesWithoutOvershoot() {
    auto p = Pacer();
    double rift = 4.0, lowest = rift;
    std::vector<int> delayed(12, 4);
    for (int tick = 0; tick < 1800; ++tick) {
        const int seen = delayed[tick % delayed.size()];
        delayed[tick % delayed.size()] = int(rift + (rift < 0 ? -0.5 : 0.5));
        p.OnRiftSample(-seen, seen);
        const double wait = p.NextWaitMs();
        CHECK(wait <= p.maxStepMs);
        p.OnWaited(wait);
        rift -= wait / (1000.0 / 60.0);
        if (rift < lowest) lowest = rift;
    }
    CHECK(rift < 1.5);
    CHECK(lowest > 0.0);
}

static void RepairOptInAndHealthyTraffic() {
    InputRepair baseline;
    baseline.OnInputSent(0, 10);
    CHECK(!baseline.TryRepair(1000, true));
    auto r = Repair();
    CHECK(!r.TryRepair(1000, true));
    for (uint32_t ms = 0; ms < 10000; ms += 16) {
        r.OnInputSent(ms, int(ms / 16));
        CHECK(!r.TryRepair(ms + 15, true));
    }
    CHECK(r.Repairs() == 0);
}

static void RepairBackoffAndBudget() {
    auto r = Repair();
    r.OnInputSent(0, 10);
    const uint32_t times[] = {33, 99, 231, 431};
    for (auto now : times) {
        CHECK(!r.TryRepair(now - 1, true));
        CHECK(r.TryRepair(now, true));
        CHECK(!r.TryRepair(now, true));
        r.OnInputSent(now, 10); // Real host regenerates the same pending input.
    }
    CHECK(!r.TryRepair(1032, true));
    CHECK(r.TryRepair(1033, true));
    CHECK(r.Repairs() == 5);
}

static void RepairAcknowledgments() {
    auto r = Repair();
    r.OnInputSent(0, 10);
    r.OnAck(11); // Impossible forward ACK cannot make pending input disappear.
    CHECK(r.TryRepair(33, true));
    r.OnAck(5); // Genuine progress may shorten backoff.
    CHECK(r.TryRepair(66, true));
    r.OnAck(5); // Duplicate/stale ACKs must not reset that backoff.
    r.OnAck(4);
    CHECK(!r.TryRepair(99, true));
    CHECK(r.TryRepair(132, true));
    r.OnAck(10); // GGPO retains the ACKed frame: it is not an unacked input.
    CHECK(!r.TryRepair(1000, true));
    r.OnInputSent(1000, 11);
    CHECK(r.TryRepair(1033, true));
}

static void RepairBackpressureAndWrap() {
    auto r = Repair();
    const uint32_t start = UINT32_MAX - 10;
    r.OnInputSent(start, 5);
    CHECK(!r.TryRepair(start + 32, true));
    CHECK(!r.TryRepair(start + 33, false));
    CHECK(r.Repairs() == 0);
    CHECK(r.TryRepair(start + 33, true));
    CHECK(!r.TryRepair(start + 98, true));
    CHECK(r.TryRepair(start + 99, true));
}

static void RepairRollingBudgetUnderProgress() {
    auto r = Repair();
    std::vector<uint32_t> sends;
    for (uint32_t now = 0; now < 10000; ++now) {
        if (now % 100 == 0) {
            r.OnInputSent(now, int(now / 100 + 1));
            r.OnAck(int(now / 100));
        }
        if (r.TryRepair(now, true)) sends.push_back(now);
    }
    CHECK(!sends.empty());
    for (size_t i = 4; i < sends.size(); ++i) {
        CHECK(sends[i] - sends[i - 4] >= 1000);
    }
}

// A scheduling fixture, not a network-performance benchmark. Incorporates
// the unchanged 200-ms fallback before the optional repair path, as the
// actual GGPO patch does. No latency, QUIC, game simulation, or ACK wire here.
static uint32_t FirstDeliveredSend(bool experiment, uint32_t lossEndsMs) {
    InputRepair r;
    r.SetEnabled(experiment);
    r.OnInputSent(0, 10);
    uint32_t fallback = 0;
    for (uint32_t now = 1; now <= lossEndsMs + 1000; ++now) {
        bool send = false;
        if (now - fallback > 200) {
            fallback = now;
            send = true;
        } else {
            send = r.TryRepair(now, true);
        }
        if (send) {
            r.OnInputSent(now, 10);
            if (now >= lossEndsMs) return now;
        }
    }
    return 0;
}

static void RepairFallbackFixture() {
    CHECK(FirstDeliveredSend(false, 50) == 201);
    CHECK(FirstDeliveredSend(true, 50) == 99);
    CHECK(FirstDeliveredSend(false, 100) == 201);
    CHECK(FirstDeliveredSend(true, 100) == 201);
    CHECK(FirstDeliveredSend(true, 1900) != 0);
}

int main() {
    const struct { const char* name; void (*test)(); } tests[] = {
        {"rift direction", RiftDirection},
        {"rift dead zone/cap", RiftDeadZoneAndCap},
        {"rift reset", RiftReset},
        {"rift hold after stall", RiftHoldsAfterStall},
        {"rift convergence", RiftConvergesWithoutOvershoot},
        {"repair opt-in/healthy", RepairOptInAndHealthyTraffic},
        {"repair backoff/budget", RepairBackoffAndBudget},
        {"repair acknowledgments", RepairAcknowledgments},
        {"repair backpressure/wrap", RepairBackpressureAndWrap},
        {"repair rolling budget", RepairRollingBudgetUnderProgress},
        {"repair fallback fixture", RepairFallbackFixture},
    };
    for (const auto& test : tests) {
        test.test();
        std::printf("PASS %s\n", test.name);
    }
    return 0;
}
