// The rollback stress audit's comparators on synthetic memory: each must
// report a flipped byte in a field the native mementos restore, and stay
// silent for bytes they do not (engine key, unsaved node bytes).
#include <cstdint>
#include <map>
#include <vector>

#include "../common/RollbackAudit.hxx"
#include "test_support.hxx"

namespace audit = sf4e::audit;

static void TestEngineRange() {
    // Action::Engine's memento covers +8..+0xFC; +0xFC..+0x114 is its key.
    const std::vector<audit::Range> engine = { { 8, 0xFC } };
    std::vector<uint8_t> saved(0x114, 0x11), live = saved;
    CHECK(audit::Diff(saved, live, engine).empty());

    live[0xE8] ^= 0xFF;
    auto diff = audit::Diff(saved, live, engine);
    CHECK(diff.size() == 1 && diff[0].begin == 0xE8 && diff[0].end == 0xE9);
    CHECK(audit::Contains(diff, 0xE8) && !audit::Contains(diff, 0xE9));

    live = saved;
    live[0x100] ^= 0xFF; // key moved: not state
    live[4] ^= 0xFF;     // IMementoable vtable: not state
    CHECK(audit::Diff(saved, live, engine).empty());

    live = saved;
    live[8] ^= 1;
    live[0xFB] ^= 1; // both ends of the range
    diff = audit::Diff(saved, live, engine);
    CHECK(diff.size() == 2 && diff[0].begin == 8 && diff[1].begin == 0xFB);
}

static void TestMergeAndSizes() {
    const std::vector<audit::Range> all = { { 0, SIZE_MAX } };
    std::vector<uint8_t> saved(64, 0), live = saved;
    live[10] = 1;
    live[12] = 1; // within a dword: one range
    live[40] = 1; // far: its own range
    auto diff = audit::Diff(saved, live, all);
    CHECK(diff.size() == 2);
    CHECK(diff[0].begin == 10 && diff[0].end == 13);
    CHECK(diff[1].begin == 40 && diff[1].end == 41);
    CHECK(audit::Describe(diff) == " 0xa+3 0x28+1");

    // Disjoint ranges only report inside themselves.
    const std::vector<audit::Range> gaps = { { 0, 11 }, { 30, 64 } };
    diff = audit::Diff(saved, live, gaps);
    CHECK(diff.size() == 2 && diff[0].begin == 10 && diff[0].end == 11 && diff[1].begin == 40);

    // A size change is a whole-object failure.
    live.push_back(0);
    diff = audit::Diff(saved, live, all);
    CHECK(diff.size() == 1 && diff[0].begin == 0 && diff[0].end == live.size());
}

// A synthetic collision-box list: node blocks with next and data held in maps,
// as the harness reads them from +168 and +160.
struct Boxes {
    std::vector<std::vector<uint8_t>> nodes;
    std::vector<std::vector<uint8_t>> data;
    std::map<const uint8_t*, const uint8_t*> next, dataOf;

    explicit Boxes(int count) : nodes(count, std::vector<uint8_t>(184)), data(count, std::vector<uint8_t>(audit::kBoxNodeDataBytes)) {
        for (int i = 0; i < count; i++) {
            for (size_t b = 0; b < nodes[i].size(); b++) nodes[i][b] = uint8_t(i * 7 + b);
            for (size_t b = 0; b < data[i].size(); b++) data[i][b] = uint8_t(i * 3 + b);
        }
        Link();
    }
    void Link() {
        next.clear();
        dataOf.clear();
        for (size_t i = 0; i < nodes.size(); i++) {
            next[nodes[i].data()] = i + 1 < nodes.size() ? nodes[i + 1].data() : nullptr;
            dataOf[nodes[i].data()] = data[i].data();
        }
    }
    std::vector<uint8_t> Serialize() {
        std::vector<uint8_t> out;
        const int count = audit::SerializeBoxList(out, nodes.empty() ? nullptr : nodes[0].data(),
            [&](const uint8_t* n) { return next[n]; }, [&](const uint8_t* n) { return dataOf[n]; });
        CHECK(count == int(nodes.size()));
        return out;
    }
};

static void TestBoxes() {
    const std::vector<audit::Range> all = { { 0, SIZE_MAX } };
    Boxes boxes(3);
    const std::vector<uint8_t> saved = boxes.Serialize();
    CHECK(saved.size() == 4 + 3 * audit::kBoxNodeSerializedBytes);
    CHECK(audit::Diff(saved, boxes.Serialize(), all).empty());

    const size_t second = 4 + audit::kBoxNodeSerializedBytes;
    // Every saved field of a node is caught, at its stream position.
    struct Case { size_t nodeByte; size_t streamOffset; };
    const Case cases[] = {
        { 0, 0 }, { 151, 151 },
        { audit::kBoxNodeCountOffset, 152 }, { audit::kBoxNodeFlagsOffset + 3, 156 + 3 },
    };
    for (const Case& c : cases) {
        boxes.nodes[1][c.nodeByte] ^= 0xFF;
        const auto diff = audit::Diff(saved, boxes.Serialize(), all);
        CHECK(diff.size() == 1 && diff[0].begin == second + c.streamOffset);
        boxes.nodes[1][c.nodeByte] ^= 0xFF;
    }
    boxes.data[1][0x5F] ^= 0xFF;
    auto diff = audit::Diff(saved, boxes.Serialize(), all);
    CHECK(diff.size() == 1 && diff[0].begin == second + 160 + 0x5F);
    boxes.data[1][0x5F] ^= 0xFF;

    // Bytes the memento does not save (152..171, the next and data pointers)
    // do not count.
    for (size_t b = 152; b < audit::kBoxNodeCountOffset; b++) boxes.nodes[1][b] ^= 0xFF;
    CHECK(audit::Diff(saved, boxes.Serialize(), all).empty());

    // A list that gains or loses a node changes its count and its size.
    Boxes fewer(2);
    diff = audit::Diff(saved, fewer.Serialize(), all);
    CHECK(!diff.empty() && audit::Contains(diff, 0));

    // An empty list serializes to its count alone.
    Boxes none(0);
    const auto empty = none.Serialize();
    CHECK(empty.size() == 4 && empty[0] == 0);
}

static void TestTally() {
    audit::Counts counts;
    CHECK(!counts.Passed()); // nothing checked is not a pass
    counts.checked = 3;
    CHECK(counts.Passed());
    counts.missing = 1;
    CHECK(!counts.Passed());
    audit::Tally tally;
    tally.probeEngine = audit::Probe::Detected;
    const std::string summary = tally.Summary();
    CHECK(summary.find("probe_engine=detected") != std::string::npos);
    CHECK(summary.find("probe_boxes=pending") != std::string::npos);
}

int main() {
    TestEngineRange();
    TestMergeAndSizes();
    TestBoxes();
    TestTally();
    std::printf("Rollback audit comparators passed\n");
}
